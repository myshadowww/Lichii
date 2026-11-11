// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <optional>
#include "common/logging.h"
#include "common/scope_exit.h"
#include "video_core/host_shaders/scene_camera_motion_comp_spv.h"
#include "video_core/host_shaders/scene_upscale_color_comp_spv.h"
#include "video_core/host_shaders/scene_upscale_depth_comp_spv.h"
#include "video_core/host_shaders/scene_upscale_output_comp_spv.h"
#include "video_core/host_shaders/scene_upscale_output_rgba16f_comp_spv.h"
#include "video_core/host_shaders/scene_upscale_output_rgba8_comp_spv.h"
#include "video_core/host_shaders/scene_upscale_output_unorm_comp_spv.h"
#include "video_core/renderer_vulkan/scene_camera_math.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_scene_upscale.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {
using Matrix = std::array<f32, 16>;
std::optional<Matrix> ClipToPreviousClip(const Matrix& current, const Matrix& previous) {
    std::array<std::array<f64, 8>, 4> rows{};
    for (size_t i = 0; i < 4; ++i) {
        std::copy_n(current.begin() + i * 4, 4, rows[i].begin());
        rows[i][i + 4] = 1.0;
    }
    for (size_t col = 0; col < 4; ++col) {
        const auto pivot =
            std::max_element(rows.begin() + col, rows.end(), [col](const auto& a, const auto& b) {
                return std::abs(a[col]) < std::abs(b[col]);
            });
        std::swap(rows[col], *pivot);
        const f64 divisor = rows[col][col];
        if (!std::isfinite(divisor) || std::abs(divisor) < 1e-10) {
            return std::nullopt;
        }
        for (f64& value : rows[col]) {
            value /= divisor;
        }
        for (size_t row = 0; row < 4; ++row) {
            if (row == col) {
                continue;
            }
            const f64 scale = rows[row][col];
            for (size_t i = 0; i < 8; ++i) {
                rows[row][i] -= scale * rows[col][i];
            }
        }
    }
    Matrix result{};
    for (size_t row = 0; row < 4; ++row) {
        for (size_t col = 0; col < 4; ++col) {
            f64 value{};
            for (size_t i = 0; i < 4; ++i) {
                value += previous[row * 4 + i] * rows[i][col + 4];
            }
            result[row * 4 + col] = static_cast<f32>(value);
        }
    }
    if (!std::ranges::all_of(result, [](f32 value) { return std::isfinite(value); })) {
        return std::nullopt;
    }
    return result;
}

