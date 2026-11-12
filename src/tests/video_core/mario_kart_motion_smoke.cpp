// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "video_core/renderer_vulkan/vk_state_tracker.h"

#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_mario_kart_motion.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/vulkan_common/vulkan_device.h"
namespace {
using namespace Vulkan;
using VideoCommon::MarioKartUpscale::Camera;
using VideoCommon::MarioKartUpscale::Curvature;
constexpr VkExtent2D CAPACITY{24, 16};
constexpr VkImageSubresourceRange RANGE{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
Camera MakeCamera(float x) {
    Camera c{};
    c.view = {1, 0, 0, -x, 0, 1, 0, 0, 0, 0, 1, 0};
    c.projection = {1, 0, 0,  0, 0, 16.f / 9.f, -.18f, 0, 0, 0, -1.000200033f, -2.000200033f,
                    0, 0, -1, 0};
    for (size_t r = 0; r < 4; ++r)
        for (size_t col = 0; col < 4; ++col) {
            c.view_projection[r * 4 + col] = c.projection[r * 4 + 3] * (col == 3);
            for (size_t k = 0; k < 3; ++k)
                c.view_projection[r * 4 + col] += c.projection[r * 4 + k] * c.view[k * 4 + col];
        }
    return c;
}
// Independent double-precision homogeneous solve, rather than the shader's factored projection.
std::array<double, 3> Unproject(const Camera& c, double u, double v, double z) {
    std::array<std::array<double, 5>, 4> a{};
    const std::array<double, 4> clip{2 * u - 1, 1 - 2 * v, 2 * z - 1, 1};
    for (size_t r = 0; r < 4; ++r) {
        for (size_t k = 0; k < 4; ++k)
            a[r][k] = c.view_projection[r * 4 + k];
        a[r][4] = clip[r];
    }
    for (size_t k = 0; k < 4; ++k) {
        size_t pivot = k;
        for (size_t r = k + 1; r < 4; ++r)
            if (std::abs(a[r][k]) > std::abs(a[pivot][k]))
                pivot = r;
        std::swap(a[k], a[pivot]);
        const double d = a[k][k];
        for (auto& x : a[k])
            x /= d;
        for (size_t r = 0; r < 4; ++r)
            if (r != k) {
                const double f = a[r][k];
                for (size_t col = 0; col < 5; ++col)
                    a[r][col] -= f * a[k][col];
            }
    }
    return {a[0][4] / a[3][4], a[1][4] / a[3][4], a[2][4] / a[3][4]};
}
} // namespace
void MarioKartMotionSmoke(const Vulkan::Device& device) {
    MemoryAllocator allocator{device};
    StateTracker tracker;
    Scheduler scheduler{device, tracker};
    DescriptorPool pool{device, scheduler};
    ComputePassDescriptorQueue queue{device,
                                     ComputePassDescriptorQueue::COMPUTE_FRAME_PAYLOAD_SIZE};
    StagingBufferPool staging{device, allocator, scheduler};
    MarioKartMotion motion{device, scheduler, allocator, pool, queue, staging};
    auto depth = allocator.CreateImage(
        {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_R32_SFLOAT,
         .extent = {CAPACITY.width, CAPACITY.height, 1},
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE});
    auto depth_view =
        device.GetLogical().CreateImageView({.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                             .image = *depth,
                                             .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                             .format = VK_FORMAT_R32_SFLOAT,
                                             .subresourceRange = RANGE});
    constexpr size_t count = CAPACITY.width * CAPACITY.height;
    auto upload = allocator.CreateBuffer({.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                          .size = count * 4,
                                          .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                          .sharingMode = VK_SHARING_MODE_EXCLUSIVE},
                                         MemoryUsage::Upload);
    auto readback = allocator.CreateBuffer({.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                            .size = count * 4,
                                            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE},
                                           MemoryUsage::Download);
    scheduler.Record([&](vk::CommandBuffer cmd) {
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                            VkImageMemoryBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                                 .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                                 .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                                 .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                 .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                 .image = *depth,
                                                 .subresourceRange = RANGE});
    });
    Camera previous = MakeCamera(0);
    Curvature previous_bend{0, 0, 500, 20000};
    bool observed_curvature_motion{};
    for (u32 step = 0; step < 8; ++step) {
        const auto camera = MakeCamera(step >= 2 ? 10.f : 0.f);
        const Curvature bend{step >= 3 ? 75.f : 0.f, step >= 3 ? -25.f : 0.f, 500, 20000};
        const VkExtent2D size = step >= 4 ? VkExtent2D{11, 7} : VkExtent2D{17, 9};
        const bool expected_reset = step == 0 || step == 5 || step == 7;
        if (step == 5)
            motion.ResetHistory();
        std::array<float, count> depths{};
        for (size_t i = 0; i < count; ++i)
            depths[i] = step == 6 && i % 2 ? -.1f : .999f;
        std::memcpy(upload.Mapped().data(), depths.data(), sizeof(depths));
        upload.Flush();
        scheduler.Record([&](vk::CommandBuffer cmd) {
            cmd.PipelineBarrier(
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                VK_ACCESS_TRANSFER_WRITE_BIT});
            cmd.CopyBufferToImage(
                *upload, *depth, VK_IMAGE_LAYOUT_GENERAL,
                VkBufferImageCopy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {CAPACITY.width, CAPACITY.height, 1}});
        });
        bool reset{};
        const auto result =
            motion.Record(*depth_view, size, camera, bend, step + 1 + (step == 7 ? 2 : 0), reset);
        if (reset != expected_reset)
            throw std::runtime_error("Mario Kart motion history reset mismatch");
        scheduler.Record([&](vk::CommandBuffer cmd) {
            cmd.PipelineBarrier(
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT});
            cmd.CopyImageToBuffer(
                result.image, VK_IMAGE_LAYOUT_GENERAL, *readback,
                VkBufferImageCopy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {size.width, size.height, 1}});
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                                VK_ACCESS_HOST_READ_BIT});
        });
        scheduler.Finish();
        readback.Invalidate();
        for (u32 y = 0; y < size.height; ++y)
            for (u32 x = 0; x < size.width; ++x) {
                const double u = (x + .5) / size.width, v = (y + .5) / size.height;
                std::array<double, 2> expected{};
                const float d = depths[y * CAPACITY.width + x];
                if (!reset && d >= 0 && d <= 1) {
                    const auto curved = Unproject(camera, u, v, d);
                    const auto world = VideoCommon::MarioKartUpscale::Unbend(curved, bend);
                    const auto prior =
                        world ? VideoCommon::MarioKartUpscale::Bend(*world, previous_bend)
                              : std::nullopt;
                    if (prior) {
                        std::array<double, 4> clip{};
                        for (size_t r = 0; r < 4; ++r) {
                            clip[r] = previous.view_projection[r * 4 + 3];
                            for (size_t k = 0; k < 3; ++k)
                                clip[r] += previous.view_projection[r * 4 + k] * (*prior)[k];
                        }
                        expected = {(clip[0] / clip[3] * .5 + .5 - u) * size.width,
                                    (.5 - clip[1] / clip[3] * .5 - v) * size.height};
                    }
                }
                std::array<_Float16, 2> actual{};
                std::memcpy(actual.data(), readback.Mapped().data() + (y * size.width + x) * 4, 4);
                for (size_t k = 0; k < 2; ++k) {
                    if (!std::isfinite(float(actual[k])) ||
                        std::abs(float(actual[k]) - expected[k]) > .003) {
                        std::fprintf(
                            stderr,
                            "Mario Kart motion step%u pixel%u,%u axis%zu actual=%g expected=%g\n",
                            step, x, y, k, float(actual[k]), expected[k]);
                        throw std::runtime_error("Mario Kart curvature camera motion regression");
                    }
                    if (step == 3 && std::abs(expected[k]) > .005)
                        observed_curvature_motion = true;
                }
            }
        previous = camera;
        previous_bend = bend;
    }
    if (!observed_curvature_motion)
        throw std::runtime_error("Mario Kart curvature case lacked motion");
    std::printf("Mario Kart motion smoke: 8 cases; stationary, camera, changing curvature, crop, "
                "invalid depth and reset passed\n");
}
