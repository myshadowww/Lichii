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
    float constant{};
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
                REQUIRE(bits(20, 14) == 0);
                value = std::bit_cast<u32>(constant);
                ++constant_reads;
            } else if ((word >> 48) == 0x5c98) {
                value = Read(bits(20, 8));
            } else if ((word >> 56) == 0x04) {
                // LOP32I XOR is the emitted floating-point sign-bit operation.
                REQUIRE(bits(53, 2) == 2);
                REQUIRE(bits(20, 32) == 0x80000000U);
                value = Read(bits(8, 8)) ^ bits(20, 32);
            } else if ((word >> 48) == 0x5980) {
                const float a = std::bit_cast<float>(Read(bits(8, 8)));
                const float b = std::bit_cast<float>(Read(bits(20, 8)));
                const float c = std::bit_cast<float>(Read(bits(39, 8)));
                value = std::bit_cast<u32>(std::fma(a, b, c));
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

TEST_CASE("Raiders FFMA constant addend negation preserves depth sign and register operands",
          "[video_core][ampere][raiders][ffma]") {
    // Native FFMA R0, R0, R7, c[3][0]: the near-plane subtraction used by Raiders.
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
                        machine.constant = 0.25f;
                        machine.predicate = predicate;
                        machine.Run(lowered.instructions);
                        const bool enabled = (guard == 7 ? true : predicate) != bool(guard & 8);
                        const float addend = negate63 != negate75 ? -0.25f : 0.25f;
                        const float expected = enabled ? std::fma(source, 0.25f, addend) : source;
                        CHECK(std::bit_cast<float>(machine.registers[0]) == expected);
                        CHECK(machine.registers[7] == std::bit_cast<u32>(0.25f));
                        CHECK(machine.constant_reads == (enabled ? 1 : 0));
                    }
                }
            }
        }
    }
}
