// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/profile.h"

namespace {
using namespace Shader;

class NativeEnvironment final : public Environment {
public:
    NativeEnvironment() {
        stage = Stage::Compute;
    }
    bool IsNativeProgram() const override { return true; }
    bool IsCbufStorageBufferPointer(u32, u32 offset) override { return offset < 0x200; }
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    TextureType ReadTextureType(u32) override { return {}; }
    TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

} // namespace
TEST_CASE("Dynamic pointer-array writes mark independently bound later slots",
          "[shader][storage_buffer][dynamic_storage_writes]") {
    const auto operation = GENERATE(IR::Opcode::WriteGlobal32, IR::Opcode::GlobalAtomicIAdd32,
                                    IR::Opcode::LoadGlobal32);
    const bool later_load_first = GENERATE(false, true);
    CAPTURE(operation, later_load_first);
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    const auto address = [&](const IR::U32& low, const IR::U32& high) {
        return IR::U64{ir.PackUint2x32(ir.CompositeConstruct(low, high))};
    };
    const auto word = [&](u32 index, u32 offset) {
        return ir.GetCbuf(ir.Imm32(index), ir.Imm32(offset));
    };
    const auto bind_later = [&] {
        static_cast<void>(ir.LoadGlobal32(address(word(0, 0x180), word(0, 0x184))));
        static_cast<void>(ir.LoadGlobal32(address(word(1, 0), word(1, 4))));
    };
    if (later_load_first) {
        bind_later();
    }
    // Only the first 16 entries are enumerated; the independent load binds entry 16.
    const IR::U32 slot{ir.IAdd(ir.ShiftLeftLogical(word(0, 0x300), ir.Imm32(3)), ir.Imm32(0x100))};
    const IR::U32 high{ir.GetCbuf(ir.Imm32(0), ir.IAdd(slot, ir.Imm32(4)))};
    const auto pointer = address(ir.GetCbuf(ir.Imm32(0), slot), high);
    switch (operation) {
    case IR::Opcode::WriteGlobal32:
        ir.WriteGlobal32(pointer, ir.Imm32(1));
        break;
    case IR::Opcode::GlobalAtomicIAdd32:
        static_cast<void>(ir.GlobalAtomicIAdd(pointer, ir.Imm32(1)));
        break;
    default:
        static_cast<void>(ir.LoadGlobal32(pointer));
        break;
    }
    const IR::Inst* const dynamic = &block.Instructions().back();
    if (!later_load_first) {
        bind_later();
    }
    IR::Program program;
    program.stage = Stage::Compute;
    program.workgroup_size = {1, 1, 1};
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    HostTranslateInfo host{};
    host.min_ssbo_alignment = 16;
    host.support_int64 = true;
    NativeEnvironment env;
    Optimization::GlobalMemoryToStorageBufferPass(env, program, host);

    REQUIRE(dynamic->GetOpcode() == operation);
    const auto& descriptors = program.info.storage_buffers_descriptors;
    REQUIRE(descriptors.size() == 18);
    const auto descriptor = [&](u32 index, u32 offset) {
        return std::ranges::find_if(descriptors, [&](const auto& d) {
            return d.cbuf_index == index && d.cbuf_offset == offset;
        });
    };
    const auto last = descriptor(0, 0x180);
    REQUIRE(last != descriptors.end());
    CHECK(last->is_written == (operation != IR::Opcode::LoadGlobal32));
    CHECK_FALSE(last->write_extent.bounded);
    const auto unrelated = descriptor(1, 0);
    REQUIRE(unrelated != descriptors.end());
    CHECK_FALSE(unrelated->is_written);
}
