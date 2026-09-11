#include <rstd/test/gtest.hpp>

import eigen;
import rstd.cppstd;
import wescene.fs;
import wescene.pkg_fs;
import wescene.pkg.parse;
import wescene.scene;
import wescene.spec_names;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

namespace
{

std::uint32_t MaxMeshIndex(const owe::Mdl::Mesh& mesh) {
    std::uint32_t max_index = 0;
    for (const auto& tri : mesh.indices) {
        for (std::uint32_t idx : tri) max_index = std::max(max_index, idx);
    }
    return max_index;
}

std::uint32_t CountUvSeamTriangles(const owe::Mdl::Mesh& mesh) {
    std::uint32_t seam_triangles = 0;
    for (const auto& tri : mesh.indices) {
        float min_u = std::numeric_limits<float>::max();
        float max_u = std::numeric_limits<float>::lowest();
        for (std::uint32_t idx : tri) {
            min_u = std::min(min_u, mesh.texcoords[usize(idx)][usize(0)]);
            max_u = std::max(max_u, mesh.texcoords[usize(idx)][usize(0)]);
        }
        if (max_u - min_u > 0.5f) ++seam_triangles;
    }
    return seam_triangles;
}

} // namespace

TEST(Puppet, ArcOwnedLayerExposesBorrowedTransforms) {
    auto              puppet = Arc<owe::Puppet>::make();
    owe::Puppet::Bone bone;
    bone.name = String::make("root"_str);
    puppet->bones.push(rstd::move(bone));
    puppet->prepared();

    owe::PuppetLayer layer(puppet.clone());
    layer.prepared(slice<owe::PuppetLayer::AnimationLayer> {});

    EXPECT_EQ(layer.boneIndex("root"_str), 1u);
    EXPECT_EQ(layer.boneIndex("missing"_str), 0u);
    EXPECT_TRUE(layer.boneTransform(0u, 0.0).is_none());
    auto transform = layer.boneTransform(1u, 0.0);
    ASSERT_TRUE(transform.is_some());
    EXPECT_TRUE(transform->matrix().isApprox(Eigen::Matrix4f::Identity()));
}

TEST(Puppet, SamplesTextureChannelBlendMapFromAnimationPlayback) {
    auto  puppet     = Arc<owe::Puppet>::make();
    auto& animation  = puppet->anims.emplace_back();
    animation.id     = 781;
    animation.name   = String::make("Arona Drool"_str);
    animation.mode   = owe::Puppet::PlayMode::Single;
    animation.fps    = 1.0f;
    animation.length = 2;
    auto& channels   = animation.trans.insert(owe::Puppet::AnimTrans {});
    channels.main_track.push(0.0f);
    channels.main_track.push(1.0f);
    channels.main_track.push(0.0f);
    auto& second = channels.tail_tracks.emplace_back();
    second.push(1.0f);
    second.push(0.5f);
    second.push(0.0f);
    puppet->prepared();

    owe::PuppetLayer                 layer(puppet.clone());
    owe::PuppetLayer::AnimationLayer authored {
        .id      = 781,
        .visible = true,
        .name    = String::make("Arona Drool"_str),
    };
    layer.prepared(slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(&authored, usize(1)));
    ASSERT_EQ(layer.AnimationPlaybacks().len(), usize(1));
    layer.AnimationPlaybacks()[usize()]->SetFrame(i32(1));
    layer.AnimationPlaybacks()[usize()]->Pause();

    const auto blend_map = layer.TextureChannelBlendMap(0.0);
    ASSERT_EQ(blend_map.len(), usize(4));
    EXPECT_FLOAT_EQ(blend_map[usize()], 1.0f);
    EXPECT_FLOAT_EQ(blend_map[usize(1)], 0.5f);
    EXPECT_FLOAT_EQ(blend_map[usize(2)], 0.0f);
    EXPECT_FLOAT_EQ(blend_map[usize(3)], 0.0f);
}

