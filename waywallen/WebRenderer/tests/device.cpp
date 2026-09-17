#include <unistd.h>
#include <rstd/test/gtest.hpp>
import rstd;
import vvk;
import weweb;
import waywallen.web_producer_device;
using namespace rstd::prelude;

namespace
{
struct Slot {
    const vvk::DeviceDispatch& dispatch;
    VkImage                    image {};
    VkDeviceMemory             memory {};
    VkCommandPool              pool {};
    ~Slot() {
        (void)dispatch.vkDeviceWaitIdle(dispatch.device);
        if (pool) dispatch.vkDestroyCommandPool(dispatch.device, pool, nullptr);
        if (image) dispatch.vkDestroyImage(dispatch.device, image, nullptr);
        if (memory) dispatch.vkFreeMemory(dispatch.device, memory, nullptr);
    }
};

void CheckUpload(ww_wescene::WebProducerDevice& producer, uint32_t width) {
    const auto&                     d = producer.DeviceDispatch();
    Slot                            slot { d };
    VkExternalMemoryImageCreateInfo external {
        .sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkImageCreateInfo image_info {
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext       = &external,
        .imageType   = VK_IMAGE_TYPE_2D,
        .format      = VK_FORMAT_R8G8B8A8_UNORM,
        .extent      = { width, 4, 1 },
        .mipLevels   = 1,
        .arrayLayers = 1,
        .samples     = VK_SAMPLE_COUNT_1_BIT,
        .tiling      = VK_IMAGE_TILING_OPTIMAL,
        .usage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    ASSERT_EQ(d.vkCreateImage(d.device, &image_info, nullptr, &slot.image), VK_SUCCESS);
    VkMemoryRequirements requirements {};
    d.vkGetImageMemoryRequirements(d.device, slot.image, &requirements);
    uint32_t type = 0;
    while (type < 32 && ! (requirements.memoryTypeBits & (1u << type))) ++type;
    ASSERT_TRUE(type < 32);
    VkMemoryDedicatedAllocateInfo dedicated {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = slot.image,
    };
    VkExportMemoryAllocateInfo export_info {
        .sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext       = &dedicated,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkMemoryAllocateInfo allocation {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = &export_info,
        .allocationSize  = requirements.size,
        .memoryTypeIndex = type,
    };
    ASSERT_EQ(d.vkAllocateMemory(d.device, &allocation, nullptr, &slot.memory), VK_SUCCESS);
    ASSERT_EQ(d.vkBindImageMemory(d.device, slot.image, slot.memory, 0), VK_SUCCESS);

    uint8_t pixels[128 * 4 * 4] {};
    for (uint32_t i = 0; i < width * 4; ++i) {
        pixels[i * 4]     = 13;
        pixels[i * 4 + 1] = 27;
        pixels[i * 4 + 2] = 91;
        pixels[i * 4 + 3] = 255;
    }
    weweb::CpuPaintFrame frame {
        .buffer     = pixels,
        .width      = static_cast<int>(width),
        .height     = 4,
        .row_stride = width * 4,
        .format     = weweb::DmaBufFormat::BGRA8_UNORM,
    };
    int fd = producer.UploadToSlot(frame, slot.image, { width, 4 }, image_info.format);
    ASSERT_TRUE(fd >= 0);
    ::close(fd);
    ASSERT_EQ(d.vkDeviceWaitIdle(d.device), VK_SUCCESS);

    auto allocator_result =
        vvk::MemoryAllocator::Create(producer.Physical(), producer.InstanceDispatch(), d);
    ASSERT_TRUE(allocator_result.is_ok());
    auto               allocator = allocator_result.unwrap_unchecked();
    VkBufferCreateInfo buffer_info {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = width * 4 * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    auto buffer_result = allocator.create_buffer(buffer_info, vvk::MemoryRequest::Readback());
    ASSERT_TRUE(buffer_result.is_ok());
    auto                    buffer = buffer_result.unwrap_unchecked();
    VkCommandPoolCreateInfo pool_info {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = producer.QueueFamily(),
    };
    ASSERT_EQ(d.vkCreateCommandPool(d.device, &pool_info, nullptr, &slot.pool), VK_SUCCESS);
    VkCommandBufferAllocateInfo commands {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = slot.pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command {};
    ASSERT_EQ(d.vkAllocateCommandBuffers(d.device, &commands, &command), VK_SUCCESS);
    VkCommandBufferBeginInfo begin { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    ASSERT_EQ(d.vkBeginCommandBuffer(command, &begin), VK_SUCCESS);
    VkImageMemoryBarrier acquire {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .dstQueueFamilyIndex = producer.QueueFamily(),
        .image               = slot.image,
        .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    d.vkCmdPipelineBarrier(command,
                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0,
                           0,
                           nullptr,
                           0,
                           nullptr,
                           1,
                           &acquire);
    VkBufferImageCopy copy {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent      = { width, 4, 1 },
    };
    d.vkCmdCopyImageToBuffer(
        command, slot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.handle(), 1, &copy);
    VkMemoryBarrier host {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    d.vkCmdPipelineBarrier(command,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT,
                           0,
                           1,
                           &host,
                           0,
                           nullptr,
                           0,
                           nullptr);
    ASSERT_EQ(d.vkEndCommandBuffer(command), VK_SUCCESS);
    VkSubmitInfo submit {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &command,
    };
    ASSERT_EQ(d.vkQueueSubmit(producer.Queue(), 1, &submit, VK_NULL_HANDLE), VK_SUCCESS);
    ASSERT_EQ(d.vkDeviceWaitIdle(d.device), VK_SUCCESS);
    auto mapping = buffer.allocation().map();
    ASSERT_TRUE(mapping.is_ok());
    ASSERT_TRUE(buffer.allocation().invalidate().is_ok());
    auto        mapped = mapping.unwrap_unchecked();
    const auto* actual = static_cast<const uint8_t*>(mapped.data());
    for (uint32_t i = 0; i < width * 4; ++i) {
        ASSERT_EQ(actual[i * 4], 91);
        ASSERT_EQ(actual[i * 4 + 1], 27);
        ASSERT_EQ(actual[i * 4 + 2], 13);
        ASSERT_EQ(actual[i * 4 + 3], 255);
    }
}
} // namespace

TEST(WebProducer, RuntimeDispatchUploadAndReinitialize) {
    ww_wescene::WebProducerDevice producer;
    ASSERT_TRUE(producer.Init());
    CheckUpload(producer, 64);
    CheckUpload(producer, 64);
    CheckUpload(producer, 128);
    producer.Shutdown();
    producer.Shutdown();
    ASSERT_EQ(producer.Instance(), VK_NULL_HANDLE);
    ASSERT_EQ(producer.Device(), VK_NULL_HANDLE);
    producer.SetRenderNode("/nonexistent/owe-test-render-node");
    ASSERT_FALSE(producer.Init());
    ASSERT_EQ(producer.Instance(), VK_NULL_HANDLE);
    producer.SetRenderNode("");
    ASSERT_TRUE(producer.Init());
    CheckUpload(producer, 32);
}
