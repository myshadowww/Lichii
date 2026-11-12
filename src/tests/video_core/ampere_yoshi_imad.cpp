// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <span>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/trap_guards.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};
constexpr std::array<Instruction, 3> Logged{{
    {{0xff117624, 0x81000400, 0x078e02ff, 0x000fe200}},
    {{0xff0cb624, 0x81000400, 0x078e02ff, 0x000fe400}},
    {{0xff04c624, 0x81000400, 0x078e02ff, 0x000fe200}},
}};

// Execute the small Maxwell integer subset these lowerings use, independently of native IMAD.
// XMAD's half selectors and unsigned multiply-add follow integer_short_multiply_add.cpp.
struct IntegerMachine {
    std::array<u32, 256> registers;
    std::array<bool, 7> predicates{};
    u32 constant{};
    u32 constant_reads{};

    IntegerMachine() { registers.fill(0xdeadbeefU); }

    u32 Read(u32 reg) const { return reg == 255 ? 0 : registers.at(reg); }
    bool Predicate(u32 guard) const {
        const bool value = (guard & 7) == 7 ? true : predicates.at(guard & 7);
        return value != bool(guard & 8);
    }

    void Run(std::span<const u64> code) {
        for (const u64 word : code) {
            const auto bits = [word](u32 offset, u32 width) {
                return static_cast<u32>((word >> offset) & ((u64{1} << width) - 1));
            };
            if (!Predicate(bits(16, 4)))
                continue;
            const u32 dest = bits(0, 8), a = bits(8, 8), b = bits(20, 8);
            u32 value{};
            if ((word >> 48) == 0x4c98) {
                REQUIRE(bits(34, 5) == 4);
                REQUIRE(bits(20, 14) * 4 == 0x10);
                ++constant_reads;
                value = constant;
            } else if ((word >> 48) == 0x5c98) {
                value = Read(b);
            } else if ((word >> 56) == 1) {
                value = bits(20, 32);
            } else if (((word >> 48) & 0xfffc) == 0x5c10) {
                const u32 left = bits(49, 1) ? 0U - Read(a) : Read(a);
                const u32 right = bits(48, 1) ? 0U - Read(b) : Read(b);
                value = left + right;
            } else if (((word >> 48) & 0xfffe) == 0x5c18) {
                value = (Read(a) << bits(39, 5)) + Read(b);
            } else if ((word >> 56) == 0x5b) {
                REQUIRE(bits(48, 2) == 0); // Unsigned halves
                REQUIRE(bits(50, 3) == 0); // Full-width addend
                REQUIRE(bits(36, 2) == 0); // No product shift or merge
                const u32 left = (Read(a) >> (bits(53, 1) * 16)) & 0xffff;
                const u32 right = (Read(b) >> (bits(35, 1) * 16)) & 0xffff;
                value = left * right + Read(bits(39, 8));
            } else {
                REQUIRE(word == 0xe30000000007000fULL);
                return;
            }
            if (dest != 255)
                registers.at(dest) = value;
        }
        FAIL("Integer lowering did not reach EXIT");
    }
};

constexpr std::array<u32, 5> Constants{0, 1, 0x7fffffff, 0x80000000, 0xffffffff};
} // namespace

TEST_CASE("Yoshi logged negated constant IMAD preserves predicates and integer negation",
          "[video_core][ampere][yoshi][imad]") {
    for (const auto& instruction : Logged) {
        const auto lowered = LowerToMaxwell(std::array{instruction, Exit});
        INFO(lowered.error << " at " << lowered.error_offset);
        REQUIRE(lowered);
        for (const u32 constant : Constants) {
            for (const bool predicate_value : {false, true}) {
                CAPTURE(instruction.words[0], constant, predicate_value);
                IntegerMachine machine;
                machine.constant = constant;
                machine.predicates.fill(predicate_value);
                const auto before = machine.registers;
                const u32 dest = instruction.Bits(16, 8);
                const bool execute = machine.Predicate(instruction.Bits(12, 4));
                machine.Run(lowered.instructions);
                CHECK(machine.registers[dest] == (execute ? 0U - constant : before[dest]));
                CHECK(machine.constant_reads == (execute ? 1U : 0U));
                for (u32 reg = 0; reg < 32; ++reg)
                    if (reg != dest)
                        CHECK(machine.registers[reg] == before[reg]);
            }
        }
    }
}