TEST(Puppet, AdditiveUsesSeparateAnimationReference) {
    auto  puppet                  = Arc<owe::Puppet>::make();
    auto& bone                    = puppet->bones.emplace_back();
    bone.local_bind.translation() = Eigen::Vector3f(100.0f, 200.0f, 0.0f);
    Eigen::Affine3f reference     = Eigen::Affine3f::Identity();
    reference.translate(Eigen::Vector3f(10.0f, 20.0f, 0.0f));
    reference.rotate(Eigen::AngleAxisf(0.25f, Eigen::Vector3f::UnitZ()));
    reference.scale(Eigen::Vector3f(2.0f, 3.0f, 1.0f));
    bone.animation_reference.insert(Eigen::Affine3f(reference));
    for (int id : { 1, 2 }) {
        auto& animation  = puppet->anims.emplace_back();
        animation.id     = id;
        animation.fps    = 1.0f;
        animation.length = 1;
        animation.mode   = owe::Puppet::PlayMode::Single;
        auto& track      = animation.bone_tracks.emplace_back();
        for (int frame : { 0, 1 }) {
            track.frames.push(owe::Puppet::BoneFrame {
                .position = Eigen::Vector3f(10.0f + (id == 2 ? 4.0f * frame : 0.0f), 20.0f, 0.0f),
                .angle    = Eigen::Vector3f(0.0f, 0.0f, 0.25f),
                .scale    = Eigen::Vector3f(2.0f, 3.0f, 1.0f),
            });
        }
    }
    puppet->prepared();
    owe::PuppetLayer                 layer(puppet.clone());
    owe::PuppetLayer::AnimationLayer authored[] = {
        { .id = 1, .blend = 1.0, .visible = true },
        { .id = 2, .blend = 0.5, .visible = true, .additive = true },
    };
    layer.prepared(slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(authored, usize(2)));
    for (const auto& playback : layer.AnimationPlaybacks()) playback->Pause();
    auto expected = reference * bone.inv_bind;
    EXPECT_TRUE(layer.genFrame(0.0)[usize()].matrix().isApprox(expected.matrix(), 0.0001f));
    layer.AnimationPlaybacks()[usize(1)]->SetFrame(i32(1));
    expected.pretranslate(Eigen::Vector3f(2.0f, 0.0f, 0.0f));
    EXPECT_TRUE(layer.genFrame(0.0)[usize()].matrix().isApprox(expected.matrix(), 0.0001f));
}

TEST(Puppet, LegacyAdditiveOnlyStackUsesEachClipsFirstFrame) {
    auto puppet                                           = Arc<owe::Puppet>::make();
    puppet->additive_uses_first_frame                     = true;
    puppet->bones.emplace_back().local_bind.translation() = Eigen::Vector3f(100.0f, 0.0f, 0.0f);
    for (int id : { 1, 2 }) {
        auto& animation  = puppet->anims.emplace_back();
        animation.id     = id;
        animation.fps    = 1.0f;
        animation.length = 1;
        animation.mode   = owe::Puppet::PlayMode::Single;
        auto& track      = animation.bone_tracks.emplace_back();
        for (int frame : { 0, 1 }) {
            track.frames.push(owe::Puppet::BoneFrame {
                .position = Eigen::Vector3f(10.0f * id + 4.0f * frame, 0.0f, 0.0f),
                .angle    = Eigen::Vector3f::Zero(),
                .scale    = Eigen::Vector3f::Ones(),
            });
        }
    }
    puppet->prepared();
    owe::PuppetLayer                 layer(puppet.clone());
    owe::PuppetLayer::AnimationLayer authored[] = {
        { .id = 1, .blend = 1.0, .visible = true, .additive = true },
        { .id = 2, .blend = 0.5, .visible = true, .additive = true },
    };
    layer.prepared(slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(authored, usize(2)));
    for (const auto& playback : layer.AnimationPlaybacks()) playback->Pause();
    EXPECT_FLOAT_EQ(layer.genFrame(0.0)[usize()].translation().x(), -90.0f);
    layer.AnimationPlaybacks()[usize(1)]->SetFrame(i32(1));
    EXPECT_FLOAT_EQ(layer.genFrame(0.0)[usize()].translation().x(), -88.0f);
}

