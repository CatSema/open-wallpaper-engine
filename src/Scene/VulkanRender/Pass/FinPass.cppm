module;

export module wescene.vulkan_render:fin_pass;
import wescene.spec_names;
import rstd.cppstd;
import wescene.vulkan;
import wescene.scene;

import :vulkan_pass;
import :resource;

export namespace owe::vulkan
{

// Final pass: present the scene render target into the frame surface. Window
// surfaces use a fullscreen graphics pass (the path supported reliably by
// MoltenVK); offscreen ExSwapchain surfaces retain the transfer path used by
// the external image protocol.
class FinPass : public VulkanPass {
public:
    struct Desc {
        // in
        std::string_view result { rstd::cppstd::as_string_view(SpecTex_Default) }; // scene RT key
        rstd::Option<TextureRequest>                    result_request;
        rstd::Option<resource::TextureUseHandle>        result_use;
        rstd::Option<resource::ExternalUseHandle>       external_use;
        rstd::Option<resource::PipelineUseHandle>       pipeline_use;
        rstd::Option<resource::RenderPassUseHandle>     render_pass_use;
        rstd::Option<resource::DescriptorBindingHandle> descriptor_use;
        resource_registry::PreparedBarrierBatch         result_barrier;
    };

    FinPass(Desc&&);
    virtual ~FinPass();

    bool             setFrameSurface(owe::FrameSurfaceLease,
                                     rstd::mut_ref<rstd::dyn<resource_registry::ExternalResourcePreparer>>,
                                     const DeviceCapabilities&, rstd::uint32_t graphics_queue_family);
    void             setPresentFormat(VkFormat);
    bool             setResultRequest(rstd::Option<TextureRequest>);
    void             resetResourceUses();
    void             declareResources(ResourceDeclarationContext&) override;
    PassResourceUses resourceUses() const override;
    auto             pipelineLayoutRequirement(const PreparedPassResources&) const
        -> Result<Option<PipelineLayoutRequirement>, resource::ResourceError> override;
    std::vector<PassTextureRequestDiagnostic> textureRequestDiagnostics() const override;
    bool                                      prepareResourceStates(
        rstd::mut_ref<rstd::dyn<resource_registry::TextureStatePreparer>>) override;

    void prepare(Scene&, const Device&, PassPrepareContext&) override;
    void record(PassRecordContext&) override;
    void destory(const Device&) override;

private:
    bool ensurePresentFramebuffer(const owe::FrameSurfaceLease&);

    Desc             m_desc;
    const Device*    m_device { nullptr };
    VkFormat         m_present_format { VK_FORMAT_UNDEFINED };
    VkRenderPass     m_present_render_pass { VK_NULL_HANDLE };
    vvk::Framebuffer m_present_framebuffer;
    VkImageView      m_present_framebuffer_view { VK_NULL_HANDLE };
    VkExtent2D       m_present_framebuffer_extent {};
    bool             m_graphics_path { false };
    bool             m_frame_graphics_path { false };
    bool             m_path_logged { false };
};

} // namespace owe::vulkan
