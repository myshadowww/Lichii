// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>

namespace VideoCommon::PokopiaUpscale {
struct Camera {
    std::array<float, 16> view_projection{};
    std::array<float, 16> projection{};
    std::array<float, 12> view{};
};

// Native VS 7e10ae5e824da84e: CB3[0:0x40] is a column-major camera VP;
// CB3[0x70:0xa0] independently transforms the object to world coordinates.
// Four captures establish a rigid, symmetric, unjittered reverse-Z camera (near10/far18000).
// Recover its unique perspective/view factorization and reject every unsupported layout.
inline std::optional<Camera> DecodeCamera(const std::array<float, 16>& column_major_vp) {
    Camera out;
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t col = 0; col < 4; ++col) {
            const float value = column_major_vp[col * 4 + row];
            if (!std::isfinite(value)) return std::nullopt;
            out.view_projection[row * 4 + col] = value;
        }
    }
    const auto& vp = out.view_projection;
    const auto norm = [&](std::size_t row) {
        double squared{};
        for (std::size_t col = 0; col < 3; ++col)
            squared += static_cast<double>(vp[row * 4 + col]) * vp[row * 4 + col];
        return std::sqrt(squared);
    };
    const double fx = norm(0), fy = norm(1);
    if (fx < 0.1 || fx > 10 || fy < 0.1 || fy > 10 || std::abs(norm(3) - 1) > 1e-5)
        return std::nullopt;
    for (std::size_t col = 0; col < 4; ++col) {
        out.view[col] = static_cast<float>(vp[col] / fx);
        out.view[4 + col] = static_cast<float>(vp[4 + col] / fy);
        out.view[8 + col] = -vp[12 + col];
    }
    // This also rejects jitter/asymmetric frusta: the X/Y VP rows must be perpendicular
    // to the view-Z row. Never silently erase a guest projection offset.
    for (std::size_t a = 0; a < 3; ++a) {
        for (std::size_t b = 0; b < 3; ++b) {
            double dot{};
            for (std::size_t col = 0; col < 3; ++col)
                dot += static_cast<double>(out.view[a * 4 + col]) * out.view[b * 4 + col];
            if (std::abs(dot - (a == b ? 1.0 : 0.0)) > 1e-5) return std::nullopt;
        }
    }
    const auto& v = out.view;
    const double determinant = v[0]*(v[5]*v[10]-v[6]*v[9]) -
        v[1]*(v[4]*v[10]-v[6]*v[8]) + v[2]*(v[4]*v[9]-v[5]*v[8]);
    if (std::abs(determinant - 1) > 1e-5) return std::nullopt;
    double a{}, denominator{};
    for (std::size_t col = 0; col < 3; ++col) {
        a += static_cast<double>(vp[8 + col]) * v[8 + col];
        denominator += static_cast<double>(v[8 + col]) * v[8 + col];
    }
    a /= denominator;
    const double b = vp[11] - a * v[11];
    if (a <= 0 || b <= 0 || std::abs(b / (a + 1) - 10) > 0.01 ||
        std::abs(b / a - 18000) > 10) return std::nullopt;
    out.projection[0] = static_cast<float>(fx);
    out.projection[5] = static_cast<float>(fy);
    out.projection[10] = static_cast<float>(a);
    out.projection[11] = static_cast<float>(b);
    out.projection[14] = -1;
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t col = 0; col < 4; ++col) {
            double composed = out.projection[row * 4 + 3] * (col == 3 ? 1.0 : 0.0);
            for (std::size_t k = 0; k < 3; ++k)
                composed += static_cast<double>(out.projection[row * 4 + k]) * v[k * 4 + col];
            if (std::abs(composed - vp[row * 4 + col]) > 1e-5 * std::max(1.0, std::abs(composed)))
                return std::nullopt;
        }
    }
    return out;
}
} // namespace VideoCommon::PokopiaUpscale