TEST(MdlMesh, LegacyMissingReferenceSelectsFirstFrameDeltas) {
    const auto pkg_path =
        std::filesystem::path(WAYWALLEN_WORKSHOP_DIR) / "2907385672" / "scene.pkg";
    if (! std::filesystem::exists(pkg_path)) GTEST_SKIP() << "workshop 2907385672 is not available";
    owe::fs::VFS vfs;
    auto         pkg_fs = owe::fs::WPPkgFs::open(owe::fs::ToPath(pkg_path.string()));
    ASSERT_TRUE(pkg_fs.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, pkg_fs->mount_handle()).is_ok());
    owe::Mdl mdl;
    ASSERT_TRUE(owe::MdlParser::Parse("models/hu tao bos 3_puppet.mdl"_str, vfs, mdl));
    ASSERT_TRUE(mdl.puppet.is_some());
    ASSERT_EQ(mdl.mdls, 2);
    EXPECT_TRUE((*mdl.puppet)->additive_uses_first_frame);
    EXPECT_TRUE((*mdl.puppet)->bones[usize(3)].animation_reference.is_none());
}

TEST(MdlMesh, ReadsLegacyAnimationReferenceWithoutReplacingMeshBind) {
    const auto pkg_path =
        std::filesystem::path(WAYWALLEN_WORKSHOP_DIR) / "3148125112" / "scene.pkg";
    if (! std::filesystem::exists(pkg_path)) GTEST_SKIP() << "workshop 3148125112 is not available";
    owe::fs::VFS vfs;
    auto         pkg_fs = owe::fs::WPPkgFs::open(owe::fs::ToPath(pkg_path.string()));
    ASSERT_TRUE(pkg_fs.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, pkg_fs->mount_handle()).is_ok());
    owe::Mdl mdl;
    ASSERT_TRUE(owe::MdlParser::Parse("models/人物2_puppet.mdl"_str, vfs, mdl));
    ASSERT_TRUE(mdl.puppet.is_some());
    ASSERT_EQ(mdl.mdls, 2);
    EXPECT_FALSE((*mdl.puppet)->additive_uses_first_frame);
    const auto& bone = (*mdl.puppet)->bones[usize(3)];
    ASSERT_TRUE(bone.animation_reference.is_some());
    EXPECT_FLOAT_EQ(bone.local_bind.translation().x(), 1627.5517578125f);
    EXPECT_FLOAT_EQ(bone.animation_reference->translation().x(), 318.3017578125f);
}

TEST(Puppet, SortCurvesDoNotScaleBoneTransforms) {
    for (float scalar : { 0.0f, -6100.0f, 9500.0f }) {
        auto puppet = Arc<owe::Puppet>::make();
        puppet->bones.emplace_back();
        auto& animation  = puppet->anims.emplace_back();
        animation.id     = 688;
        animation.mode   = owe::Puppet::PlayMode::Single;
        animation.fps    = 1.0f;
        animation.length = 1;
        auto& track      = animation.bone_tracks.emplace_back();
        track.frames.push(owe::Puppet::BoneFrame {
            .position = Eigen::Vector3f::Zero(),
            .angle    = Eigen::Vector3f::Zero(),
            .scale    = Eigen::Vector3f::Ones(),
        });
        track.frames.push(owe::Puppet::BoneFrame {
            .position = Eigen::Vector3f(4.0f, 0.0f, 0.0f),
            .angle    = Eigen::Vector3f::Zero(),
            .scale    = Eigen::Vector3f(1.0f, 0.5f, 1.0f),
        });
        auto& curve = animation.scalar_curves.emplace_back();
        curve.values.emplace_back(scalar);
        curve.values.emplace_back(scalar);
        puppet->prepared();
        owe::PuppetLayer                 layer(puppet.clone());
        owe::PuppetLayer::AnimationLayer authored {
            .id       = 688,
            .blend    = 0.5,
            .visible  = true,
            .additive = true,
        };
        layer.prepared(
            slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(&authored, usize(1)));
        layer.AnimationPlaybacks()[usize()]->SetFrame(i32(1));
        layer.AnimationPlaybacks()[usize()]->Pause();
        auto matrices = layer.genFrame(0.0);
        ASSERT_EQ(matrices.len(), usize(1));
        EXPECT_FLOAT_EQ(matrices[usize()].translation().x(), 2.0f);
        EXPECT_FLOAT_EQ(matrices[usize()].linear()(1, 1), 0.75f);
    }
}