constexpr VkImageSubresourceRange COLOR_RANGE{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
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
struct Parameters {
    Matrix clip_to_previous_clip;
    std::array<f32, 4> pixel_to_clip;
    std::array<f32, 4> depth_and_jitter;
    std::array<u32, 2> active_extent;
};
struct OutputParameters {
    std::array<f32, 4> luminance{};
    std::array<f32, 4> shoulder{};
    u32 enabled{};
    u32 padding{};
    std::array<s32, 2> origin{};
    std::array<u32, 2> active_extent{};
    u32 scaled_srgb{};
    f32 srgb_scale = 1.0f;
};
struct CopyParameters {
    std::array<s32, 2> origin{};
    f32 srgb_scale = 1.0f;
    u32 scaled_srgb{};
    std::array<u32, 2> source_extent{};
    std::array<u32, 18> padding{};
    std::array<u32, 2> active_extent{};
};
static_assert(offsetof(CopyParameters, active_extent) == 96);
static_assert(offsetof(OutputParameters, origin) == 40);
static_assert(offsetof(OutputParameters, active_extent) == 48);
bool Contains(VkExtent2D backing, VkOffset2D origin, VkExtent2D active) {
    return origin.x >= 0 && origin.y >= 0 && active.width && active.height &&
           u32(origin.x) <= backing.width && u32(origin.y) <= backing.height &&
           active.width <= backing.width - u32(origin.x) &&
           active.height <= backing.height - u32(origin.y);
}
bool SameRegion(const std::optional<SceneUpscaler::Region>& a, const SceneUpscaler::Region* b) {
    if (a.has_value() != (b != nullptr))
        return false;
    if (!b)
        return true;
    return a->color_origin.x == b->color_origin.x && a->color_origin.y == b->color_origin.y &&
           a->depth_origin.x == b->depth_origin.x && a->depth_origin.y == b->depth_origin.y &&
           a->target_origin.x == b->target_origin.x && a->target_origin.y == b->target_origin.y &&
           a->target_extent.width == b->target_extent.width &&
           a->target_extent.height == b->target_extent.height && a->scaled_srgb == b->scaled_srgb &&
           a->srgb_scale == b->srgb_scale;
}
constexpr std::array PUSH_CONSTANTS{
    VkPushConstantRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Parameters)},
};
class ScenePass final : public ComputePass {
public:
    ScenePass(const Device& device_, Scheduler& scheduler, DescriptorPool& pool,
              std::span<const u32> code)
        : ComputePass(device_, scheduler, pool, BINDINGS, TEMPLATES, BANK, PUSH_CONSTANTS, code) {}
    template <typename Push = Parameters>
    void Record(Scheduler& scheduler, const void* descriptors, VkExtent2D size,
                Push parameters = {}, const NvidiaDlssUpscaler* require_success = nullptr) {
        static_assert(sizeof(Push) <= sizeof(Parameters));
        if constexpr (requires { parameters.active_extent; })
            parameters.active_extent = {size.width, size.height};
        scheduler.Record(
            [this, descriptors, size, parameters, require_success](vk::CommandBuffer cmd) {
                if (require_success && require_success->Failed())
                    return;
                const VkMemoryBarrier before{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                };
                cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, before);
                const VkDescriptorSet set = descriptor_allocator.Commit();
                device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, descriptors);
                cmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
                cmd.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
                cmd.PushConstants(*layout, VK_SHADER_STAGE_COMPUTE_BIT, parameters);
                cmd.Dispatch((size.width + 7) / 8, (size.height + 7) / 8, 1);
                const VkMemoryBarrier after{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                };
                cmd.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, after);
            });
    }
};
} // namespace

struct SceneUpscaler::Impl {
    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& allocator;
    ComputePassDescriptorQueue& queue;
    ScenePass color_pass, depth_pass, motion_pass, output_pass, output_unorm_pass,
        output_rgba8_pass, output_rgba16f_pass;
    vk::Image color, depth, motion;
    vk::ImageView color_view, depth_view, motion_view;
    VkExtent2D extent{};
    VkExtent2D output_extent{};
    Camera previous{};
    u64 previous_frame{};
    bool reset{true};
    bool previous_prepared_motion{};
    bool previous_resampled{};
    std::optional<Region> previous_region;
    std::chrono::steady_clock::time_point last_evaluation{};
    u64 evaluations{};
    std::array<u64, 13> reset_reason_counts{};
    u64 reset_trace_frames{};
    u32 reset_trace_samples{};
    NvidiaDlssUpscaler upscaler;

    Impl(const Device& device_, Scheduler& scheduler_, MemoryAllocator& allocator_,
         DescriptorPool& pool, ComputePassDescriptorQueue& queue_)
        : device{device_}, scheduler{scheduler_}, allocator{allocator_}, queue{queue_},
          color_pass{device, scheduler, pool, SCENE_UPSCALE_COLOR_COMP_SPV},
          depth_pass{device, scheduler, pool, SCENE_UPSCALE_DEPTH_COMP_SPV},
          motion_pass{device, scheduler, pool, SCENE_CAMERA_MOTION_COMP_SPV},
          output_pass{device, scheduler, pool, SCENE_UPSCALE_OUTPUT_COMP_SPV},
          output_unorm_pass{device, scheduler, pool, SCENE_UPSCALE_OUTPUT_UNORM_COMP_SPV},
          output_rgba8_pass{device, scheduler, pool, SCENE_UPSCALE_OUTPUT_RGBA8_COMP_SPV},
          output_rgba16f_pass{device, scheduler, pool, SCENE_UPSCALE_OUTPUT_RGBA16F_COMP_SPV},
          upscaler{device, scheduler, allocator} {}

