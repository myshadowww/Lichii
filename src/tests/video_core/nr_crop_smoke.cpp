// SPDX-License-Identifier: GPL-3.0-or-later
// Optional hardware regression: exercise the production NR motion lookup without loading a model.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include "nr_crop_smoke_spv.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace {
using namespace Vulkan;
constexpr VkExtent2D CAPACITY{8, 6}, OUTPUT{17, 13};
constexpr VkImageSubresourceRange RANGE{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
struct Parameters {
    std::array<float, 2> valid{float(OUTPUT.width), float(OUTPUT.height)};
    std::array<float, 2> jitter{};
    std::array<float, 2> scale{0.125f, -0.25f};
    u32 extent{};
};
constexpr std::array BINDINGS{
    VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
};
constexpr std::array TEMPLATES{
    VkDescriptorUpdateTemplateEntry{0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0,
                                    sizeof(DescriptorUpdateEntry)},
    VkDescriptorUpdateTemplateEntry{1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                    sizeof(DescriptorUpdateEntry), sizeof(DescriptorUpdateEntry)},
};
constexpr DescriptorBankInfo BANK{.textures = 1, .images = 1, .score = 2};
constexpr std::array PUSH{VkPushConstantRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Parameters)}};

class Probe final : public ComputePass {
public:
    Probe(const Device& device, Scheduler& scheduler, DescriptorPool& pool)
        : ComputePass(device, scheduler, pool, BINDINGS, TEMPLATES, BANK, PUSH, NR_CROP_SMOKE_SPV) {
    }
    void Record(Scheduler& scheduler, const void* descriptors, Parameters params) {
        scheduler.Record([this, descriptors, params](vk::CommandBuffer cmd) {
            const auto set = descriptor_allocator.Commit();
            device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, descriptors);
            cmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
            cmd.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
            cmd.PushConstants(*layout, VK_SHADER_STAGE_COMPUTE_BIT, params);
            cmd.Dispatch((OUTPUT.width + 7) / 8, (OUTPUT.height + 7) / 8, 1);
        });
    }
};

struct Image {
    vk::Image image;
    vk::ImageView view;
};
Image MakeImage(const Device& device, MemoryAllocator& allocator, VkExtent2D size, VkFormat format,
                VkImageUsageFlags usage) {
    Image result;
    result.image = allocator.CreateImage({
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {size.width, size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    });
    result.view = device.GetLogical().CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *result.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = RANGE,
    });
    return result;
}
} // namespace