TEST(Puppet, DrawOrderUsesPlaybackAndStableIntegerKeys) {
    auto puppet = Arc<owe::Puppet>::make();
    for (int depth : { 100, 200, 300 }) puppet->bones.emplace_back().draw_order = depth;
    auto& animation  = puppet->anims.emplace_back();
    animation.id     = 42;
    animation.mode   = owe::Puppet::PlayMode::Single;
    animation.fps    = 1.0f;
    animation.length = 1;
    for (float target : { 500.0f, -200.0f, 100.0f }) {
        auto& curve = animation.scalar_curves.emplace_back();
        curve.values.push(0.0f);
        curve.values.emplace_back(target);
    }
    puppet->prepared();
    owe::PuppetLayer                 layer(puppet.clone());
    owe::PuppetLayer::AnimationLayer authored { .id = 42, .blend = 0.5, .visible = true };
    layer.prepared(slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(&authored, usize(1)));
    const owe::PuppetLayer::PartOrder parts[] = { { u32(0), i32(0) },
                                                  { u32(1), i32(0) },
                                                  { u32(2), i32(0) } };
    const auto input = slice<owe::PuppetLayer::PartOrder>::from_raw_parts(parts, usize(3));
    layer.AnimationPlaybacks()[usize()]->Pause();
    layer.AnimationPlaybacks()[usize()]->SetFrame(i32(1));
    auto order = layer.DrawOrder(input);
    ASSERT_EQ(order.len(), usize(3));
    EXPECT_EQ(order[usize(0)], usize(1));
    EXPECT_EQ(order[usize(1)], usize(2));
    EXPECT_EQ(order[usize(2)], usize(0));
    layer.AnimationPlaybacks()[usize()]->SetFrame(i32(0));
    order = layer.DrawOrder(input);
    EXPECT_EQ(order[usize(0)], usize(0));
    EXPECT_EQ(order[usize(1)], usize(1));
    EXPECT_EQ(order[usize(2)], usize(2));
}

TEST(Puppet, DrawOrderKeepsTiesAndAppliesPartOffsets) {
    auto puppet                             = Arc<owe::Puppet>::make();
    puppet->bones.emplace_back().draw_order = 100;
    puppet->prepared();
    owe::PuppetLayer layer(puppet.clone());
    layer.prepared(slice<owe::PuppetLayer::AnimationLayer> {});
    const owe::PuppetLayer::PartOrder parts[] = { { u32(0), i32(0) },
                                                  { u32(0), i32(-200) },
                                                  { u32(0), i32(0) } };
    const auto                        order =
        layer.DrawOrder(slice<owe::PuppetLayer::PartOrder>::from_raw_parts(parts, usize(3)));
    EXPECT_EQ(order[usize(0)], usize(1));
    EXPECT_EQ(order[usize(1)], usize(0));
    EXPECT_EQ(order[usize(2)], usize(2));
}

