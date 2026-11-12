// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace VideoCommon::OrbitalsUpscale {
constexpr unsigned long long ProgramId = 0x040004F027560000ULL;
constexpr std::size_t CameraBytes = 0x5a0;

struct Camera {
    std::array<float, 16> view_projection{};
    std::array<float, 16> projection{};
    std::array<float, 12> view{};
};

// Friends Pass v327680 tonemap CB5: UE row-vector matrices and an absolute camera
// origin. The observed local split uses unjittered infinite reverse-Z (near 10).
inline std::optional<Camera> DecodeCamera(std::span<const float> constants, unsigned player) {
    if (constants.size_bytes() < CameraBytes || player > 1)
        return std::nullopt;
    const auto at = [&](std::size_t offset) { return constants[offset / sizeof(float)]; };
    const auto close = [](double a, double b) {
        return std::isfinite(a) && std::isfinite(b) &&
               std::abs(a - b) <= 1e-5 * std::max({1.0, std::abs(a), std::abs(b)});
    };
    if (!close(at(0x4f0), 0.25) || !close(at(0x4f4), -0.5) || !close(at(0x4f8), 0.5) ||
        !close(at(0x4fc), player ? 0.75 : 0.25))
        return std::nullopt;
    Camera out;
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t col = 0; col < 4; ++col) {
            const auto offset = (col * 4 + row) * sizeof(float);
            const float p = at(0x1c0 + offset);
            if (!close(p, at(0x200 + offset)) || !close(p, at(0x560 + offset)))
                return std::nullopt;
            out.projection[row * 4 + col] = p;
            out.view_projection[row * 4 + col] = at(0x3c0 + offset);
            if (!std::isfinite(out.view_projection[row * 4 + col]))
                return std::nullopt;
            if (row < 3)
                out.view[row * 4 + col] = at(0xc0 + offset);
        }
    }
    const auto& p = out.projection;
    if (p[0] < 0.1f || p[0] > 10.f || p[5] < 0.1f || p[5] > 10.f || p[11] != 10.f || p[14] != 1.f)
        return std::nullopt;
    for (std::size_t i = 0; i < p.size(); ++i)
        if (i != 0 && i != 5 && i != 11 && i != 14 && p[i] != 0.f)
            return std::nullopt;
    for (std::size_t row = 0; row < 3; ++row) {
        if (!close(out.view[row * 4 + 3], 0))
            return std::nullopt;
        double translation{};
        for (std::size_t col = 0; col < 3; ++col)
            translation -= static_cast<double>(out.view[row * 4 + col]) * at(0x480 + col * 4);
        if (!std::isfinite(translation))
            return std::nullopt;
        out.view[row * 4 + 3] = static_cast<float>(translation);
        for (std::size_t other = 0; other < 3; ++other) {
            double dot{};
            for (std::size_t col = 0; col < 3; ++col)
                dot += static_cast<double>(out.view[row * 4 + col]) * out.view[other * 4 + col];
            if (!close(dot, row == other ? 1 : 0))
                return std::nullopt;
        }
    }
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t col = 0; col < 4; ++col) {
            double expected = p[row * 4 + 3] * (col == 3 ? 1.0 : 0.0);
            for (std::size_t k = 0; k < 3; ++k)
                expected += static_cast<double>(p[row * 4 + k]) * out.view[k * 4 + col];
            if (!close(expected, out.view_projection[row * 4 + col]))
                return std::nullopt;
        }
    }
    return out;
}
} // namespace VideoCommon::OrbitalsUpscale
