// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0x000fea00}};
constexpr Instruction MinMax{{0x0b037817, 0x1fffffff, 0x03800000, 0x001fc800}};
constexpr Instruction Reduction{{0x0500759f, 0x20c00206, 0x0010e000, 0x000fe200}};
}

TEST_CASE("Yoshi captured IMNMX preserves full-width immediates", "[ampere][yoshi]") {
    for (const u32 value : {0x80000U, 0x1fffffffU, 0x7fffffffU, 0x80000000U, 0xfff7ffffU}) {
        for (const u32 guard : {0U, 7U, 9U}) {
            for (const u32 mode : {0U, 0x200U, 0x04000000U, 0x04000200U}) {
                auto instruction = MinMax;
                instruction.words[0] = (instruction.words[0] & ~0xf000U) | (guard << 12);
                instruction.words[1] = value;
                instruction.words[2] |= mode;
                const auto lowered = LowerToMaxwell(std::array{instruction, Exit});
                INFO(lowered.error);
                REQUIRE(lowered);
                REQUIRE(lowered.instructions.size() == 3);
                CHECK(lowered.instructions[0] ==
                      (0x010000000000f0fbULL | (u64{guard} << 16) | (u64{value} << 20)));
                CHECK(lowered.instructions[1] ==
                      (0x5c2000000fb00b03ULL | (u64{guard} << 16) |
                       (u64{instruction.Bits(87, 4)} << 39) |
                       (u64{instruction.Bits(73, 1)} << 48)));
                CHECK(lowered.source_size == 32);
            }
        }
    }
}

TEST_CASE("Ampere IMNMX retains its signed twenty-bit immediate encoding", "[ampere][yoshi]") {
    for (const u32 value : {0U, 0x7ffffU, 0xfff80000U, 0xffffffffU}) {
        auto instruction = MinMax;
        instruction.words[1] = value;
        const auto lowered = LowerToMaxwell(std::array{instruction, Exit});
        INFO(lowered.error);
        REQUIRE(lowered);
        REQUIRE(lowered.instructions.size() == 2);
        CHECK(lowered.instructions[0] ==
              (0x3820038000070b03ULL | (u64{value & 0x7ffffU} << 20) |
               (u64{value >> 31} << 56)));
    }
    auto unsupported = MinMax;
    unsupported.words[2] |= 0x400;
    REQUIRE_FALSE(LowerToMaxwell(std::array{unsupported, Exit}));
}

TEST_CASE("Yoshi captured SURED preserves its dimension and descriptor", "[ampere][yoshi]") {
    for (const u32 dimension : {1U, 3U, 5U}) {
        for (const u32 guard : {0U, 7U, 9U}) {
            auto instruction = Reduction;
            instruction.words[0] = (instruction.words[0] & ~0xf000U) | (guard << 12);
            instruction.words[1] = (instruction.words[1] & 0x1fffffffU) | (dimension << 29);
            const auto lowered = LowerToMaxwell(std::array{instruction, Exit});
            INFO(lowered.error);
            REQUIRE(lowered);
            REQUIRE(lowered.instructions.size() == 2);
            CHECK(lowered.required_texture_buffer == 3);
            CHECK(lowered.instructions[0] ==
                  (0xeb48002000000506ULL | (u64{guard} << 16) | (u64{dimension} << 33)));
        }
    }
    for (const u32 dimension : {0U, 2U, 4U, 6U, 7U}) {
        auto instruction = Reduction;
        instruction.words[1] = (instruction.words[1] & 0x1fffffffU) | (dimension << 29);
        REQUIRE_FALSE(LowerToMaxwell(std::array{instruction, Exit}));
    }
    auto unsupported = Reduction;
    unsupported.words[2] |= 0x200;
    REQUIRE_FALSE(LowerToMaxwell(std::array{unsupported, Exit}));
}

TEST_CASE("Yoshi 3D surface coordinates stay outside lowering scratch registers",
          "[ampere][yoshi]") {
    const Instruction uniform_marker{{0x0000fc02, 0, 0, 0}};
    const auto reduction = [](u32 coordinate) {
        auto instruction = Reduction;
        instruction.words[0] = (instruction.words[0] & 0x00ffffffU) | (coordinate << 24);
        instruction.words[1] = (instruction.words[1] & 0x1fffffffU) | (5U << 29);
        return instruction;
    };
    const auto plain = LowerToMaxwell(std::array{reduction(243), Exit});
    INFO(plain.error);
    REQUIRE(plain);
    CHECK(((plain.instructions.front() >> 8) & 0xff) == 243);
    const auto plain_overlap = LowerToMaxwell(std::array{reduction(244), Exit});
    REQUIRE_FALSE(plain_overlap);
    CHECK(plain_overlap.error == "native surface reduction mode");

    const auto uniform = LowerToMaxwell(std::array{uniform_marker, reduction(213), Exit});
    INFO(uniform.error);
    REQUIRE(uniform);
    CHECK(((uniform.instructions[uniform.source_instruction_offsets[1]] >> 8) & 0xff) == 213);
    const auto uniform_overlap =
        LowerToMaxwell(std::array{uniform_marker, reduction(214), Exit});
    REQUIRE_FALSE(uniform_overlap);
    CHECK(uniform_overlap.error == "native surface reduction mode");
}

TEST_CASE("Yoshi 3D surface register shadows include the depth coordinate",
          "[ampere][yoshi]") {
    auto instruction = Reduction;
    instruction.words[1] = (instruction.words[1] & 0x1fffffffU) | (5U << 29);
    instruction.words[0] = (instruction.words[0] & 0x00ffffffU) | (218U << 24);
    CHECK_FALSE(Shader::Ampere::PrepareRegisterPressure(instruction, 216).valid);

    instruction.words[0] = (instruction.words[0] & 0x00ffffffU) | (220U << 24);
    const auto pressure = Shader::Ampere::PrepareRegisterPressure(instruction, 216);
    REQUIRE(pressure.valid);
    const u32 coordinate = pressure.instruction.Bits(24, 8);
    CHECK(coordinate + 3 <= 216);
    for (u32 i = 0; i < 3; ++i) {
        CHECK(std::find(pressure.prefix.begin(), pressure.prefix.end(),
                        Shader::Ampere::NativeRegisterCopy(0, coordinate + i, 1, 220 + i)) !=
              pressure.prefix.end());
    }

    // Shadowing the data must not borrow the low quartet containing the third coordinate.
    instruction.words[0] = (instruction.words[0] & 0x00ffffffU) | (210U << 24);
    instruction.words[1] = (instruction.words[1] & ~0xffU) | 240;
    const auto data_pressure = Shader::Ampere::PrepareRegisterPressure(instruction, 216);
    REQUIRE(data_pressure.valid);
    CHECK(data_pressure.instruction.Bits(24, 8) == 210);
    const u32 data = data_pressure.instruction.Bits(32, 8);
    CHECK(data / 4 != 212 / 4);
}