TEST(Puppet, DrawOrderBoundsAdditiveAgainstCurrentAndSample) {
    auto puppet                             = Arc<owe::Puppet>::make();
    puppet->bones.emplace_back().draw_order = 100;
    puppet->bones.emplace_back().draw_order = 100;
    for (int id : { 1, 2 }) {
        auto& animation  = puppet->anims.emplace_back();
        animation.id     = id;
        animation.fps    = 1.0f;
        animation.length = 1;
        animation.scalar_curves.emplace_back().values.push(id == 1 ? 300.0f : 150.0f);
        animation.scalar_curves.emplace_back().values.push(id == 1 ? 200.0f : 325.0f);
    }
    puppet->prepared();
    owe::PuppetLayer                 layer(puppet.clone());
    owe::PuppetLayer::AnimationLayer authored[] = {
        { .id = 1, .blend = 1.0, .visible = true },
        { .id = 2, .blend = 1.0, .visible = true, .additive = true },
    };
    layer.prepared(slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(authored, usize(2)));
    const owe::PuppetLayer::PartOrder parts[] = { { u32(0), i32(0) }, { u32(1), i32(0) } };
    const auto                        order =
        layer.DrawOrder(slice<owe::PuppetLayer::PartOrder>::from_raw_parts(parts, usize(2)));
    EXPECT_EQ(order[usize(0)], usize(0));
    EXPECT_EQ(order[usize(1)], usize(1));
}

TEST(MdlMesh, DrawOrderWithoutAnimationPreservesFileOrder) {
    auto puppet = Arc<owe::Puppet>::make();
    puppet->prepared();
    auto layer = Arc<owe::PuppetLayer>::make(puppet.clone());
    layer->prepared(slice<owe::PuppetLayer::AnimationLayer> {});
    owe::Mdl::Mesh source;
    source.parts.push(owe::Mdl::Mesh::Part { 2, 0, 3 });
    owe::SceneMesh::Submesh submesh;
    submesh.draw_ranges.push_back({ u32(0), u32(3) });
    owe::MdlParser::BindDrawOrder(submesh, source, layer.clone());
    EXPECT_TRUE(submesh.draw_range_order.is_none());
}

TEST(MdlMesh, DrawOrderOwnsFilteredPartMapping) {
    auto puppet = Arc<owe::Puppet>::make();
    for (int depth : { 100, 200, 300 }) puppet->bones.emplace_back().draw_order = depth;
    puppet->anims.emplace_back().scalar_curves.emplace_back();
    puppet->prepared();
    auto layer = Arc<owe::PuppetLayer>::make(puppet.clone());
    layer->prepared(slice<owe::PuppetLayer::AnimationLayer> {});
    owe::SceneMesh::Submesh submesh;
    {
        owe::Mdl::Mesh source;
        source.parts.push(owe::Mdl::Mesh::Part { 2, 0, 3 });
        source.parts.push(owe::Mdl::Mesh::Part { 1, 3, 3 });
        source.parts.push(owe::Mdl::Mesh::Part { 0, 6, 3 });
        submesh.draw_ranges.push_back({ u32(0), u32(3) });
        submesh.draw_ranges.push_back({ u32(6), u32(3) });
        owe::MdlParser::BindDrawOrder(submesh, source, layer.clone());
    }
    ASSERT_TRUE(submesh.draw_range_order.is_some());
    const auto order = (*submesh.draw_range_order)->operator()();
    ASSERT_EQ(order.len(), usize(2));
    EXPECT_EQ(order[usize(0)], usize(1));
    EXPECT_EQ(order[usize(1)], usize(0));
}

TEST(MdlMesh, KeepsPuppetPositionsInMdlLocalSpace) {
    owe::Mdl::Mesh source;
    source.positions.push(array<float, 3> { 244.0f, 349.5f, 0.0f });
    source.texcoords.push(array<float, 2> { 0.25f, 0.75f });
    source.indices.push(array<std::uint32_t, 3> { 0u, 0u, 0u });

    owe::SceneMesh::Submesh submesh;
    owe::MdlParser::GenMeshFromMdl(submesh, source);

    ASSERT_EQ(submesh.vertex_arrays.size(), 1u);
    const auto& vertices = submesh.vertex_arrays.front();
    ASSERT_NE(vertices.Data(), nullptr);
    EXPECT_FLOAT_EQ(vertices.Data()[0], 244.0f);
    EXPECT_FLOAT_EQ(vertices.Data()[1], 349.5f);
    EXPECT_FLOAT_EQ(vertices.Data()[2], 0.0f);
}

