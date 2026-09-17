module;

module wescene.vulkan;
import wescene.types;
import rstd.cppstd;

namespace owe
{
namespace vulkan
{

AllocatedBufferParameters::AllocatedBufferParameters()  = default;
AllocatedBufferParameters::~AllocatedBufferParameters() = default;
AllocatedBufferParameters::AllocatedBufferParameters(AllocatedBufferParameters&& o) noexcept
    : handle(std::move(o.handle)), req_size(o.req_size) {}
AllocatedBufferParameters&
AllocatedBufferParameters::operator=(AllocatedBufferParameters&& o) noexcept {
    handle   = std::move(o.handle);
    req_size = o.req_size;
    return *this;
}

AllocatedImageParameters::AllocatedImageParameters()  = default;
AllocatedImageParameters::~AllocatedImageParameters() = default;
AllocatedImageParameters::AllocatedImageParameters(AllocatedImageParameters&& o) noexcept
    : handle(std::move(o.handle)),
      view(std::move(o.view)),
      sampler(std::move(o.sampler)),
      extent(o.extent),
      mipmap_level(o.mipmap_level),
      generation(o.generation) {}
AllocatedImageParameters&
AllocatedImageParameters::operator=(AllocatedImageParameters&& o) noexcept {
    if (this == &o) return *this;
    view.reset();
    sampler.reset();
    handle       = std::move(o.handle);
    view         = std::move(o.view);
    sampler      = std::move(o.sampler);
    extent       = o.extent;
    mipmap_level = o.mipmap_level;
    generation   = o.generation;
    return *this;
}

ExImageParameters::ExImageParameters()  = default;
ExImageParameters::~ExImageParameters() = default;
ExImageParameters::ExImageParameters(ExImageParameters&& o) noexcept
    : mem(std::move(o.mem)),
      mem_reqs(o.mem_reqs),
      handle(std::move(o.handle)),
      view(std::move(o.view)),
      sampler(std::move(o.sampler)),
      extent(o.extent),
      mipmap_level(o.mipmap_level),
      generation(o.generation),
      fd(std::exchange(o.fd, 0)),
      drm_fourcc(o.drm_fourcc),
      drm_modifier(o.drm_modifier),
      plane0_offset(o.plane0_offset),
      plane0_stride(o.plane0_stride) {}
ExImageParameters& ExImageParameters::operator=(ExImageParameters&& o) noexcept {
    mem           = std::move(o.mem);
    mem_reqs      = o.mem_reqs;
    handle        = std::move(o.handle);
    view          = std::move(o.view);
    sampler       = std::move(o.sampler);
    extent        = o.extent;
    mipmap_level  = o.mipmap_level;
    generation    = o.generation;
    fd            = std::exchange(o.fd, 0);
    drm_fourcc    = o.drm_fourcc;
    drm_modifier  = o.drm_modifier;
    plane0_offset = o.plane0_offset;
    plane0_stride = o.plane0_stride;
    return *this;
}

ImageSlots::ImageSlots()  = default;
ImageSlots::~ImageSlots() = default;
ImageSlots::ImageSlots(ImageSlots&& o) noexcept: slots(std::move(o.slots)) {}
ImageSlots& ImageSlots::operator=(ImageSlots&& o) noexcept {
    slots = std::move(o.slots);
    return *this;
}

ImageSlotsRef::ImageSlotsRef()  = default;
ImageSlotsRef::~ImageSlotsRef() = default;
ImageSlotsRef::ImageSlotsRef(const ImageSlots& o) {
    slots.reserve(o.slots.size());
    for (const auto& image : o.slots) slots.push_back(ToImageParameters(image));
}

} // namespace vulkan
} // namespace owe