    void Allocate(vk::Image& image, vk::ImageView& view, VkFormat format) {
        view.reset();
        image = allocator.CreateImage(VkImageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {extent.width, extent.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        });
        view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = *image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = COLOR_RANGE,
        });
        scheduler.Record([handle = *image](vk::CommandBuffer cmd) {
            const VkImageMemoryBarrier barrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = handle,
                .subresourceRange = COLOR_RANGE,
            };
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, barrier);
        });
    }

    bool Record(NvidiaDlssUpscaler::Inputs inputs, VkImageView target, const Camera* camera,
                u64 frame, VkFormat target_format, const OutputTransform* transform = nullptr,
                const Region* region = nullptr, const VkExtent2D* source_extent = nullptr) {
        if (upscaler.Failed() || !target || !inputs.color.view || !inputs.depth.view ||
            inputs.render_size.width == 0 || inputs.render_size.height == 0 ||
            (target_format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 &&
             target_format != VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
             target_format != VK_FORMAT_R8G8B8A8_UNORM &&
             target_format != VK_FORMAT_R16G16B16A16_SFLOAT) ||
            (evaluations && frame == previous_frame))
            return false;
        if (source_extent &&
            (!camera || region || transform || !inputs.hdr ||
             inputs.jitter != std::array<f32, 2>{} ||
             !Contains(inputs.color.extent, {}, *source_extent) ||
             !Contains(inputs.depth.extent, {}, *source_extent)))
            return false;
        if (region &&
            (!camera || transform || target_format != VK_FORMAT_R16G16B16A16_SFLOAT ||
             !Contains(inputs.color.extent, region->color_origin, inputs.render_size) ||
             !Contains(inputs.depth.extent, region->depth_origin, inputs.render_size) ||
             !Contains(region->target_extent, region->target_origin, inputs.upscale_size) ||
             !std::isfinite(region->srgb_scale) || region->srgb_scale <= 0 ||
             (region->scaled_srgb && !inputs.hdr)))
            return false;
        if (transform &&
            (target_format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
             !std::ranges::all_of(transform->luminance, [](f32 x) { return std::isfinite(x); }) ||
             !std::ranges::all_of(transform->shoulder, [](f32 x) { return std::isfinite(x); }) ||
             transform->luminance[2] < 0 || transform->shoulder[0] < 0 ||
             (transform->luminance[3] != 0 && transform->luminance[2] == 0) ||
             transform->shoulder[1] > 0 || transform->shoulder[3] < 0))
            return false;
        const bool prepared_motion = camera == nullptr;
        if (prepared_motion) {
            const auto& input = inputs.motion_vectors;
            if (!input.view || !input.image || input.format != VK_FORMAT_R16G16_SFLOAT ||
                input.range.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
                ((input.extent.width || input.extent.height) &&
                 (input.extent.width < inputs.render_size.width ||
                  input.extent.height < inputs.render_size.height)))
                return false;
        }
        // Camera-position reconstruction assumes a rigid world-to-view basis.
        if (camera)
            for (size_t row = 0; row < 3; ++row) {
                for (size_t other = 0; other < 3; ++other) {
                    f32 dot{};
                    for (size_t col = 0; col < 3; ++col)
                        dot += camera->view[row * 4 + col] * camera->view[other * 4 + col];
                    if (!std::isfinite(dot) || std::abs(dot - (row == other ? 1.f : 0.f)) > 1e-3f)
                        return false;
                }
            }
        const auto now = std::chrono::steady_clock::now();
        bool history_reset = reset || !evaluations || frame != previous_frame + 1 ||
                             !SameRegion(previous_region, region) ||
                             prepared_motion != previous_prepared_motion ||
                             (source_extent != nullptr) != previous_resampled ||
                             now - last_evaluation > std::chrono::milliseconds{250} ||
                             output_extent.width != inputs.upscale_size.width ||
                             output_extent.height != inputs.upscale_size.height;
        // Bounded diagnostics distinguish independent history-reset causes.
        // explicit, first, frame gap, region, path/policy, wall gap, output, projection,
        // rotation, distance, clip convention, allocation, caller reset.
        u32 reset_reasons = (u32{reset} << 0) | (u32{!evaluations} << 1) |
                            (u32{frame != previous_frame + 1} << 2) |
                            (u32{!SameRegion(previous_region, region)} << 3) |
                            (u32{prepared_motion != previous_prepared_motion ||
                                 (source_extent != nullptr) != previous_resampled} << 4) |
                            (u32{now - last_evaluation > std::chrono::milliseconds{250}} << 5) |
                            (u32{output_extent.width != inputs.upscale_size.width ||
                                 output_extent.height != inputs.upscale_size.height} << 6) |
                            (u32{inputs.reset} << 12);
        f32 projection_delta{};
        f32 minimum_axis_dot{1};
        double camera_distance_squared{};
        if (camera) {
            const bool policy_changed =
                camera->focal_scale_animation != previous.focal_scale_animation;
            history_reset |= policy_changed;
            reset_reasons |= u32{policy_changed} << 4;
            const bool focal_scale_compatible =
                camera->focal_scale_animation && previous.focal_scale_animation &&
                SceneFocalScaleCompatible(camera->projection, previous.projection);
            for (size_t i = 0; i < camera->projection.size(); ++i) {
                const f32 delta = std::abs(camera->projection[i] - previous.projection[i]);
                projection_delta = std::max(projection_delta, delta);
                const bool projection_cut =
                    delta > 1e-4f && !(focal_scale_compatible && (i == 0 || i == 5));
                reset_reasons |= u32{projection_cut} << 7;
                history_reset |= projection_cut;
            }
            double distance_squared{};
            const auto current_position = SceneCameraPosition(camera->view);
            const auto previous_position = SceneCameraPosition(previous.view);
            for (size_t row = 0; row < 3; ++row) {
                f32 axis_dot{};
                for (size_t col = 0; col < 3; ++col) {
                    axis_dot += camera->view[row * 4 + col] * previous.view[row * 4 + col];
                }
                minimum_axis_dot = std::min(minimum_axis_dot, axis_dot);
                reset_reasons |= u32{axis_dot < 0.7f} << 8;
                history_reset |= axis_dot < 0.7f;
                const double delta = current_position[row] - previous_position[row];
                distance_squared += delta * delta;
            }
            camera_distance_squared = distance_squared;
            reset_reasons |= (u32{distance_squared > 2500.f} << 9) |
                             (u32{camera->pixel_to_clip != previous.pixel_to_clip ||
                                  camera->depth_to_clip != previous.depth_to_clip} << 10);
            history_reset |= distance_squared > 2500.f ||
                             camera->pixel_to_clip != previous.pixel_to_clip ||
                             camera->depth_to_clip != previous.depth_to_clip;
        }
        SCOPE_EXIT {
            scheduler.InvalidateState();
        };
        scheduler.RequestOutsideRenderPassOperationContext();
        if (!color || extent.width < inputs.render_size.width ||
            extent.height < inputs.render_size.height) {
            if (color)
                scheduler.Finish();
            extent = {std::max(extent.width, inputs.render_size.width),
                      std::max(extent.height, inputs.render_size.height)};
            Allocate(color, color_view, VK_FORMAT_R16G16B16A16_SFLOAT);
            Allocate(depth, depth_view, VK_FORMAT_R32_SFLOAT);
            if (camera || motion)
                Allocate(motion, motion_view, VK_FORMAT_R16G16_SFLOAT);
            history_reset = true;
            reset_reasons |= 1U << 11;
        }
        Parameters parameters{};
        if (camera) {
            if (!motion) {
                Allocate(motion, motion_view, VK_FORMAT_R16G16_SFLOAT);
                history_reset = true;
                reset_reasons |= 1U << 11;
            }
            const auto transform = ClipToPreviousClip(camera->view_projection,
                                                      history_reset ? camera->view_projection
                                                                    : previous.view_projection);
            if (!transform) {
                reset = true;
                return false;
            }
            parameters = {*transform,
                          camera->pixel_to_clip,
                          {camera->depth_to_clip[0], camera->depth_to_clip[1], inputs.jitter[0],
                           inputs.jitter[1]}};
        }
        queue.Acquire(scheduler, 2);
        queue.AddSampledImage(inputs.color.view, upscaler.PointSampler());
        queue.AddImage(*color_view);
        CopyParameters color_copy{};
        CopyParameters depth_copy{};
        if (source_extent) {
            color_copy.source_extent = {source_extent->width, source_extent->height};
            depth_copy.source_extent = color_copy.source_extent;
        }
        if (region) {
            color_copy.origin = {region->color_origin.x, region->color_origin.y};
            color_copy.scaled_srgb = region->scaled_srgb;
            color_copy.srgb_scale = region->srgb_scale;
            depth_copy.origin = {region->depth_origin.x, region->depth_origin.y};
        }
        color_pass.Record(scheduler, queue.UpdateData(), inputs.render_size, color_copy);
        queue.Acquire(scheduler, 2);
        queue.AddSampledImage(inputs.depth.view, upscaler.PointSampler());
        queue.AddImage(*depth_view);
        depth_pass.Record(scheduler, queue.UpdateData(), inputs.render_size, depth_copy);
        if (camera) {
            queue.Acquire(scheduler, 2);
            queue.AddSampledImage(*depth_view, upscaler.PointSampler());
            queue.AddImage(*motion_view);
            motion_pass.Record(scheduler, queue.UpdateData(), inputs.render_size, parameters);
            inputs.motion_vectors = {*motion_view,
                                     *motion,
                                     VK_FORMAT_R16G16_SFLOAT,
                                     COLOR_RANGE,
                                     VK_IMAGE_ASPECT_COLOR_BIT,
                                     extent};
        }
        inputs.color = {*color_view,
                        *color,
                        VK_FORMAT_R16G16B16A16_SFLOAT,
                        COLOR_RANGE,
                        VK_IMAGE_ASPECT_COLOR_BIT,
                        extent};
        inputs.depth = {
            *depth_view, *depth, VK_FORMAT_R32_SFLOAT, COLOR_RANGE, VK_IMAGE_ASPECT_COLOR_BIT,
            extent};
        inputs.motion_vector_scale = {1, 1};
        inputs.reset |= history_reset;
        if (!upscaler.Upscale(inputs)) {
            reset = true;
            return false;
        }
        queue.Acquire(scheduler, 2);
        queue.AddSampledImage(upscaler.OutputView(), upscaler.PointSampler());
        queue.AddImage(target);
        auto& writeback = target_format == VK_FORMAT_R16G16B16A16_SFLOAT ? output_rgba16f_pass
                          : target_format == VK_FORMAT_R8G8B8A8_UNORM    ? output_rgba8_pass
                          : target_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? output_unorm_pass
                                                                                : output_pass;
        OutputParameters output_parameters =
            transform ? OutputParameters{transform->luminance, transform->shoulder, 1}
                      : OutputParameters{};
        if (region) {
            output_parameters.origin = {region->target_origin.x, region->target_origin.y};
            output_parameters.scaled_srgb = region->scaled_srgb;
            output_parameters.srgb_scale = region->srgb_scale;
        }
        writeback.Record(scheduler, queue.UpdateData(), inputs.upscale_size, output_parameters,
                         &upscaler);
        static const bool trace_resets = [] {
            const char* value = std::getenv("EDEN_NX2_DLSS_CONTRACT_TRACE");
            return value && std::string_view{value} == "1";
        }();
        if (trace_resets) {
            ++reset_trace_frames;
            for (size_t bit = 0; bit < reset_reason_counts.size(); ++bit)
                reset_reason_counts[bit] += (reset_reasons >> bit) & 1;
            if ((inputs.reset && reset_trace_samples < 24) || reset_trace_frames % 120 == 0) {
                reset_trace_samples += inputs.reset && reset_trace_samples < 24;
                LOG_INFO(Render_Vulkan,
                         "Scene history trace: queued={} frame={} previous={} reasons={:#x} "
                         "projection_delta={} camera_distance={} axis_dot={} wall_ms={} "
                         "counts(explicit,first,frame,region,path,wall,output,projection,rotation,"
                         "distance,clip,allocation,caller)={},{},{},{},{},{},{},{},{},{},{},{},{}",
                         reset_trace_frames, frame, previous_frame, reset_reasons,
                         projection_delta, std::sqrt(camera_distance_squared), minimum_axis_dot,
                         std::chrono::duration<double, std::milli>(now - last_evaluation).count(),
                         reset_reason_counts[0], reset_reason_counts[1], reset_reason_counts[2],
                         reset_reason_counts[3], reset_reason_counts[4], reset_reason_counts[5],
                         reset_reason_counts[6], reset_reason_counts[7], reset_reason_counts[8],
                         reset_reason_counts[9], reset_reason_counts[10], reset_reason_counts[11],
                         reset_reason_counts[12]);
            }
        }
        if (evaluations++ % 120 == 0 || inputs.reset) {
            LOG_INFO(Render_Vulkan,
                     "Scene NVIDIA DLSS queued: {}x{} -> {}x{}, {} motion, jitter={},{} reset={}",
                     inputs.render_size.width, inputs.render_size.height, inputs.upscale_size.width,
                     inputs.upscale_size.height, prepared_motion ? "prepared" : "camera",
                     inputs.jitter[0], inputs.jitter[1], inputs.reset);
        }
        if (camera)
            previous = *camera;
        previous_prepared_motion = prepared_motion;
        previous_resampled = source_extent != nullptr;
        previous_region = region ? std::optional{*region} : std::nullopt;
        previous_frame = frame;
        output_extent = inputs.upscale_size;
        last_evaluation = now;
        reset = false;
        return true;
    }
};

