// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include "common/common_types.h"
#include "video_core/textures/texture.h"

namespace VideoCommon::FireEmblemUpscale {

constexpr u64 ProgramId = 0x04007EF01F8AE000ULL;
// NativeProgramKey of the captured DLSS final output pass, not its lowered shader hash.
constexpr u64 OutputProgramKey = 0x14b4222bbe23abeeULL;
constexpr u64 PrepassProgramKey = 0x4cbf6eff32525aecULL;
constexpr u64 AlternatePrepassProgramKey = 0x5e8d8dc58bed8ba4ULL;
constexpr u64 StarFoxProgramId = 0x0400E88025B1A000ULL;
constexpr u64 StarFoxPrepassProgramKey = 0xd686ca0b9fad9e6dULL;
constexpr u64 StarFoxCutscenePrepassProgramKey = 0xebab31dd6f0fc08dULL;
constexpr u64 StarFoxOutputProgramKey = 0x7d3764ff101bec27ULL;
constexpr u64 StarFoxFinalUpscaleProgramKey = 0x4027797e061f5eb4ULL;

constexpr bool IsStarFoxFinalUpscale(u64 program_id, u64 native_key) {
    return program_id == StarFoxProgramId && native_key == StarFoxFinalUpscaleProgramKey;
}
// Lowered hashes of the captured skinned scene geometry; the fragment stage writes unjittered
// current-minus-previous NDC into SNORM RG. The vertex adds cb6[370/374] only to raster position.
constexpr u64 StarFoxGameplayVertex = 0x7d855f8f8f931fd9ULL;
constexpr u64 StarFoxGameplayFragment = 0xd77cd49b720c0e8eULL;
constexpr u64 StarFoxSceneCopyVertex = 0x0b00888463c45ad4ULL;
constexpr u64 StarFoxSceneCopyFragment = 0xd1e9123209ee3c2dULL;

inline std::optional<std::array<f32, 2>> StarFoxGameplayJitter(
    std::span<const u32> vertex_object, std::span<const u32> camera,
    std::span<const u32> fragment_object, std::span<const u32> projection,
    std::span<const u32> fragment_camera) {
    if (vertex_object.size_bytes() < 0x11c || camera.size_bytes() < 0x394 ||
        fragment_object.size_bytes() < 0x12c || projection.size_bytes() < 0xc0 ||
        fragment_camera.size_bytes() < 0x200) {
        return std::nullopt;
    }
    const auto f = [](std::span<const u32> words, u32 offset) {
        return std::bit_cast<f32>(words[offset / sizeof(u32)]);
    };
    if (vertex_object[0x7c / 4] != 0 || vertex_object[0xac / 4] != 0 || camera[0x334 / 4] != 0 ||
        camera[0x390 / 4] != 0) {
        return std::nullopt;
    }
    for (const auto object : {vertex_object, fragment_object}) {
        if (f(object, 0x110) != 0 || f(object, 0x114) != 0 || f(object, 0x118) != 1) {
            return std::nullopt;
        }
    }
    if (f(fragment_object, 0x120) != 0 || f(fragment_object, 0x124) != 0 ||
        f(fragment_object, 0x128) != 1) {
        return std::nullopt;
    }
    // Perspective projection has no jitter baked in; the known vertex shader adds it separately.
    for (const u32 offset :
         {0x44, 0x48, 0x4c, 0x50, 0x58, 0x5c, 0x60, 0x64, 0x68, 0x70, 0x74, 0x7c}) {
        if (f(projection, offset) != 0) {
            return std::nullopt;
        }
    }
    for (const u32 offset : {0x40, 0x54, 0x78}) {
        if (!std::isfinite(f(projection, offset)) || f(projection, offset) <= 0) {
            return std::nullopt;
        }
    }
    if (f(projection, 0x6c) != -1) {
        return std::nullopt;
    }
    for (u32 i = 0; i < 0xc0 / 4; ++i) {
        if (projection[i] != fragment_camera[i] ||
            !std::isfinite(std::bit_cast<f32>(projection[i]))) {
            return std::nullopt;
        }
    }
    // Both fragment VP matrices must be formed from that unjittered projection. This also
    // rejects a previous projection that changed or incorporated the previous raster jitter.
    for (const auto [view, vp] : {std::pair{0U, 0x80U}, std::pair{0x1c0U, 0x180U}}) {
        for (u32 column = 0; column < 4; ++column) {
            for (u32 row = 0; row < 4; ++row) {
                f32 expected{};
                for (u32 k = 0; k < 4; ++k) {
                    expected += f(projection, 0x40 + (k * 4 + row) * 4) *
                                f(fragment_camera, view + (column * 4 + k) * 4);
                }
                const f32 actual = f(fragment_camera, vp + (column * 4 + row) * 4);
                const f32 tolerance = std::max(1.0f, std::abs(expected)) * 0.000001f;
                if (!std::isfinite(expected) || !std::isfinite(actual) ||
                    std::abs(expected - actual) > tolerance) {
                    return std::nullopt;
                }
            }
        }
    }
    std::array<f32, 2> jitter{f(camera, 0x370) * 960.0f, f(camera, 0x374) * -540.0f};
    for (u32 i = 0; i < 4; ++i) {
        const f32 value = f(camera, 0x370 + i * 4) * (i % 2 == 0 ? 960.0f : -540.0f);
        if (!std::isfinite(value) || std::abs(value) > 0.5f) {
            return std::nullopt;
        }
    }
    return jitter;
}

constexpr u32 PrepassColorOffset = 0x278;
constexpr u32 JitterOffset = 0x1b0;
constexpr u32 OutputColorInputOffset = 0x328;
constexpr u32 OutputColorOffset = 0x348;

struct Passes {
    u64 prepass;
    u64 output;
    bool star_fox;
    u64 alternate_prepass{};

