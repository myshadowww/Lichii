// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include "common/scope_exit.h"
#include "video_core/host_shaders/mario_kart_motion_comp_spv.h"
#include "video_core/renderer_vulkan/scene_camera_math.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_mario_kart_motion.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
namespace Vulkan {
namespace {
constexpr VkImageSubresourceRange RANGE{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr std::array BINDINGS{
    VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
};
constexpr std::array TEMPLATES{
    VkDescriptorUpdateTemplateEntry{0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0,
                                    sizeof(DescriptorUpdateEntry)},
    VkDescriptorUpdateTemplateEntry{1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                    sizeof(DescriptorUpdateEntry), sizeof(DescriptorUpdateEntry)},
    VkDescriptorUpdateTemplateEntry{2, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                    2 * sizeof(DescriptorUpdateEntry),
                                    sizeof(DescriptorUpdateEntry)},
};
struct Parameters {
    std::array<f32, 12> view;
    std::array<f32, 4> projection;
    std::array<f32, 4> depth;
    std::array<f32, 16> previous_vp;
    VideoCommon::MarioKartUpscale::Curvature current_bend, previous_bend;
    std::array<u32, 4> extent;
};
static_assert(sizeof(Parameters) == 192);
class Pass final : public ComputePass {
public:
    Pass(const Device& device, Scheduler& scheduler, DescriptorPool& pool)
        : ComputePass(
              device, scheduler, pool, BINDINGS, TEMPLATES,
              DescriptorBankInfo{.uniform_buffers = 1, .textures = 1, .images = 1, .score = 3}, {},
              MARIO_KART_MOTION_COMP_SPV) {}
    void Record(Scheduler& scheduler, const void* descriptors, VkExtent2D size) {
        scheduler.Record([this, descriptors, size](vk::CommandBuffer cmd) {
            const VkMemoryBarrier before{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
                                 VK_ACCESS_SHADER_WRITE_BIT};
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, before);
            const auto set = descriptor_allocator.Commit();
            device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, descriptors);
            cmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
            cmd.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
            cmd.Dispatch((size.width + 7) / 8, (size.height + 7) / 8, 1);
            const VkMemoryBarrier after{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT};
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, after);
        });
    }
};
} // namespace
struct MarioKartMotion::Impl {
    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& allocator;
    ComputePassDescriptorQueue& queue;
    StagingBufferPool& staging;
    Pass pass;
    vk::Image image;
    vk::ImageView view;
    vk::Sampler sampler;
    VkExtent2D extent{};
    VideoCommon::MarioKartUpscale::Camera previous{};
    VideoCommon::MarioKartUpscale::Curvature previous_bend{};
    u64 previous_frame{};
    bool reset{true};
    std::chrono::steady_clock::time_point last{};
    Impl(const Device& d, Scheduler& s, MemoryAllocator& a, DescriptorPool& p,
         ComputePassDescriptorQueue& q, StagingBufferPool& b)
        : device{d}, scheduler{s}, allocator{a}, queue{q}, staging{b}, pass{d, s, p} {
        sampler = device.GetLogical().CreateSampler(
            VkSamplerCreateInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .magFilter = VK_FILTER_NEAREST,
                                .minFilter = VK_FILTER_NEAREST,
                                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .maxLod = 0});
    }
    NvidiaDlssUpscaler::Image Record(VkImageView depth, VkExtent2D size,
                                     const VideoCommon::MarioKartUpscale::Camera& camera,
                                     const VideoCommon::MarioKartUpscale::Curvature& bend,
                                     u64 frame, bool& history_reset) {
        const auto now = std::chrono::steady_clock::now();
        history_reset = reset || frame != previous_frame + 1 ||
                        now - last > std::chrono::milliseconds{250} ||
                        camera.projection != previous.projection;
        if (!history_reset) {
            const auto current_position = SceneCameraPosition(camera.view);
            const auto previous_position = SceneCameraPosition(previous.view);
            double distance{};
            for (size_t i = 0; i < 3; ++i)
                distance += std::pow(current_position[i] - previous_position[i], 2);
            float direction{};
            for (size_t i = 0; i < 3; ++i)
                direction += camera.view[8 + i] * previous.view[8 + i];
            history_reset = distance > 500.0 * 500.0 || direction < 0.5f ||
                            std::hypot(bend.center_x - previous_bend.center_x,
                                       bend.center_z - previous_bend.center_z) > 500.f;
        }
        SCOPE_EXIT {
            scheduler.InvalidateState();
        };
        scheduler.RequestOutsideRenderPassOperationContext();
        if (!image || size.width > extent.width || size.height > extent.height) {
            if (image)
                scheduler.Finish();
            extent = {std::max(extent.width, size.width), std::max(extent.height, size.height)};
            view.reset();
            image = allocator.CreateImage(
                VkImageCreateInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .imageType = VK_IMAGE_TYPE_2D,
                                  .format = VK_FORMAT_R16G16_SFLOAT,
                                  .extent = {extent.width, extent.height, 1},
                                  .mipLevels = 1,
                                  .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE});
            view = device.GetLogical().CreateImageView(
                VkImageViewCreateInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                      .image = *image,
                                      .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                      .format = VK_FORMAT_R16G16_SFLOAT,
                                      .subresourceRange = RANGE});
            scheduler.Record([handle = *image](vk::CommandBuffer cmd) {
                const VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                                   .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                                   .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                   .image = handle,
                                                   .subresourceRange = RANGE};
                cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, barrier);
            });
            history_reset = true;
        }
        const auto& p = camera.projection;
        const Parameters parameters{camera.view,
                                    {p[0], p[5], p[6], p[10]},
                                    {p[11], history_reset ? 1.f : 0.f, 0, 0},
                                    history_reset ? camera.view_projection
                                                  : previous.view_projection,
                                    bend,
                                    history_reset ? bend : previous_bend,
                                    {size.width, size.height, 0, 0}};
        const auto upload = staging.Request(sizeof(parameters), MemoryUsage::Upload);
        std::memcpy(upload.mapped_span.data(), &parameters, sizeof(parameters));
        queue.Acquire(scheduler, 3);
        queue.AddSampledImage(depth, *sampler);
        queue.AddImage(*view);
        queue.AddBuffer(upload.buffer, upload.offset, sizeof(parameters));
        pass.Record(scheduler, queue.UpdateData(), size);
        scheduler.InvalidateState();
        previous = camera;
        previous_bend = bend;
        previous_frame = frame;
        last = now;
        reset = false;
        return {*view, *image, VK_FORMAT_R16G16_SFLOAT, RANGE, VK_IMAGE_ASPECT_COLOR_BIT, extent};
    }
};
MarioKartMotion::MarioKartMotion(const Device& d, Scheduler& s, MemoryAllocator& a,
                                 DescriptorPool& p, ComputePassDescriptorQueue& q,
                                 StagingBufferPool& b)
    : impl{std::make_unique<Impl>(d, s, a, p, q, b)} {}
MarioKartMotion::~MarioKartMotion() {
    if (impl->image)
        impl->scheduler.Finish();
}
NvidiaDlssUpscaler::Image MarioKartMotion::Record(VkImageView d, VkExtent2D s,
                                                  const VideoCommon::MarioKartUpscale::Camera& c,
                                                  const VideoCommon::MarioKartUpscale::Curvature& b,
                                                  u64 f, bool& reset) {
    return impl->Record(d, s, c, b, f, reset);
}
void MarioKartMotion::ResetHistory() {
    impl->reset = true;
}
} // namespace Vulkan