TEST_CASE("Yoshi zero-product IMAD negates the constant on the move-shaped form",
          "[video_core][ampere][yoshi][imad]") {
    auto instruction = Logged.front();
    instruction.words[2] = 0x078e00ff;
    const auto lowered = LowerToMaxwell(std::array{instruction, Exit});
    INFO(lowered.error << " at " << lowered.error_offset);
    REQUIRE(lowered);
    for (const u32 constant : Constants) {
        IntegerMachine machine;
        machine.constant = constant;
        machine.Run(lowered.instructions);
        CHECK(machine.registers[17] == 0U - constant);
        CHECK(machine.constant_reads == 1);
    }
}

TEST_CASE("Yoshi negated IMAD addend follows modulo arithmetic with aliased operands",
          "[video_core][ampere][yoshi][imad]") {
    for (const u32 dest : {17U, 2U, 3U}) {
        auto instruction = Logged.front();
        instruction.words[0] = (2U << 24) | (dest << 16) | 0x7624;
        instruction.words[2] = (instruction.words[2] & ~0xffU) | 3;
        const auto lowered = LowerToMaxwell(std::array{instruction, Exit});
        INFO(lowered.error << " at " << lowered.error_offset);
        REQUIRE(lowered);
        for (const auto [a, b] : std::array{
                 std::pair{0U, 0U}, std::pair{0xffffffffU, 0xffffffffU},
                 std::pair{0x12345678U, 0x9abcdef0U}, std::pair{0x80000000U, 3U}}) {
            for (const u32 constant : Constants) {
                CAPTURE(dest, a, b, constant);
                IntegerMachine machine;
                machine.registers[2] = a;
                machine.registers[3] = b;
                machine.constant = constant;
                machine.Run(lowered.instructions);
                CHECK(machine.registers[dest] == static_cast<u32>(u64{a} * b) - constant);
                CHECK(machine.constant_reads == 1);
                if (dest != 2)
                    CHECK(machine.registers[2] == a);
                if (dest != 3)
                    CHECK(machine.registers[3] == b);
            }
        }
    }
}

TEST_CASE("Yoshi IMAD retains rejection of an absolute constant addend",
          "[video_core][ampere][yoshi][imad]") {
    for (const u32 mode : {0x078e00ffU, 0x078e02ffU}) {
        auto instruction = Logged.front();
        instruction.words[1] |= 1U << 30;
        instruction.words[2] = mode;
        CHECK_FALSE(LowerToMaxwell(std::array{instruction, Exit}));
    }
}

TEST_CASE("Yoshi trap guards agree with lowered zero-product constant IMAD",
          "[video_core][ampere][yoshi][imad][trap_guards]") {
    std::array<Instruction, 5> code{{
        {{0xff037624, 0x01000400, 0x078e00ff, 0}},
        {{0x0300720c, 255, 0x03f06270, 0}},
        {{0x0947, 16, 0x03800000, 0}},
        {{0x795c, 4, 0x00300000, 0}},
        Exit,
    }};
    for (const bool negate : {false, true}) {
        code[0].words[1] = 0x01000400U | (negate ? 1U << 31 : 0U);
        const auto lowered = LowerToMaxwell(std::array{code[0], Exit});
        INFO(lowered.error);
        REQUIRE(lowered);
        for (const u32 constant : Constants) {
            CAPTURE(negate, constant);
            IntegerMachine machine;
            machine.constant = constant;
            machine.Run(lowered.instructions);
            const u32 expected = negate ? 0U - constant : constant;
            CHECK(machine.registers[3] == expected);
            const auto read = [constant](u32 bank, u32 offset) -> std::optional<u32> {
                REQUIRE(bank == 4);
                REQUIRE(offset == 0x10);
                return constant;
            };
            CHECK(Shader::Ampere::VerifyTrapGuards(std::span{code}.first(4), read) ==
                  (machine.registers[3] < 0x80000000U));
        }
        CHECK_FALSE(Shader::Ampere::VerifyTrapGuards(std::span{code}.first(4), {}));
        CHECK_FALSE(Shader::Ampere::VerifyTrapGuards(
            std::span{code}.first(4), [](u32, u32) { return std::optional<u32>{}; }));
    }
    code[0].words[1] |= 1U << 30;
    CHECK_FALSE(Shader::Ampere::VerifyTrapGuards(
        std::span{code}.first(4), [](u32, u32) { return std::optional<u32>{0}; }));
    code[0] = Instruction{{0xff037a10, 0x81000400, 0x07ffe0ff, 0}};
    CHECK_FALSE(Shader::Ampere::VerifyTrapGuards(
        std::span{code}.first(4), [](u32, u32) { return std::optional<u32>{0}; }));
}