void NrCropSmoke(const Vulkan::Device& device) {
    MemoryAllocator allocator{device};
    StateTracker tracker;
    Scheduler scheduler{device, tracker};
    DescriptorPool pool{device, scheduler};
    ComputePassDescriptorQueue queue{device,
                                     ComputePassDescriptorQueue::COMPUTE_FRAME_PAYLOAD_SIZE};
    Probe probe{device, scheduler, pool};
    auto motion = MakeImage(device, allocator, CAPACITY, VK_FORMAT_R32G32_SFLOAT,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    auto result = MakeImage(device, allocator, OUTPUT, VK_FORMAT_R32G32B32A32_SFLOAT,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    auto sampler = device.GetLogical().CreateSampler({
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 1.0f,
    });
    auto upload = allocator.CreateBuffer(
        {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = CAPACITY.width * CAPACITY.height * sizeof(float) * 2,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        },
        MemoryUsage::Upload);
    auto readback = allocator.CreateBuffer(
        {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = OUTPUT.width * OUTPUT.height * sizeof(float) * 4,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        },
        MemoryUsage::Download);
    scheduler.Record([&](vk::CommandBuffer cmd) {
        for (const auto image : {*motion.image, *result.image}) {
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                VkImageMemoryBarrier{
                                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = image,
                                    .subresourceRange = RANGE,
                                });
        }
    });
    // Same image survives shrinking/growing crops, fractional mappings, jitter at both edges,
    // and the zero/default ABI. Poison every inactive texel before each dispatch.
    const std::array cases{
        Parameters{.extent = 4u | (3u << 16)},
        Parameters{.jitter = {-100.0f, 100.0f}, .extent = 4u | (3u << 16)},
        Parameters{.jitter = {100.0f, -100.0f}, .extent = 4u | (3u << 16)},
        Parameters{.jitter = {0.375f, -0.25f}, .extent = 2u | (2u << 16)},
        Parameters{.extent = 7u | (5u << 16)},
        Parameters{},
        Parameters{.extent = 64u | (64u << 16)},
    };
    for (const auto& params : cases) {
        const u32 packed_width = params.extent & 65535u, packed_height = params.extent >> 16;
        const bool cropped = packed_width && packed_height;
        const u32 width = cropped ? std::min(CAPACITY.width, packed_width) : CAPACITY.width;
        const u32 height = cropped ? std::min(CAPACITY.height, packed_height) : CAPACITY.height;
        for (u32 y = 0; y < CAPACITY.height; ++y) {
            for (u32 x = 0; x < CAPACITY.width; ++x) {
                const std::array<float, 2> value =
                    x < width && y < height
                        ? std::array<float, 2>{0.01f * (x + 1), -0.02f * (y + 1)}
                        : std::array<float, 2>{1000.0f, -1000.0f};
                std::memcpy(upload.Mapped().data() + (y * CAPACITY.width + x) * sizeof(value),
                            value.data(), sizeof(value));
            }
        }
        upload.Flush();
        scheduler.Record([&](vk::CommandBuffer cmd) {
            cmd.PipelineBarrier(
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                VK_ACCESS_TRANSFER_WRITE_BIT});
            cmd.CopyBufferToImage(
                *upload, *motion.image, VK_IMAGE_LAYOUT_GENERAL,
                VkBufferImageCopy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {CAPACITY.width, CAPACITY.height, 1}});
            cmd.PipelineBarrier(
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT});
        });
        queue.Acquire(scheduler, 2);
        queue.AddSampledImage(*motion.view, *sampler);
        queue.AddImage(*result.view);
        probe.Record(scheduler, queue.UpdateData(), params);
        scheduler.Record([&](vk::CommandBuffer cmd) {
            cmd.PipelineBarrier(
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT});
            cmd.CopyImageToBuffer(
                *result.image, VK_IMAGE_LAYOUT_GENERAL, *readback,
                VkBufferImageCopy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {OUTPUT.width, OUTPUT.height, 1}});
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                                VK_ACCESS_HOST_READ_BIT});
        });
        scheduler.Finish();
        readback.Invalidate();
        for (u32 y = 0; y < OUTPUT.height; ++y) {
            for (u32 x = 0; x < OUTPUT.width; ++x) {
                const int tx = std::clamp(int((x + params.jitter[0] + 0.5f) / OUTPUT.width * width),
                                          0, int(width) - 1);
                const int ty =
                    std::clamp(int((y + params.jitter[1] + 0.5f) / OUTPUT.height * height), 0,
                               int(height) - 1);
                const std::array<float, 4> expected{
                    float(tx), float(ty),
                    (x + 0.5f) / OUTPUT.width + 0.01f * (tx + 1) * params.scale[0],
                    (y + 0.5f) / OUTPUT.height - 0.02f * (ty + 1) * params.scale[1]};
                std::array<float, 4> actual;
                std::memcpy(actual.data(),
                            readback.Mapped().data() + (y * OUTPUT.width + x) * sizeof(actual),
                            sizeof(actual));
                for (size_t channel = 0; channel < actual.size(); ++channel) {
                    if (!std::isfinite(actual[channel]) ||
                        std::abs(actual[channel] - expected[channel]) > 1e-6f) {
                        throw std::runtime_error("NR motion crop/clamp GPU readback mismatch");
                    }
                }
            }
        }
    }
    std::printf(
        "NR crop smoke: %zu cases, %u pixels each; crop, jitter clamp and full-view ABI passed\n",
        cases.size(), OUTPUT.width * OUTPUT.height);
}
