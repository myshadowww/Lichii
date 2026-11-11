// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace VideoCommon::MarioKartUpscale {
// Native HDRCompose keeps a 2560x1440 output while the scene can reach that full size.
inline bool InputExtentSupported(unsigned width, unsigned height) {
    return width >= 1280 && width <= 2560 && height >= 720 && height <= 1440 &&
           std::abs(static_cast<float>(width) / height - 16.f / 9.f) <= 0.015f;
}

struct Camera {
    std::array<float, 12> view;
    std::array<float, 16> view_projection;
    std::array<float, 16> projection;
};
struct Curvature {
    float center_x;
    float center_z;
    float threshold;
    float radius;
};

struct Transfer {
    std::array<float, 4> luminance;
    std::array<float, 4> shoulder;
};
// Captured daytime/sunset/Bowser share this shoulder; exposure varies with lighting.
inline std::optional<Transfer> DecodeTransfer(const std::array<float, 24>& cb) {
    if (!std::isfinite(cb[0]) || !std::isfinite(cb[1]) || cb[0] < .25f || cb[0] > 1.f ||
        cb[1] < 0 || cb[1] > 1.f || cb[2] != .15f || cb[3] != 1 || cb[4] != .75f ||
        cb[5] != -5.7707800865f || cb[6] != 2.3280851841f || cb[23] != 1)
        return std::nullopt;
    return Transfer{{cb[0], cb[1], cb[2], cb[3]}, {cb[4], cb[5], cb[6], cb[23]}};
}

// Native vehicle VS4552c6a6f5014b6c reads CB4 view, VP and projection at0,0x30,0x70.
// The captured Free Roam camera is deliberately off-centre; P[6]=-0.18 is not jitter.
inline std::optional<Camera> DecodeCamera(const std::array<float, 44>& cb4) {
    if (!std::all_of(cb4.begin(), cb4.end(), [](float x) { return std::isfinite(x); }))
        return std::nullopt;
    Camera out;
    std::copy_n(cb4.begin(), 12, out.view.begin());
    std::copy_n(cb4.begin() + 12, 16, out.view_projection.begin());
    std::copy_n(cb4.begin() + 28, 16, out.projection.begin());
    const auto& p = out.projection;
    if (p[0] < 0.1f || p[0] > 10.f || p[5] < 0.1f || p[5] > 10.f ||
        std::abs(p[5] / p[0] - 16.f / 9.f) > 1e-4f || std::abs(p[6] + 0.18f) > 1e-6f ||
        p[14] != -1.f)
        return std::nullopt;
    for (size_t i = 0; i < 16; ++i) {
        if (i != 0 && i != 5 && i != 6 && i != 10 && i != 11 && i != 14 && p[i] != 0.f)
            return std::nullopt;
    }
    const double near = static_cast<double>(p[11]) / (p[10] - 1.0);
    const double far = static_cast<double>(p[11]) / (p[10] + 1.0);
    if (!std::isfinite(near) || !std::isfinite(far) || std::abs(near - 1.0) > 1e-4 ||
        std::abs(far - 10000.0) > 10.0)
        return std::nullopt;
    const auto& v = out.view;
    for (size_t a = 0; a < 3; ++a) {
        for (size_t b = 0; b < 3; ++b) {
            double dot{};
            for (size_t k = 0; k < 3; ++k)
                dot += static_cast<double>(v[a * 4 + k]) * v[b * 4 + k];
            if (std::abs(dot - (a == b ? 1.0 : 0.0)) > 1e-5)
                return std::nullopt;
        }
    }
    const double determinant = v[0] * (v[5] * v[10] - v[6] * v[9]) -
                               v[1] * (v[4] * v[10] - v[6] * v[8]) +
                               v[2] * (v[4] * v[9] - v[5] * v[8]);
    if (std::abs(determinant - 1.0) > 1e-5)
        return std::nullopt;
    for (size_t row = 0; row < 4; ++row) {
        for (size_t col = 0; col < 4; ++col) {
            double composed = p[row * 4 + 3] * (col == 3 ? 1.0 : 0.0);
            for (size_t k = 0; k < 3; ++k)
                composed += static_cast<double>(p[row * 4 + k]) * v[k * 4 + col];
            if (std::abs(composed - out.view_projection[row * 4 + col]) >
                1e-5 * std::max(1.0, std::abs(composed)))
                return std::nullopt;
        }
    }
    return out;
}

inline std::optional<Curvature> DecodeCurvature(const std::array<float, 4>& words) {
    if (!std::all_of(words.begin(), words.end(), [](float x) { return std::isfinite(x); }) ||
        std::abs(words[0]) > 1e7f || std::abs(words[1]) > 1e7f ||
        std::abs(words[2] - 500.f) > 1e-3f || std::abs(words[3] - 20000.f) > 1e-2f)
        return std::nullopt;
    return Curvature{words[0], words[1], words[2], words[3]};
}

// Reference for the native pre-camera bend. Outside the unique inverse domain, callers
// must reject the sample rather than treating curved coordinates as ordinary world space.
inline std::optional<std::array<double, 3>> Bend(const std::array<double, 3>& world,
                                                 const Curvature& c) {
    if (!std::all_of(world.begin(), world.end(), [](double x) { return std::isfinite(x); }))
        return std::nullopt;
    const double dx = world[0] - c.center_x, dz = world[2] - c.center_z;
    const double distance = std::hypot(dx, dz);
    if (distance <= c.threshold)
        return world;
    const double angle = (distance - c.threshold) / c.radius;
    if (world[1] + c.radius <= 0 || angle <= 0 || angle >= 1.5707963267948966)
        return std::nullopt;
    const double radial = c.threshold + (world[1] + c.radius) * std::sin(angle);
    return std::array<double, 3>{c.center_x + dx / distance * radial,
                                 (world[1] + c.radius) * std::cos(angle) - c.radius,
                                 c.center_z + dz / distance * radial};
}

inline std::optional<std::array<double, 3>> Unbend(const std::array<double, 3>& curved,
                                                   const Curvature& c) {
    if (!std::all_of(curved.begin(), curved.end(), [](double x) { return std::isfinite(x); }))
        return std::nullopt;
    const double dx = curved[0] - c.center_x, dz = curved[2] - c.center_z;
    const double radial = std::hypot(dx, dz);
    if (radial <= c.threshold)
        return curved;
    const double shifted_y = curved[1] + c.radius;
    if (shifted_y <= 0)
        return std::nullopt;
    const double shifted_radial = radial - c.threshold;
    const double angle = std::atan2(shifted_radial, shifted_y);
    const double distance = c.threshold + c.radius * angle;
    return std::array<double, 3>{c.center_x + dx / radial * distance,
                                 std::hypot(shifted_radial, shifted_y) - c.radius,
                                 c.center_z + dz / radial * distance};
}
} // namespace VideoCommon::MarioKartUpscale
