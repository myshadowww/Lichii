// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace VideoCommon::KirbyUpscale {
inline constexpr std::uint64_t PROGRAM_ID = 0x040077201BE50000ULL;
// Native command corruption still prevents reliable moving-gameplay validation.
inline constexpr bool GAMEPLAY_VALIDATED = false;
inline constexpr std::uint64_t TONEMAP_VERTEX = 0x38d4fe54627a98bfULL;
inline constexpr std::uint64_t TONEMAP_FRAGMENT = 0xbb6be8093b607898ULL;
inline constexpr std::uint64_t SMAA_VERTEX = 0xeb21e6be1d399ddaULL;
inline constexpr std::uint64_t SMAA_FRAGMENT = 0xf2eb2333f228f72cULL;
struct NetworkKey {
    std::uint64_t native, translated;
};
// Captured native NDSR variants all decode sRGB before their linear A2 stores.
inline constexpr std::array NETWORKS{
    NetworkKey{0x29e48ad9f9dee2e9ULL, 0xcefb81a8c8f8a9bdULL},
    NetworkKey{0xb5455397e0db120eULL, 0xd9dce89be4f3e1f7ULL},
    NetworkKey{0xb7edffe0d33047cbULL, 0xd7d3aa138eb605eeULL},
    NetworkKey{0xb68938854f6ac47cULL, 0x482f2ead53e3faabULL},
    NetworkKey{0x8be483aa35a62abaULL, 0x56a85b3f3a55859eULL},
    NetworkKey{0xa80e3f0ea154c598ULL, 0x6fb08a2c546114caULL},
};
inline bool IsNetworkShader(std::uint64_t shader) {
    return std::ranges::any_of(NETWORKS, [&](const auto& key) { return key.translated == shader; });
}
inline bool IsNetwork(std::uint64_t native, std::uint64_t shader) {
    return std::ranges::any_of(NETWORKS, [&](const auto& key) {
        return key.native == native && key.translated == shader;
    });
}
struct Camera {
    std::array<float, 16> view_projection{};
    std::array<float, 16> projection{};
    std::array<float, 12> view{};
    std::array<std::int32_t, 3> origin{};
};

// Native VS 84e8f6c7a914adc8/e74a14f56242e348 subtract the integer origin and
// floating camera position before applying CB3+0xe0. The same frame's tonemap
// CB4 repeats the first 0x1f0 bytes of this camera, linking it to sampled depth.
// reference_origin must remain fixed throughout a temporal history interval.
inline std::optional<Camera> DecodeCamera(std::span<const std::byte> cb,
                                          const std::array<std::int32_t, 3>& reference_origin,
                                          std::uint32_t width, std::uint32_t height) {
    if (cb.size() < 0x210 || !width || !height)
        return std::nullopt;
    const auto scalar = [&](std::size_t offset) {
        float value;
        std::memcpy(&value, cb.data() + offset, sizeof(value));
        return value;
    };
    if (scalar(0x1e0) != static_cast<float>(width) || scalar(0x1e4) != static_cast<float>(height) ||
        !std::isfinite(scalar(0x1e8)) || std::abs(scalar(0x1e8) - 0.1f) > 1e-6f ||
        scalar(0x1ec) != 20000.0f)
        return std::nullopt;
    const auto close = [](double a, double b) {
        return std::isfinite(a) && std::isfinite(b) &&
               std::abs(a - b) <= 1e-5 * std::max({1.0, std::abs(a), std::abs(b)});
    };
    if (!close(scalar(0x1f0), 1.0 / width) || !close(scalar(0x1f4), 1.0 / height))
        return std::nullopt;
    Camera out;
    std::array<double, 9> inverse_rotation{};
    std::array<double, 3> position{};
    for (std::size_t row = 0; row < 3; ++row) {
        std::memcpy(&out.origin[row], cb.data() + 0x200 + row * 4, sizeof(out.origin[row]));
        // Subtract in integer precision before adding the fractional camera position.
        const auto cell_delta = static_cast<std::int64_t>(out.origin[row]) - reference_origin[row];
        position[row] = static_cast<double>(cell_delta) + scalar(0x3c + row * 16);
        // A caller can choose a new nearby reference and explicitly reset history.
        if (!std::isfinite(position[row]) || std::abs(position[row]) > 65536.0)
            return std::nullopt;
        for (std::size_t col = 0; col < 3; ++col) {
            inverse_rotation[row * 3 + col] = scalar(0x30 + row * 16 + col * 4);
            if (!std::isfinite(inverse_rotation[row * 3 + col]))
                return std::nullopt;
        }
    }
    const auto& r = inverse_rotation;
    for (std::size_t a = 0; a < 3; ++a) {
        for (std::size_t b = 0; b < 3; ++b) {
            double dot{};
            for (std::size_t k = 0; k < 3; ++k)
                dot += r[a * 3 + k] * r[b * 3 + k];
            if (!close(dot, a == b ? 1.0 : 0.0))
                return std::nullopt;
        }
    }
    const double determinant = r[0] * (r[4] * r[8] - r[5] * r[7]) -
                               r[1] * (r[3] * r[8] - r[5] * r[6]) +
                               r[2] * (r[3] * r[7] - r[4] * r[6]);
    if (!close(determinant, 1.0))
        return std::nullopt;
    const double near = scalar(0x1e8), far = scalar(0x1ec);
    std::array<double, 16> expected{};
    expected[0] = scalar(0x60);
    expected[5] = scalar(0x74);
    expected[10] = near / (far - near);
    expected[11] = near * far / (far - near);
    expected[14] = -1.0;
    if (expected[0] <= 0.0 || expected[5] <= 0.0)
        return std::nullopt;
    for (std::size_t i = 0; i < 16; ++i) {
        const float value = scalar(0x60 + i * 4);
        // Zero off-axis terms prove the captured unjittered, symmetric frustum.
        if (!std::isfinite(value) ||
            std::abs(value - expected[i]) > 1e-6 * std::abs(expected[i]) + 1e-9 ||
            (expected[i] == 0.0 && value != 0.0f))
            return std::nullopt;
        out.projection[i] = value;
    }
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t col = 0; col < 4; ++col) {
            double identity{}, clip{};
            for (std::size_t k = 0; k < 4; ++k) {
                identity += static_cast<double>(out.projection[row * 4 + k]) *
                            scalar(0xa0 + (k * 4 + col) * 4);
                const double rotation =
                    k < 3 && col < 3 ? r[col * 3 + k] : (k == 3 && col == 3 ? 1.0 : 0.0);
                clip += out.projection[row * 4 + k] * rotation;
            }
            if (!close(identity, row == col ? 1.0 : 0.0) ||
                !close(clip, scalar(0xe0 + (row * 4 + col) * 4)))
                return std::nullopt;
        }
    }
    for (std::size_t row = 0; row < 3; ++row) {
        double translation{};
        for (std::size_t col = 0; col < 3; ++col) {
            out.view[row * 4 + col] = static_cast<float>(r[col * 3 + row]);
            translation -= r[col * 3 + row] * position[col];
        }
        out.view[row * 4 + 3] = static_cast<float>(translation);
    }
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t col = 0; col < 4; ++col) {
            double value = out.projection[row * 4 + 3] * (col == 3 ? 1.0 : 0.0);
            for (std::size_t k = 0; k < 3; ++k)
                value += static_cast<double>(out.projection[row * 4 + k]) * out.view[k * 4 + col];
            out.view_projection[row * 4 + col] = static_cast<float>(value);
        }
    }
    return out;
}
} // namespace VideoCommon::KirbyUpscale