TEST(MdlMesh, FindsMeshByNormalizedMaterialReference) {
    owe::Mdl mdl;
    mdl.meshes.push(owe::Mdl::Mesh {});
    mdl.meshes.push(owe::Mdl::Mesh {});
    mdl.meshes[usize()].mat_json_files.push(String::make("materials/main.json"_str));
    mdl.meshes[usize(1)].mat_json_files.push(String::make("overlay"_str));

    auto main = owe::MdlParser::FindMeshByMaterial(mdl, "materials/main"_str);
    ASSERT_TRUE(main.is_some());
    EXPECT_EQ(*main, usize());

    auto overlay = owe::MdlParser::FindMeshByMaterial(mdl, "materials/overlay.json"_str);
    ASSERT_TRUE(overlay.is_some());
    EXPECT_EQ(*overlay, usize(1));
    EXPECT_TRUE(owe::MdlParser::FindMeshByMaterial(mdl, "missing"_str).is_none());
}

TEST(MdlMesh, Mdlv23LargeStaticMeshUsesUint32GlobalIndices) {
    const std::filesystem::path pkg_path =
        std::filesystem::path(WAYWALLEN_WORKSHOP_DIR) / "3557068717" / "scene.pkg";
    if (! std::filesystem::exists(pkg_path)) {
        GTEST_SKIP() << "workshop 3557068717 is not available";
    }

    owe::fs::VFS vfs;
    auto         assets_fs = owe::fs::make_physical_fs(owe::fs::ToPath(WAYWALLEN_ASSETS_DIR));
    if (assets_fs.is_ok()) {
        ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets_fs).unwrap_unchecked()).is_ok());
    }
    auto pkg_fs = owe::fs::WPPkgFs::open(owe::fs::ToPath(pkg_path.string()));
    ASSERT_TRUE(pkg_fs.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, pkg_fs->mount_handle()).is_ok());

    owe::Mdl mdl;
    ASSERT_TRUE(owe::MdlParser::Parse("models/球体01/球体01.mdl"_str, vfs, mdl));
    ASSERT_FALSE(mdl.meshes.is_empty());

    const auto& mesh = mdl.meshes[usize()];
    ASSERT_EQ(mdl.header.mdlv, 23);
    ASSERT_EQ(mesh.positions.len(), usize(520192));
    ASSERT_EQ(mesh.texcoords.len(), mesh.positions.len());
    ASSERT_EQ(mesh.indices.len(), usize(260096));
    ASSERT_LT(MaxMeshIndex(mesh), mesh.positions.len().to_primitive());
    EXPECT_EQ(mesh.indices[usize(0)], (array<std::uint32_t, 3> { 0u, 1u, 2u }));
    EXPECT_EQ(mesh.indices[usize(1)], (array<std::uint32_t, 3> { 0u, 2u, 3u }));
    EXPECT_EQ(mesh.indices[mesh.indices.len() - usize(1)],
              (array<std::uint32_t, 3> { 520188u, 520190u, 520191u }));
    EXPECT_EQ(CountUvSeamTriangles(mesh), 0u);

    owe::SceneMesh::Submesh submesh;
    owe::MdlParser::GenMeshFromMdl(submesh, mesh);
    ASSERT_EQ(submesh.vertex_arrays.size(), 1u);
    ASSERT_EQ(submesh.index_arrays.size(), 1u);
    EXPECT_TRUE(submesh.draw_ranges.empty());

    const auto& index_array = submesh.index_arrays.front();
    ASSERT_EQ(index_array.DataCount(), rstd::usize(780288));
    EXPECT_EQ(index_array.Data()[0], 0u);
    EXPECT_EQ(index_array.Data()[1], 1u);
    EXPECT_EQ(index_array.Data()[2], 2u);
    EXPECT_EQ(index_array.Data()[780285], 520188u);
    EXPECT_EQ(index_array.Data()[780286], 520190u);
    EXPECT_EQ(index_array.Data()[780287], 520191u);
}

