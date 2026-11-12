// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <map>
#include "common/settings.h"
#include "yuzu/configuration/graphics_settings.h"

namespace ConfigurationShared {

namespace {
struct Placement {
    GraphicsSection section;
    size_t order;
};

const std::map<u32, Placement>& GetPlacements() {
    static const auto placements = [] {
        using Section = GraphicsSection;
        const auto& values = Settings::values;
        const std::pair<u32, Section> ordered[]{
            {values.renderer_backend.Id(), Section::Api},
            {values.vulkan_device.Id(), Section::Api},
            {values.fullscreen_mode.Id(), Section::Display},
            {values.aspect_ratio.Id(), Section::Display},
            {values.vsync_mode.Id(), Section::Display},
            {values.fps_limit.Id(), Section::Display},
            {values.frame_rate_limit.Id(), Section::Display},
            {values.frame_pacing_mode.Id(), Section::Display},
            {values.use_video_framerate.Id(), Section::Display},
            {values.hdr_mode.Id(), Section::Display},
            {values.hdr_swapchain.Id(), Section::Display},
            {values.hdr_reference_white.Id(), Section::Display},
            {values.hdr_peak_luminance.Id(), Section::Display},
            {values.resolution_setup.Id(), Section::ImageQuality},
            {values.scaling_filter.Id(), Section::ImageQuality},
            {values.anti_aliasing.Id(), Section::ImageQuality},
            {values.max_anisotropy.Id(), Section::ImageQuality},
            {values.post_shader_enabled.Id(), Section::ImageQuality},
            {values.game_dlss_network.Id(), Section::Upscaling},
            {values.fsr_sharpening_slider.Id(), Section::Upscaling},
            {values.nvidia_dlss.Id(), Section::Upscaling},
            {values.nvidia_dlss_quality.Id(), Section::Upscaling},
            {values.nvidia_dlss_preset.Id(), Section::Upscaling},
            {values.amd_fsr4.Id(), Section::Upscaling},
            {values.neural_rendering.Id(), Section::NeuralRendering},
            {values.neural_rendering_strength.Id(), Section::NeuralRendering},
            {values.neural_rendering_tone.Id(), Section::NeuralRendering},
            {values.neural_rendering_structure.Id(), Section::NeuralRendering},
            {values.frame_gen.Id(), Section::FrameGeneration},
            {values.frame_gen_type.Id(), Section::FrameGeneration},
            {values.frame_gen_multiplier.Id(), Section::FrameGeneration},
            {values.frame_gen_target_rate.Id(), Section::FrameGeneration},
            {values.frame_gen_flow_scale_auto.Id(), Section::FrameGeneration},
            {values.frame_gen_flow_scale.Id(), Section::FrameGeneration},
            {values.frame_gen_queue_target.Id(), Section::FrameGeneration},
            {values.gpu_accuracy.Id(), Section::Accuracy},
            {values.dma_accuracy.Id(), Section::Accuracy},
            {values.gpu_fence_behavior.Id(), Section::Accuracy},
            {values.native_compute_write_mark.Id(), Section::Accuracy},
            {values.sync_memory_operations.Id(), Section::Accuracy},
            {values.use_reactive_flushing.Id(), Section::Accuracy},
            {values.barrier_feedback_loops.Id(), Section::Accuracy},
            {values.enable_buffer_history.Id(), Section::Accuracy},
            {values.use_disk_shader_cache.Id(), Section::Shaders},
            {values.use_vulkan_driver_pipeline_cache.Id(), Section::Shaders},
            {values.precache_shaders.Id(), Section::Shaders},
            {values.build_all_cached_shaders.Id(), Section::Shaders},
            {values.use_asynchronous_shaders.Id(), Section::Shaders},
            {values.native_async_draws.Id(), Section::Shaders},
            {values.native_pipeline_wait_ms.Id(), Section::Shaders},
            {values.enable_compute_pipelines.Id(), Section::Shaders},
            {values.vram_usage_mode.Id(), Section::Memory},
            {values.accelerate_astc.Id(), Section::Memory},
            {values.astc_recompression.Id(), Section::Memory},
            {values.gpu_unswizzle_enabled.Id(), Section::Memory},
            {values.gpu_unswizzle_texture_size.Id(), Section::Memory},
            {values.gpu_unswizzle_stream_size.Id(), Section::Memory},
            {values.gpu_unswizzle_chunk_size.Id(), Section::Memory},
            {values.use_asynchronous_gpu_emulation.Id(), Section::Performance},
            {values.async_presentation.Id(), Section::Performance},
            {values.renderer_force_max_clock.Id(), Section::Performance},
            {values.nvdec_emulation.Id(), Section::Performance},
            {values.pin_gpu_threads.Id(), Section::Performance},
            {values.frame_gen_dump_flow.Id(), Section::Diagnostics},
            {values.ocarina_fsr.Id(), Section::Ocarina},
            {values.oot_anti_aliasing.Id(), Section::Ocarina},
            {values.mkw_30fps.Id(), Section::MarioKart},
            {values.mkw_dynamic_fps.Id(), Section::MarioKart},
        };
        std::map<u32, Placement> result;
        for (const auto& [id, section] : ordered) {
            result.emplace(id, Placement{section, result.size()});
        }
        return result;
    }();
    return placements;
}
} // namespace

GraphicsSection GetGraphicsSection(const Settings::BasicSetting& setting) {
    using Section = GraphicsSection;
    const auto& placements = GetPlacements();
    if (const auto it = placements.find(setting.Id()); it != placements.end()) {
        return it->second.section;
    }
    switch (setting.GetCategory()) {
    case Settings::Category::RendererAdvanced:
        return Section::Performance;
    case Settings::Category::RendererExtensions:
        return Section::Vulkan;
    case Settings::Category::RendererHacks:
        return Section::Workarounds;
    default:
        return Section::ImageQuality;
    }
}

static GraphicsPage GetGraphicsPage(GraphicsSection section) {
    switch (section) {
    case GraphicsSection::Api:
    case GraphicsSection::Display:
    case GraphicsSection::ImageQuality:
    case GraphicsSection::Upscaling:
    case GraphicsSection::NeuralRendering:
    case GraphicsSection::FrameGeneration:
        return GraphicsPage::Graphics;
    case GraphicsSection::Workarounds:
    case GraphicsSection::Vulkan:
        return GraphicsPage::Compatibility;
    case GraphicsSection::Ocarina:
    case GraphicsSection::MarioKart:
        return GraphicsPage::GameSpecific;
    default:
        return GraphicsPage::Advanced;
    }
}

std::vector<Settings::BasicSetting*> GetGraphicsSettings(GraphicsPage page) {
    std::vector<Settings::BasicSetting*> settings;
    for (const auto category :
         {Settings::Category::Renderer, Settings::Category::RendererAdvanced,
          Settings::Category::RendererExtensions, Settings::Category::RendererHacks}) {
        for (auto* setting : Settings::values.linkage.by_category.at(category)) {
            if (GetGraphicsPage(GetGraphicsSection(*setting)) == page) {
                settings.push_back(setting);
            }
        }
    }
    const auto& placements = GetPlacements();
    std::ranges::sort(settings, {}, [&placements](const Settings::BasicSetting* setting) {
        const auto it = placements.find(setting->Id());
        return it != placements.end() ? it->second.order : placements.size() + setting->Id();
    });
    return settings;
}

} // namespace ConfigurationShared