SceneUpscaler::SceneUpscaler(const Device& device, Scheduler& scheduler, MemoryAllocator& allocator,
                             DescriptorPool& pool, ComputePassDescriptorQueue& queue)
    : impl{std::make_unique<Impl>(device, scheduler, allocator, pool, queue)} {}
SceneUpscaler::~SceneUpscaler() = default;
bool SceneUpscaler::Record(NvidiaDlssUpscaler::Inputs inputs, VkImageView target,
                           const Camera& camera, u64 frame, VkFormat target_format,
                           const Region* region) {
    return impl->Record(inputs, target, &camera, frame, target_format, nullptr, region);
}
bool SceneUpscaler::RecordWithMotion(NvidiaDlssUpscaler::Inputs inputs, VkImageView target,
                                     u64 frame, VkFormat target_format,
                                     const OutputTransform* transform) {
    return impl->Record(inputs, target, nullptr, frame, target_format, transform);
}
bool SceneUpscaler::RecordResampled(NvidiaDlssUpscaler::Inputs inputs, VkImageView target,
                                    const Camera& camera, u64 frame, VkExtent2D source_extent,
                                    VkFormat target_format) {
    return impl->Record(inputs, target, &camera, frame, target_format, nullptr, nullptr,
                        &source_extent);
}
void SceneUpscaler::ResetHistory() {
    impl->reset = true;
}
} // namespace Vulkan