TEST(MdlMesh, Mdlv23ReadsPerMeshMaterialSkins) {
    const std::filesystem::path pkg_path =
        std::filesystem::path(WAYWALLEN_WORKSHOP_DIR) / "1979606285" / "scene.pkg";
    if (! std::filesystem::exists(pkg_path)) {
        GTEST_SKIP() << "workshop 1979606285 is not available";
    }

    owe::fs::VFS vfs;
    auto         assets_fs = owe::fs::make_physical_fs(owe::fs::ToPath(WAYWALLEN_ASSETS_DIR));
    if (assets_fs.is_ok()) {
        ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets_fs).unwrap_unchecked()).is_ok());
    }
    auto pkg_fs = owe::fs::WPPkgFs::open(owe::fs::ToPath(pkg_path.string()));
    ASSERT_TRUE(pkg_fs.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, pkg_fs->mount_handle()).is_ok());

    owe::Mdl mdl;
    ASSERT_TRUE(owe::MdlParser::Parse("models/prism/prism.mdl"_str, vfs, mdl));
    ASSERT_EQ(mdl.header.mdlv, 23);
    ASSERT_EQ(mdl.header.skin_count, 2u);
    ASSERT_EQ(mdl.header.mesh_count, 1u);
    ASSERT_EQ(mdl.meshes.len(), usize(1));

    const auto& mesh = mdl.meshes[usize()];
    ASSERT_EQ(mesh.mat_json_files.len(), usize(2));
    EXPECT_EQ(mesh.mat_json_files[usize()].as_str(), "materials/prism/prism.json"_str);
    EXPECT_EQ(mesh.mat_json_files[usize(1)].as_str(), "materials/prism/prism_main.json"_str);
    EXPECT_EQ(mesh.positions.len(), usize(60));
    EXPECT_EQ(mesh.indices.len(), usize(32));
}

TEST(MdlPuppet, Mdlv23ReadsMultiCurveMorphEvents) {
    const std::filesystem::path pkg_path =
        std::filesystem::path(WAYWALLEN_WORKSHOP_DIR) / "3686252018" / "scene.pkg";
    if (! std::filesystem::exists(pkg_path)) {
        GTEST_SKIP() << "workshop 3686252018 is not available";
    }

    owe::fs::VFS vfs;
    auto         assets_fs = owe::fs::make_physical_fs(owe::fs::ToPath(WAYWALLEN_ASSETS_DIR));
    if (assets_fs.is_ok()) {
        ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets_fs).unwrap_unchecked()).is_ok());
    }
    auto pkg_fs = owe::fs::WPPkgFs::open(owe::fs::ToPath(pkg_path.string()));
    ASSERT_TRUE(pkg_fs.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, pkg_fs->mount_handle()).is_ok());

    owe::Mdl mdl;
    ASSERT_TRUE(owe::MdlParser::Parse("models/sheet_puppet.mdl"_str, vfs, mdl));
    ASSERT_EQ(mdl.mdla, 6);
    ASSERT_TRUE(mdl.puppet.is_some());

    const auto& anims = (*mdl.puppet)->anims;
    ASSERT_EQ(anims.len(), usize(17));
    const auto& left_eye = anims[usize()];
    ASSERT_EQ(left_eye.name.as_str(), "Left eye"_str);
    ASSERT_EQ(left_eye.v4_events.len(), usize(1));

    const auto& event = left_eye.v4_events[usize()];
    EXPECT_EQ(event.flags, 0);
    ASSERT_EQ(event.curves.len(), usize(6));
    for (usize i {}; i < event.curves.len(); ++i) {
        const auto& curve = event.curves[i];
        EXPECT_EQ(curve.id, i.to_primitive());
        ASSERT_EQ(curve.values.len(), usize(211));
        EXPECT_FLOAT_EQ(curve.values[usize()], 1.0f);
    }

    ASSERT_EQ(mdl.morph_sections.len(), usize(1));
    EXPECT_FLOAT_EQ(mdl.morph_sections[usize()].event_time, event.time);
    EXPECT_EQ(mdl.morph_sections[usize()].sections.len(), event.curves.len());
}
