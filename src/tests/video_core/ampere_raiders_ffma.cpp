// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <bit>
#include <cmath>
#include <span>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

// Evaluate emitted Maxwell operations independently of the Ampere operand decoder.
// This deliberately checks arithmetic, rather than reproducing the lowering's bit rewrite.
struct FloatMachine {
    std::array<u32, 256> registers{};
    std::array<float, 64> constants{};
    bool predicate{};
    u32 constant_reads{};

    u32 Read(u32 reg) const { return reg == 255 ? 0 : registers.at(reg); }

    void Run(std::span<const u64> code) {
        for (const u64 word : code) {
            const auto bits = [word](u32 offset, u32 width) {
                return static_cast<u32>((word >> offset) & ((u64{1} << width) - 1));
            };
            const u32 guard = bits(16, 4);
            REQUIRE(((guard & 7) == 0 || (guard & 7) == 7));
            const bool enabled = ((guard & 7) == 7 ? true : predicate) != bool(guard & 8);
            if (!enabled)
                continue;
            const u32 dest = bits(0, 8);
            u32 value{};
            if ((word >> 48) == 0x4c98) {
                REQUIRE(bits(34, 5) == 3);
                value = std::bit_cast<u32>(constants.at(bits(20, 14)));
                ++constant_reads;
            } else if ((word >> 48) == 0x5c98) {
                value = Read(bits(20, 8));
            } else if ((word >> 56) == 0x04) {
                // LOP32I XOR is the emitted floating-point sign-bit operation.
                REQUIRE(bits(53, 2) == 2);
                REQUIRE(bits(20, 32) == 0x80000000U);
                value = Read(bits(8, 8)) ^ bits(20, 32);
            } else if ((word >> 56) == 0x59) {
                const float a = std::bit_cast<float>(Read(bits(8, 8)));
                const float b = std::bit_cast<float>(Read(bits(20, 8)));
                const float c = std::bit_cast<float>(Read(bits(39, 8)));
                value = std::bit_cast<u32>(std::fma(a, b, c));
            } else if ((word >> 48) == 0x5c68) {
                REQUIRE(bits(41, 3) == 0);
                value = std::bit_cast<u32>(std::bit_cast<float>(Read(bits(8, 8))) *
                                          std::bit_cast<float>(Read(bits(20, 8))));
            } else if ((word >> 56) == 0x01) {
                value = bits(20, 32);
            } else if ((word >> 48) == 0x5ca0) {
                const bool select_a = predicate != bool(bits(42, 1));
                REQUIRE(bits(39, 3) == 0);
                value = Read(select_a ? bits(8, 8) : bits(20, 8));
            } else {
                REQUIRE(word == 0xe30000000007000fULL);
                return;
            }
            if (dest != 255)
                registers.at(dest) = value;
        }
        FAIL("Floating-point lowering did not reach EXIT");
    }
};
} // namespace

TEST_CASE("Raiders FFMA preserves independent register and constant negation",
          "[video_core][ampere][raiders][ffma]") {
    // Native FFMA R0, R0, R7, c[3][0]: bit 75 negates R7, and bit 63 negates the constant.
    // R0 aliases an input, and R7 is also used after this operation in the guest shader.
    const Instruction form{{0x00007623, 0x00c00000, 0x00000007, 0}};
    for (const bool negate63 : {false, true}) {
        for (const bool negate75 : {false, true}) {
            for (const u32 guard : {0U, 7U, 8U}) {
                for (const bool predicate : {false, true}) {
                    auto instruction = form;
                    instruction.words[0] = (instruction.words[0] & ~0xf000U) | (guard << 12);
                    instruction.words[1] |= u32{negate63} << 31;
                    instruction.words[2] |= u32{negate75} << 11;
                    const auto lowered = Shader::Ampere::LowerToMaxwell(
                        std::array{instruction, Exit});
                    INFO(lowered.error);
                    REQUIRE(lowered);
                    for (const float source : {0.0f, 0.5f, 1.0f, 2.0f}) {
                        CAPTURE(negate63, negate75, guard, predicate, source);
                        FloatMachine machine;
                        machine.registers[0] = std::bit_cast<u32>(source);
                        machine.registers[7] = std::bit_cast<u32>(0.25f);
                        machine.constants[0] = 0.75f;
                        machine.predicate = predicate;
                        machine.Run(lowered.instructions);
                        const bool enabled = (guard == 7 ? true : predicate) != bool(guard & 8);
                        const float multiplier = negate75 ? -0.25f : 0.25f;
                        const float addend = negate63 ? -0.75f : 0.75f;
                        const float expected = enabled ? std::fma(source, multiplier, addend) : source;
                        CHECK(std::bit_cast<float>(machine.registers[0]) == expected);
                        CHECK(machine.registers[7] == std::bit_cast<u32>(0.25f));
                        CHECK(machine.constant_reads == (enabled ? 1 : 0));
                    }
                }
            }
        }
    }
}

// Captured v65536 native fragment 4bdf7dbdbe62230c, instruction offsets c0/e0.
// nvdisasm SM87 decodes FFMA R0,R0,-R7,c[3][0] followed by FSEL R0,-R0,1,P0.
// The select's explicit negation is essential: moving the FFMA negate to its constant
// produces negative linear depth in the captured scene and breaks lighting and haze.
TEST_CASE("Raiders captured FFMA and FSEL chain returns positive linear depth",
          "[video_core][ampere][raiders][ffma]") {
    const std::array code{
        Instruction{{0x00007623, 0x00c00000, 0x00010807, 0x001fc800}},
        Instruction{{0x00007a20, 0x00c00a00, 0x00410000, 0x000fca00}},
        Instruction{{0x00007808, 0x3f800000, 0x00000100, 0x000fc800}},
        Exit,
    };
    const auto lowered = Shader::Ampere::LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(lowered);
    constexpr float near_plane = 0.1f;
    constexpr float reciprocal_range = 0.00010000099427998066f;
    constexpr float projection_scale = -0.9999899864196777f;
    for (const float depth : {0.f, 0.5f, 0.976052f, 0.988462f, 0.999951f, 1.f}) {
        CAPTURE(depth);
        const float reciprocal = 1.f / std::fma(depth, projection_scale, 1.f);
        FloatMachine machine;
        machine.registers[0] = std::bit_cast<u32>(reciprocal);
        machine.registers[7] = std::bit_cast<u32>(near_plane);
        machine.constants[0] = near_plane;
        machine.constants[0x28 / 4] = reciprocal_range;
        machine.predicate = depth != 1.f;
        machine.Run(lowered.instructions);
        const float result = std::bit_cast<float>(machine.registers[0]);
        const float expected = depth == 1.f ? 1.f :
            std::fma(reciprocal, near_plane, -near_plane) * reciprocal_range;
        CHECK(std::isfinite(result));
        CHECK(result >= 0.f);
        CHECK(std::abs(result - expected) < 1e-7f);
        CHECK(machine.registers[7] == std::bit_cast<u32>(near_plane));
    }
}
