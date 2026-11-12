// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include "common/common_types.h"
namespace VideoCommon::YoshiUpscale {
constexpr u64 PROGRAM_ID = 0x0400245021D2C000ULL;
constexpr u64 MOTION_NATIVE = 0x01fae3e0cd6781c3ULL;
constexpr u64 MOTION_SHADER = 0x505d41b4f1088151ULL;
constexpr u64 TEMPORAL_NATIVE = 0x85237f4449a5ebe7ULL;
constexpr u64 TEMPORAL_SHADER = 0x7a6e67d9226d0b53ULL;
struct Contract {
    std::array<f32, 2> jitter;
    bool reset;
};
// Exact observed gameplay variant. Other resolutions/passes retain their native output.
inline std::optional<Contract> Decode(std::span<const u8> motion, std::span<const u8> temporal) {
    if (motion.size() < 64 || temporal.size() < 608)
        return std::nullopt;
    const auto u = [](std::span<const u8> data, size_t at) {
        u32 value;
        std::memcpy(&value, data.data() + at, sizeof(value));
        return value;
    };
    const auto f = [](std::span<const u8> data, size_t at) {
        f32 value;
        std::memcpy(&value, data.data() + at, sizeof(value));
        return value;
    };
    const auto close = [](f32 actual, f32 expected) {
        return std::isfinite(actual) && std::abs(actual - expected) < 1e-6f;
    };
    if (u(motion, 0x10) || u(motion, 0x14) || u(motion, 0x18) != 960 || u(motion, 0x1c) != 540 ||
        u(motion, 0x28) || u(motion, 0x2c))
        return std::nullopt;
    constexpr std::array<std::pair<size_t, f32>, 8> motion_values{{{0x8, 1.f / 960},
                                                                   {0xc, 1.f / 540},
                                                                   {0x20, 960},
                                                                   {0x24, 540},
                                                                   {0x30, 1920},
                                                                   {0x34, 1080},
                                                                   {0x38, 1.f / 1920},
                                                                   {0x3c, 1.f / 1080}}};
    for (auto [at, expected] : motion_values)
        if (!close(f(motion, at), expected))
            return std::nullopt;
    constexpr std::array<u32, 12> rects{0, 0, 959, 539, 0, 0, 1919, 1079, 0, 0, 1920, 1080};
    for (size_t i = 0; i < rects.size(); ++i)
        if (u(temporal, 0x1c0 + 4 * i) != rects[i])
            return std::nullopt;
    constexpr std::array<std::pair<size_t, f32>, 10> temporal_values{{{0x160, .5f},
                                                                      {0x164, .5f},
                                                                      {0x168, 0},
                                                                      {0x16c, 0},
                                                                      {0x188, 1},
                                                                      {0x18c, 1},
                                                                      {0x198, 2},
                                                                      {0x19c, 2},
                                                                      {0x1f8, 1.f / 960},
                                                                      {0x1fc, 1.f / 540}}};
    for (auto [at, expected] : temporal_values)
        if (!close(f(temporal, at), expected))
            return std::nullopt;
    Contract result{{f(motion, 0), f(motion, 4)}, temporal[0x180] != 0};
    for (size_t i = 0; i < 2; ++i)
        if (!std::isfinite(result.jitter[i]) || std::abs(result.jitter[i]) > .5f ||
            !close(f(temporal, 0x170 + i * 4) + .5f, result.jitter[i]))
            return std::nullopt;
    return result;
}
} // namespace VideoCommon::YoshiUpscale
