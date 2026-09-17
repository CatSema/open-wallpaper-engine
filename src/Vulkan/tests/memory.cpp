#include <vulkan/vulkan.h>
#include <rstd/test/gtest.hpp>
import rstd;
import rstd.cppstd;
import wescene.vulkan;
import wescene.types;
import wavsen.video;
using namespace rstd::prelude;
using namespace owe::vulkan;
namespace
{
using rstd::sync::atomic::Ordering;
rstd::sync::atomic::Atomic<u32> errors { u32() };
VKAPI_ATTR VkBool32 VKAPI_CALL  OnDebug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                        VkDebugUtilsMessageTypeFlagsEXT,
                                        const VkDebugUtilsMessengerCallbackDataEXT*, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        errors.fetch_add(u32(1), Ordering::Relaxed);
    return VK_FALSE;
}
struct Context {
    Instance                 instance;
    vvk::DebugUtilsMessenger debug;
    Device                   device;
    bool                     initialize() {
        const InstanceLayer layers[] { { false, "VK_LAYER_KHRONOS_validation" } };
        if (! Instance::Create(instance, {}, layers)) return false;
        debug = instance.inst().CreateDebugUtilsMessenger({
            .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
            .pfnUserCallback = OnDebug,
        });
        const Extension extensions[] { { true, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME },
                                       { false, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME },
                                       { false, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME },
                                       { false, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME } };
        if (! instance.ChoosePhysicalDevice([&](const vvk::PhysicalDevice& gpu) {
                return Device::CheckGPU(gpu, extensions, VK_NULL_HANDLE);
            }))
            return false;
        return Device::Create(instance, extensions, { 64, 64 }, device);
    }
};
struct Submission {
    const Device&                  device;
    vvk::CommandBuffers            commands;
    vvk::CommandBuffer             command;
    vvk::Fence                     fence;
    Option<BufferUploadBatchLease> uploads;
    bool                           submitted {};
    ~Submission() {
        if (submitted) (void)device.handle().WaitIdle();
    }
    bool begin() {
        if (device.cmd_pool().Allocate(usize(1), VK_COMMAND_BUFFER_LEVEL_PRIMARY, commands) !=
            VK_SUCCESS)
            return false;
        command = vvk::CommandBuffer(commands[usize()], device.handle().Dispatch());
        return command.Begin({ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }) ==
               VK_SUCCESS;
    }
    bool submit() {
        if (command.End() != VK_SUCCESS) return false;
        if (device.handle().CreateFence({ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }, fence) !=
            VK_SUCCESS)
            return false;
        VkSubmitInfo info { .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .commandBufferCount = 1,
                            .pCommandBuffers    = command.address() };
        submitted = device.graphics_queue().handle.Submit(info, *fence) == VK_SUCCESS;
        return submitted;
    }
};
} // namespace
TEST(OweMemory, BufferPagesUploadsAndCompletionLeases) {
    errors.store(u32(), Ordering::Relaxed);
    {
        Context context;
        ASSERT_TRUE(context.initialize());
        auto&         device = context.device;
        BufferManager manager(device);
        ASSERT_TRUE(manager.init());
        auto first  = manager.Allocate({ 256, 256, BufferUploadClass::Transfer });
        auto second = manager.Allocate({ 256, 256, BufferUploadClass::Transfer });
        ASSERT_TRUE(first.is_some());
        ASSERT_TRUE(second.is_some());
        auto a = first.unwrap_unchecked(), b = second.unwrap_unchecked();
        EXPECT_EQ(a.buffer(), b.buffer());
        EXPECT_NE(a.offset(), b.offset());
        const auto released = a.offset();
        a                   = BufferAllocation {};
        auto reused         = manager.Allocate({ 256, 256, BufferUploadClass::Transfer });
        ASSERT_TRUE(reused.is_some());
        a = reused.unwrap_unchecked();
        EXPECT_EQ(a.offset(), released);
        rstd::uint8_t bytes[256];
        for (unsigned i = 0; i < 256; ++i) bytes[i] = static_cast<rstd::uint8_t>(i ^ 0xa5);
        ASSERT_TRUE(manager.QueueWrite(a, bytes).is_some());
        VkBufferCreateInfo info { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size  = 256,
                                  .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
        auto made = device.memory_allocator().create_buffer(info, vvk::MemoryRequest::Readback());
        ASSERT_TRUE(made.is_ok());
        auto       readback = made.unwrap_unchecked();
        Submission submission { device };
        ASSERT_TRUE(submission.begin());
        RecordedBufferUploads recorded;
        ASSERT_TRUE(manager.RecordPendingUploads(submission.command, recorded));
        VkBufferCopy copy { a.offset(), 0, 256 };
        const auto&  d = device.handle().Dispatch();
        d.vkCmdCopyBuffer(*submission.command, a.buffer(), readback.handle(), 1, &copy);
        VkBufferMemoryBarrier barrier { .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .buffer              = readback.handle(),
                                        .size                = VK_WHOLE_SIZE };
        d.vkCmdPipelineBarrier(*submission.command,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_HOST_BIT,
                               0,
                               0,
                               nullptr,
                               1,
                               &barrier,
                               0,
                               nullptr);
        submission.uploads = manager.CommitRecordedUploads(rstd::move(recorded));
        ASSERT_TRUE(submission.uploads.is_some());
        ASSERT_TRUE(submission.submit());
        a = BufferAllocation {};
        b = BufferAllocation {};
        manager.destroy();
        ASSERT_EQ(submission.fence.Wait(~rstd::uint64_t(0)), VK_SUCCESS);
        auto memory = readback.allocation();
        auto mapped = memory.map();
        ASSERT_TRUE(mapped.is_ok());
        auto mapping = mapped.unwrap_unchecked();
        ASSERT_TRUE(memory.invalidate().is_ok());
        for (unsigned i = 0; i < 256; ++i)
            EXPECT_EQ(static_cast<const rstd::uint8_t*>(mapping.data())[i], bytes[i]);
        const auto   heaps = device.memory_allocator().budget();
        VkDeviceSize usage {}, budget {};
        for (unsigned i = 0; i < heaps.heap_count; ++i) {
            usage += heaps.heaps[i].usage;
            budget += heaps.heaps[i].budget;
        }
        EXPECT_EQ(device.MemoryBudget().budget, budget);
        EXPECT_GT(usage, 0u);
    }
    EXPECT_EQ(errors.load(Ordering::Relaxed), u32());
}
TEST(OweMemory, FontAtlasUploadAndImageReadback) {
    errors.store(u32(), Ordering::Relaxed);
    {
        Context context;
        ASSERT_TRUE(context.initialize());
        auto&        device = context.device;
        TextureCache cache(device);
        auto         allocated = cache.AllocateTexture({ .width  = i32(8),
                                                         .height = i32(8),
                                                         .usage  = VK_IMAGE_USAGE_SAMPLED_BIT |
                                                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                                         .format = owe::TextureFormat::R8,
                                                         .sample = {} });
        ASSERT_TRUE(allocated.is_some());
        auto          texture = allocated.unwrap_unchecked();
        rstd::uint8_t bytes[64];
        for (unsigned i = 0; i < 64; ++i) bytes[i] = static_cast<rstd::uint8_t>(i * 3);
        ASSERT_TRUE(cache.UploadFontAtlasRegion(texture.deref(), bytes, 8, 0, 0, 8, 8));
        auto view  = texture->View();
        auto image = view.getActive().handle;
        auto made =
            device.memory_allocator().create_buffer({ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                                      .size  = 64,
                                                      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT },
                                                    vvk::MemoryRequest::Readback());
        ASSERT_TRUE(made.is_ok());
        auto       readback = made.unwrap_unchecked();
        Submission submission { device };
        ASSERT_TRUE(submission.begin());
        const auto&          d = device.handle().Dispatch();
        VkImageMemoryBarrier barrier { .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                       .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
                                       .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                       .oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                       .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .image               = image,
                                       .subresourceRange    = {
                                           VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        d.vkCmdPipelineBarrier(*submission.command,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0,
                               0,
                               nullptr,
                               0,
                               nullptr,
                               1,
                               &barrier);
        VkBufferImageCopy copy { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent      = { 8, 8, 1 } };
        d.vkCmdCopyImageToBuffer(*submission.command,
                                 image,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 readback.handle(),
                                 1,
                                 &copy);
        VkBufferMemoryBarrier host { .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                     .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
                                     .dstAccessMask       = VK_ACCESS_HOST_READ_BIT,
                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .buffer              = readback.handle(),
                                     .size                = VK_WHOLE_SIZE };
        d.vkCmdPipelineBarrier(*submission.command,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_HOST_BIT,
                               0,
                               0,
                               nullptr,
                               1,
                               &host,
                               0,
                               nullptr);
        ASSERT_TRUE(submission.submit());
        ASSERT_EQ(submission.fence.Wait(~rstd::uint64_t(0)), VK_SUCCESS);
        auto memory = readback.allocation();
        auto mapped = memory.map();
        ASSERT_TRUE(mapped.is_ok());
        auto mapping = mapped.unwrap_unchecked();
        ASSERT_TRUE(memory.invalidate().is_ok());
        for (unsigned i = 0; i < 64; ++i)
            EXPECT_EQ(static_cast<const rstd::uint8_t*>(mapping.data())[i], bytes[i]);
    }
    EXPECT_EQ(errors.load(Ordering::Relaxed), u32());
}
TEST(OweMemory, VideoBorrowsEnabledDispatch) {
    errors.store(u32(), Ordering::Relaxed);
    {
        Context context;
        ASSERT_TRUE(context.initialize());
        auto&                                       device = context.device;
        wavsen::video::Producer::ExternalDeviceInfo info {
            .instance           = device.instance_handle(),
            .physical_device    = *device.gpu(),
            .device             = *device.handle(),
            .queue              = *device.graphics_queue().handle,
            .queue_family_index = u32(device.graphics_queue().family_index),
            .api_version        = u32(device.instance_api_version()),
            .width              = u32(16),
            .height             = u32(16),
        };
        auto absent = wavsen::video::Producer::from_external(rstd::move(info));
        EXPECT_TRUE(absent.is_err());
        wavsen::video::Producer::ExternalDeviceInfo complete {
            .instance           = device.instance_handle(),
            .physical_device    = *device.gpu(),
            .device             = *device.handle(),
            .queue              = *device.graphics_queue().handle,
            .queue_family_index = u32(device.graphics_queue().family_index),
            .api_version        = u32(device.instance_api_version()),
            .width              = u32(16),
            .height             = u32(16),
            .instance_dispatch  = &device.instance_dispatch(),
            .device_dispatch    = &device.handle().Dispatch(),
        };
        auto adopted = wavsen::video::Producer::from_external(rstd::move(complete));
        ASSERT_TRUE(adopted.is_ok());
        auto producer = adopted.unwrap_unchecked();
        EXPECT_TRUE(producer->device_dispatch().capabilities.timeline_semaphore);
        EXPECT_FALSE(producer->device_dispatch().capabilities.buffer_device_address);
        auto converted = wavsen::video::YuvToRgba::create(producer->instance_dispatch(),
                                                          producer->physical_device(),
                                                          producer->device_dispatch(),
                                                          producer->queue_family_index(),
                                                          producer->queue(),
                                                          u32(16),
                                                          u32(16));
        ASSERT_TRUE(converted.is_ok());
    }
    EXPECT_EQ(errors.load(Ordering::Relaxed), u32());
}