    constexpr bool IsPrepass(u64 key) const {
        return key == prepass || (alternate_prepass != 0 && key == alternate_prepass);
    }
};

constexpr std::optional<Passes> CapturedPasses(u64 program_id) {
    if (program_id == ProgramId) {
        return Passes{PrepassProgramKey, OutputProgramKey, false, AlternatePrepassProgramKey};
    }
    if (program_id == StarFoxProgramId) {
        return Passes{StarFoxPrepassProgramKey, StarFoxOutputProgramKey, true,
                      StarFoxCutscenePrepassProgramKey};
    }
    return std::nullopt;
}

struct JitterSample {
    GPUVAddr color{};
    u32 width{}, height{};
    std::array<f32, 2> offset{};
    bool reset{};
};

class FrameJitter {
public:
    void Capture(GPUVAddr color, u32 width, u32 height, std::array<f32, 2> offset,
                 bool reset = false) {
        sample.reset();
        if (color == 0 || width == 0 || height == 0 ||
            !std::isfinite(offset[0]) || !std::isfinite(offset[1]) ||
            std::abs(offset[0]) > 0.5f || std::abs(offset[1]) > 0.5f) {
            return;
        }
        sample = JitterSample{color, width, height, offset, reset};
    }

    // Consume on every output attempt, even if its resources cannot be used.
    std::optional<JitterSample> Take() {
        return std::exchange(sample, std::nullopt);
    }

private:
    std::optional<JitterSample> sample;
};

inline bool UseReplacement(u64 program_id, u64 native_key, const char* option) {
    const auto passes = CapturedPasses(program_id);
    return passes && native_key == passes->output &&
           (option == nullptr || (std::string_view{option} == "fsr" || std::string_view{option} == "dlss"));
}

inline bool UseTemporalReplacement(bool allow_history, bool nvidia_enabled, const char* option) {
    return allow_history &&
           (option ? std::string_view{option} == "fsr" || std::string_view{option} == "dlss"
                   : nvidia_enabled);
}

inline bool PlainImage(const Tegra::Texture::TICEntry& image) {
    using namespace Tegra::Texture;
    return (image.texture_type == TextureType::Texture2D ||
            image.texture_type == TextureType::Texture2DNoMipmap) && image.IsBlockLinear() &&
           image.Depth() == 1 && image.BaseLayer() == 0 &&
           image.max_mip_level == 0 && image.Address() != 0;
}

inline bool StarFoxColor(const Tegra::Texture::TICEntry& image,
                         Tegra::Texture::TextureFormat format,
                         Tegra::Texture::ComponentType component, bool srgb) {
    using namespace Tegra::Texture;
    return PlainImage(image) && image.format == format && (image.srgb_conversion != 0) == srgb &&
           image.r_type == component && image.g_type == component &&
           image.b_type == component && image.a_type == component &&
           image.x_source == SwizzleSource::R && image.y_source == SwizzleSource::G &&
           image.z_source == SwizzleSource::B && image.w_source == SwizzleSource::A &&
           image.res_min_mip_level == 0 && image.res_max_mip_level == 0 &&
           image.msaa_mode == MsaaMode::Msaa1x1 && image.block_width == 0 &&
           image.block_depth == 0 && image.is_sparse == 0;
}

inline bool ValidInput(const Tegra::Texture::TICEntry& input, bool star_fox = false) {
    using namespace Tegra::Texture;
    if (star_fox) {
        return StarFoxColor(input, TextureFormat::A8B8G8R8, ComponentType::UNORM, true);
    }
    return input.format == TextureFormat::B10G11R11 && input.IsBlockLinear();
}

/// Only the captured unscaled, single-sample motion/depth pair may feed Star Fox's host DLSS.
inline bool ValidStarFoxMotionDepth(const Tegra::Texture::TICEntry& color,
                                    const Tegra::Texture::TICEntry& motion,
                                    const Tegra::Texture::TICEntry& depth) {
    using namespace Tegra::Texture;
    const auto plain = [&](const TICEntry& image) {
        return PlainImage(image) && image.Width() == color.Width() &&
               image.Height() == color.Height() && image.res_min_mip_level == 0 &&
               image.res_max_mip_level == 0 && image.msaa_mode == MsaaMode::Msaa1x1 &&
               image.block_width == 0 && image.block_depth == 0 && image.is_sparse == 0 &&
               !image.srgb_conversion && image.Address() != color.Address();
    };
    return ValidInput(color, true) && plain(motion) && plain(depth) &&
           motion.Address() != depth.Address() && motion.format == TextureFormat::R16G16 &&
           motion.r_type == ComponentType::FLOAT && motion.g_type == ComponentType::FLOAT &&
           motion.x_source == SwizzleSource::R && motion.y_source == SwizzleSource::G &&
           depth.format == TextureFormat::Z32_X24S8;
}

inline bool ValidPair(const Tegra::Texture::TICEntry& input, const Tegra::Texture::TICEntry& output,
                      bool star_fox = false) {
    using namespace Tegra::Texture;
    if (star_fox) {
        return ValidInput(input, true) &&
               StarFoxColor(output, TextureFormat::R16G16B16A16, ComponentType::FLOAT, false) &&
               input.Address() != output.Address() && input.Width() == output.Width() &&
               input.Height() == output.Height();
    }
    return PlainImage(input) && PlainImage(output) &&
           input.format == TextureFormat::B10G11R11 && output.format == TextureFormat::B10G11R11 &&
           input.Address() != output.Address() && output.Width() > input.Width() &&
           output.Height() > input.Height();
}

// A successful substitute dispatch, not a shader rejection. Never cache it as a failure.
struct DispatchReplaced {};

} // namespace VideoCommon::FireEmblemUpscale
