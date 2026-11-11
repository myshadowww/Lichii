// SPDX-License-Identifier: GPL-3.0-or-later

#include <type_traits>
#include <bit>
#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/instruction.h"
#include "video_core/fire_emblem_upscale.h"

namespace {
using namespace Tegra::Texture;
namespace FE = VideoCommon::FireEmblemUpscale;

TICEntry Image(u32 address, u32 width, u32 height) {
    TICEntry image{};
    image.address_low = address;
    image.width_minus_one.Assign(width - 1);
    image.height_minus_1.Assign(height - 1);
    image.header_version.Assign(TICHeaderVersion::BlockLinear);
    image.texture_type.Assign(TextureType::Texture2D);
    image.format.Assign(TextureFormat::B10G11R11);
    return image;
}
} // namespace

TEST_CASE("FE upscale substitute only selects the captured output pass", "[fe_upscale]") {
    REQUIRE(FE::UseReplacement(FE::ProgramId, FE::OutputProgramKey, nullptr));
    REQUIRE(FE::UseReplacement(FE::ProgramId, FE::OutputProgramKey, "fsr"));
    for (const char* option : {"native", "standin", "0", "", "10", "invalid"}) {
        REQUIRE_FALSE(FE::UseReplacement(FE::ProgramId, FE::OutputProgramKey, option));
    }
    for (u64 title : {0ULL, 0x040074A01BF12000ULL, 0x0400C3F00006E000ULL}) {
        REQUIRE_FALSE(FE::UseReplacement(title, FE::OutputProgramKey, nullptr));
    }
    for (unsigned bit = 0; bit < 64; ++bit) {
        REQUIRE_FALSE(FE::UseReplacement(FE::ProgramId, FE::OutputProgramKey ^ (1ULL << bit),
                                          nullptr));
        REQUIRE_FALSE(FE::UseReplacement(FE::ProgramId ^ (1ULL << bit), FE::OutputProgramKey,
                                          nullptr));
    }
}

TEST_CASE("FE upscale substitute validates both surface descriptors", "[fe_upscale]") {
    const auto input = Image(0x100000, 1280, 720);
    const auto output = Image(0x800000, 2560, 1440);
    REQUIRE(FE::ValidPair(input, output));
    REQUIRE_FALSE(FE::ValidPair(output, input));
    REQUIRE_FALSE(FE::ValidPair(input, input));
    REQUIRE_FALSE(FE::ValidPair(input, Image(0, 2560, 1440)));
    REQUIRE_FALSE(FE::ValidPair(input, Image(0x800000, 1280, 1440)));
    REQUIRE_FALSE(FE::ValidPair(input, Image(0x800000, 2560, 720)));
    REQUIRE_FALSE(FE::ValidPair(input, Image(0x100000, 2560, 1440)));
    for (bool alter_input : {false, true}) {
        const auto rejected = [&](auto mutate) {
            auto from = input;
            auto to = output;
            mutate(alter_input ? from : to);
            REQUIRE_FALSE(FE::ValidPair(from, to));
        };
        rejected([](auto& image) { image.texture_type.Assign(TextureType::Texture2DArray); });
        rejected([](auto& image) { image.header_version.Assign(TICHeaderVersion::Pitch); });
        rejected([](auto& image) { image.format.Assign(TextureFormat::A8B8G8R8); });
        rejected([](auto& image) { image.depth_minus_1.Assign(1); });
        rejected([](auto& image) { image.max_mip_level.Assign(1); });
        rejected([](auto& image) { image.layer_base_0_2.Assign(1); });
        rejected([](auto& image) { image.address_low = 0; });
    }
}

TEST_CASE("FE temporal selection follows the live setting and explicit comparisons", "[fe_upscale]") {
    REQUIRE_FALSE(FE::UseTemporalReplacement(true, false, nullptr));
    REQUIRE(FE::UseTemporalReplacement(true, true, nullptr));
    REQUIRE_FALSE(FE::UseTemporalReplacement(true, false, nullptr));
    for (bool enabled : {false, true}) {
        for (const char* option : {"fsr", "dlss"}) {
            REQUIRE(FE::UseTemporalReplacement(true, enabled, option));
            REQUIRE_FALSE(FE::UseTemporalReplacement(false, enabled, option));
        }
        for (const char* option : {"native", "standin", "0", "", "invalid"}) {
            REQUIRE_FALSE(FE::UseTemporalReplacement(true, enabled, option));
        }
        // Star Fox records spatial repair first and selects its own temporal pass separately.
        REQUIRE_FALSE(FE::UseTemporalReplacement(false, enabled, nullptr));
    }
}

TEST_CASE("FE substitute completion is not a cached shader rejection", "[fe_upscale]") {
    static_assert(!std::is_base_of_v<Shader::Exception, FE::DispatchReplaced>);
    bool completed = false;
    try {
        try {
            throw FE::DispatchReplaced{};
        } catch (const Shader::Exception&) {
            FAIL("Substitute completion must not enter shader rejection handling");
        }
    } catch (const FE::DispatchReplaced&) {
        completed = true;
    }
    REQUIRE(completed);
}

TEST_CASE("FE camera jitter belongs to one output attempt", "[fe_upscale]") {
    FE::FrameJitter state;
    REQUIRE_FALSE(state.Take());
    state.Capture(0x1000, 1280, 720, {0.25f, -0.375f});
    const auto frame = state.Take();
    REQUIRE(frame);
    REQUIRE(frame->color == 0x1000);
    REQUIRE(frame->width == 1280);
    REQUIRE(frame->height == 720);
    REQUIRE(frame->offset == std::array<f32, 2>{0.25f, -0.375f});
    REQUIRE_FALSE(state.Take());
    state.Capture(0x1000, 1280, 720, {0.25f, -0.375f});
    state.Capture(0x2000, 1920, 1080, {-0.5f, 0.5f});
    const auto next = state.Take();
    REQUIRE(next);
    REQUIRE(next->color == 0x2000);
    REQUIRE(next->offset == std::array<f32, 2>{-0.5f, 0.5f});
}

TEST_CASE("FE invalid camera jitter clears the earlier frame", "[fe_upscale]") {
    FE::FrameJitter state;
    for (f32 invalid : {std::numeric_limits<f32>::quiet_NaN(),
                        std::numeric_limits<f32>::infinity(), -0.51f, 0.51f}) {
        for (size_t component = 0; component < 2; ++component) {
            state.Capture(0x1000, 1280, 720, {0.25f, -0.375f});
            std::array<f32, 2> jitter{};
            jitter[component] = invalid;
            state.Capture(0x1000, 1280, 720, jitter);
            REQUIRE_FALSE(state.Take());
        }
    }
    state.Capture(0, 1280, 720, {});
    REQUIRE_FALSE(state.Take());
    state.Capture(0x1000, 0, 720, {});
    REQUIRE_FALSE(state.Take());
    state.Capture(0x1000, 1280, 0, {});
    REQUIRE_FALSE(state.Take());
}

TEST_CASE("Star Fox spatial replacement selects only its captured output", "[starfox_upscale]") {
    constexpr u64 program_id = 0x0400E88025B1A000ULL;
    constexpr u64 prepass = 0xd686ca0b9fad9e6dULL;
    constexpr u64 output = 0x7d3764ff101bec27ULL;
    for (const char* option : {static_cast<const char*>(nullptr), "fsr", "dlss"}) {
        REQUIRE(FE::UseReplacement(program_id, output, option));
        REQUIRE_FALSE(FE::UseReplacement(program_id, prepass, option));
        REQUIRE_FALSE(FE::UseReplacement(program_id, FE::OutputProgramKey, option));
        REQUIRE_FALSE(FE::UseReplacement(FE::ProgramId, output, option));
    }
    for (const char* option : {"native", "0", "", "invalid"}) {
        REQUIRE_FALSE(FE::UseReplacement(program_id, output, option));
    }
    for (unsigned bit = 0; bit < 64; ++bit) {
        REQUIRE_FALSE(FE::UseReplacement(program_id, output ^ (1ULL << bit), nullptr));
        REQUIRE_FALSE(FE::UseReplacement(program_id ^ (1ULL << bit), output, nullptr));
    }
    for (const u64 shader : {0x4027797e061f5eb4ULL, 0x0cbf774bc3b02cbdULL,
                            0x4f21fa94fc963d1cULL, 0xd4b6c9a1c0035421ULL}) {
        REQUIRE_FALSE(FE::UseReplacement(program_id, shader, nullptr));
    }
}

TEST_CASE("Star Fox captured passes use the validated colour and jitter slots", "[starfox_upscale]") {
    using Shader::Ampere::Instruction;
    constexpr Instruction prepass_color{{0x020c7b66, 0x30009e1d, 0x009e0f06, 0x00014200}};
    constexpr Instruction output_color{{0x050e7b66, 0x3000ca0e, 0x009e0f10, 0x000f4200}};
    constexpr Instruction output_store{{0x52003b9b, 0x6000d234, 0x0010af00, 0x0007e200}};
    constexpr Instruction secondary_store{{0x0e003b9b, 0x6000d434, 0x0010af00, 0x0001e200}};
    constexpr Instruction jitter_x{{0x07077621, 0x00006c00, 0x00010000, 0x000fe400}};
    constexpr Instruction jitter_y{{0x05057621, 0x00006d00, 0x00010000, 0x000fc600}};
    for (const auto instruction : {prepass_color, output_color, output_store, secondary_store,
                                  jitter_x, jitter_y}) {
        REQUIRE(instruction.Bits(54, 5) == 0);
    }
    REQUIRE(prepass_color.Bits(40, 14) * 4 == FE::PrepassColorOffset);
    REQUIRE(output_color.Bits(40, 14) * 4 == FE::OutputColorInputOffset);
    REQUIRE(output_store.Bits(40, 14) * 4 == FE::OutputColorOffset);
    REQUIRE(secondary_store.Bits(40, 14) * 4 == 0x350);
    REQUIRE(jitter_x.Bits(38, 16) == FE::JitterOffset);
    REQUIRE(jitter_y.Bits(38, 16) == FE::JitterOffset + sizeof(f32));

    const auto starfox = FE::CapturedPasses(FE::StarFoxProgramId);
    REQUIRE(starfox);
    REQUIRE(starfox->prepass == 0xd686ca0b9fad9e6dULL);
    REQUIRE(starfox->output == 0x7d3764ff101bec27ULL);
    REQUIRE(starfox->star_fox);
    const auto fire_emblem = FE::CapturedPasses(FE::ProgramId);
    REQUIRE(fire_emblem);
    REQUIRE_FALSE(fire_emblem->star_fox);
    REQUIRE_FALSE(FE::CapturedPasses(0x040074A01BF12000ULL));
}

TEST_CASE("Star Fox cutscene prepass opens the existing spatial replacement chain",
          "[starfox_upscale]") {
    const auto passes = FE::CapturedPasses(FE::StarFoxProgramId);
    REQUIRE(passes);
    for (const u64 prepass : {0xd686ca0b9fad9e6dULL, 0xebab31dd6f0fc08dULL}) {
        REQUIRE(passes->IsPrepass(prepass));
        REQUIRE_FALSE(FE::CapturedPasses(FE::ProgramId)->IsPrepass(prepass));
        REQUIRE_FALSE(FE::UseReplacement(FE::StarFoxProgramId, prepass, nullptr));
        for (unsigned bit = 0; bit < 64; ++bit) {
            REQUIRE_FALSE(passes->IsPrepass(prepass ^ (1ULL << bit)));
        }
    }
    REQUIRE_FALSE(passes->IsPrepass(0));
    REQUIRE_FALSE(passes->IsPrepass(FE::StarFoxOutputProgramKey));
    REQUIRE_FALSE(passes->IsPrepass(FE::PrepassProgramKey));
    REQUIRE(FE::CapturedPasses(FE::ProgramId)->IsPrepass(FE::PrepassProgramKey));
    REQUIRE_FALSE(FE::CapturedPasses(FE::ProgramId)->IsPrepass(0));
}

TEST_CASE("FE captured alternate prepass opens only its own replacement chain", "[fe_upscale]") {
    const auto passes = FE::CapturedPasses(FE::ProgramId);
    REQUIRE(passes);
    for (const u64 prepass : {0x4cbf6eff32525aecULL, 0x5e8d8dc58bed8ba4ULL}) {
        REQUIRE(passes->IsPrepass(prepass));
        REQUIRE_FALSE(FE::CapturedPasses(FE::StarFoxProgramId)->IsPrepass(prepass));
        REQUIRE_FALSE(FE::UseReplacement(FE::ProgramId, prepass, nullptr));
        for (unsigned bit = 0; bit < 64; ++bit) {
            REQUIRE_FALSE(passes->IsPrepass(prepass ^ (1ULL << bit)));
        }
    }
    REQUIRE_FALSE(passes->IsPrepass(0));
    REQUIRE_FALSE(passes->IsPrepass(FE::OutputProgramKey));
    REQUIRE_FALSE(FE::CapturedPasses(FE::ProgramId ^ 1));
}

TEST_CASE("Star Fox scene output requires its captured title and native program",
          "[starfox_upscale]") {
    REQUIRE(FE::IsStarFoxFinalUpscale(FE::StarFoxProgramId, FE::StarFoxFinalUpscaleProgramKey));
    for (unsigned bit = 0; bit < 64; ++bit) {
        REQUIRE_FALSE(FE::IsStarFoxFinalUpscale(FE::StarFoxProgramId ^ (1ULL << bit),
                                                FE::StarFoxFinalUpscaleProgramKey));
        REQUIRE_FALSE(FE::IsStarFoxFinalUpscale(FE::StarFoxProgramId,
                                                FE::StarFoxFinalUpscaleProgramKey ^ (1ULL << bit)));
    }
    REQUIRE_FALSE(FE::IsStarFoxFinalUpscale(FE::ProgramId, FE::StarFoxFinalUpscaleProgramKey));
    REQUIRE_FALSE(FE::IsStarFoxFinalUpscale(FE::StarFoxProgramId, 0xa2884a1f4cd841e7ULL));
}

TEST_CASE("Star Fox logged native-resolution cutscene colour pair is accepted", "[starfox_upscale]") {
    const auto input = std::bit_cast<TICEntry>(std::array<u32, 8>{
        0x58d24908, 0x26f62000, 0x00600081, 0x00070020,
        0xe8c0077f, 0x80000437, 0x03000000, 0x00000000});
    const auto output = std::bit_cast<TICEntry>(std::array<u32, 8>{
        0x58d7ff83, 0x352f1000, 0x00600081, 0x00070020,
        0xe880077f, 0x80000437, 0x03000000, 0x00000000});
    REQUIRE(input.Width() == 1920);
    REQUIRE(input.Height() == 1080);
    REQUIRE(output.Width() == input.Width());
    REQUIRE(output.Height() == input.Height());
    REQUIRE_FALSE(FE::ValidInput(input));
    REQUIRE_FALSE(FE::ValidPair(input, output));
    REQUIRE(FE::ValidInput(input, true));
    REQUIRE(FE::ValidPair(input, output, true));
    FE::FrameJitter state;
    state.Capture(input.Address(), input.Width(), input.Height(),
                  {std::bit_cast<f32>(0x3ec00000U), std::bit_cast<f32>(0x3d638e40U)});
    const auto sample = state.Take();
    REQUIRE(sample);
    REQUIRE(sample->color == input.Address());
    REQUIRE(sample->offset[0] == 0.375f);
    REQUIRE_FALSE(state.Take());

    for (const bool alter_input : {false, true}) {
        const auto rejected = [&](auto mutate) {
            auto from = input;
            auto to = output;
            mutate(alter_input ? from : to);
            REQUIRE_FALSE(FE::ValidPair(from, to, true));
        };
        rejected([](auto& image) { image.srgb_conversion.Assign(!image.srgb_conversion); });
        rejected([](auto& image) { image.r_type.Assign(ComponentType::UINT); });
        rejected([](auto& image) { image.a_type.Assign(ComponentType::UNORM_FORCE_FP16); });
        rejected([](auto& image) { image.x_source.Assign(SwizzleSource::B); });
        rejected([](auto& image) { image.w_source.Assign(SwizzleSource::OneFloat); });
        rejected([](auto& image) { image.format.Assign(TextureFormat::B10G11R11); });
        rejected([](auto& image) { image.texture_type.Assign(TextureType::Texture2DArray); });
        rejected([](auto& image) { image.header_version.Assign(TICHeaderVersion::Pitch); });
        rejected([](auto& image) { image.depth_minus_1.Assign(1); });
        rejected([](auto& image) { image.max_mip_level.Assign(1); });
        rejected([](auto& image) { image.res_min_mip_level.Assign(1); });
        rejected([](auto& image) { image.res_max_mip_level.Assign(1); });
        rejected([](auto& image) { image.layer_base_3_7.Assign(4); });
        rejected([](auto& image) { image.width_minus_one.Assign(1279); });
        rejected([](auto& image) { image.height_minus_1.Assign(719); });
        rejected([](auto& image) { image.msaa_mode.Assign(MsaaMode::Msaa2x2); });
        rejected([](auto& image) { image.block_depth.Assign(1); });
        rejected([](auto& image) { image.is_sparse.Assign(1); });
        rejected([](auto& image) { image.address_low = 0; image.address_high.Assign(0); });
    }
    auto alias = output;
    alias.address_low = input.address_low;
    alias.address_high.Assign(input.address_high);
    REQUIRE_FALSE(FE::ValidPair(input, alias, true));
}

TEST_CASE("Star Fox temporal inputs retain captured motion and depth interpretation",
          "[starfox_upscale]") {
    const auto color =
        std::bit_cast<TICEntry>(std::array<u32, 8>{0x58d24908, 0x26f62000, 0x00600081, 0x00070020,
                                                   0xe8c0077f, 0x80000437, 0x03000000, 0x00000000});
    const auto motion =
        std::bit_cast<TICEntry>(std::array<u32, 8>{0x70d7ff8c, 0x31fd2000, 0x00600081, 0x00070020,
                                                   0xe880077f, 0x80000437, 0x03000000, 0x00000000});
    const auto depth =
        std::bit_cast<TICEntry>(std::array<u32, 8>{0x701253b0, 0x230f2000, 0x00600081, 0x00070020,
                                                   0xe880077f, 0x80000437, 0x03000000, 0x00000000});
    REQUIRE(FE::ValidStarFoxMotionDepth(color, motion, depth));
    for (const bool alter_depth : {false, true}) {
        const auto rejected = [&](auto mutate) {
            auto mv = motion;
            auto z = depth;
            mutate(alter_depth ? z : mv);
            REQUIRE_FALSE(FE::ValidStarFoxMotionDepth(color, mv, z));
        };
        rejected([](auto& image) { image.width_minus_one.Assign(1279); });
        rejected([](auto& image) { image.height_minus_1.Assign(719); });
        rejected([](auto& image) { image.texture_type.Assign(TextureType::Texture2DArray); });
        rejected([](auto& image) { image.max_mip_level.Assign(1); });
        rejected([](auto& image) { image.res_min_mip_level.Assign(1); });
        rejected([](auto& image) { image.msaa_mode.Assign(MsaaMode::Msaa2x2); });
        rejected([](auto& image) { image.is_sparse.Assign(1); });
        rejected([&](auto& image) { image.address_low = color.address_low; });
        rejected([](auto& image) { image.srgb_conversion.Assign(1); });
    }
    auto signed_motion = motion;
    signed_motion.r_type.Assign(ComponentType::SNORM);
    REQUIRE_FALSE(FE::ValidStarFoxMotionDepth(color, signed_motion, depth));
    auto swapped_motion = motion;
    swapped_motion.x_source.Assign(SwizzleSource::G);
    REQUIRE_FALSE(FE::ValidStarFoxMotionDepth(color, swapped_motion, depth));
    auto wrong_depth = depth;
    wrong_depth.format.Assign(TextureFormat::Z32);
    REQUIRE_FALSE(FE::ValidStarFoxMotionDepth(color, motion, wrong_depth));
    REQUIRE_FALSE(FE::ValidStarFoxMotionDepth(color, depth, motion));
}

TEST_CASE("Star Fox history reset belongs to the captured frame", "[starfox_upscale]") {
    FE::FrameJitter state;
    state.Capture(0x1000, 1920, 1080, {-0.4375f, 0.38888896f}, true);
    const auto first = state.Take();
    REQUIRE(first);
    REQUIRE(first->reset);
    REQUIRE_FALSE(state.Take());
    state.Capture(0x1000, 1920, 1080, {0.125f, -0.277778f});
    const auto next = state.Take();
    REQUIRE(next);
    REQUIRE_FALSE(next->reset);
}

TEST_CASE("Star Fox gameplay jitter requires unjittered current and previous motion projections",
          "[starfox_upscale]") {
    // Captured training cb7: current view/projection/VP, previous VP and previous view.
    std::array<u32, 0x200 / 4> fragment_camera{
        0x3bae1f57U, 0x3def9f67U, 0xbf7e3cf0U, 0x00000000U, 0xbf7fff07U, 0xba278e9fU, 0xbbb1cb4bU,
        0x00000000U, 0xbaa66971U, 0x3f7e3ddcU, 0x3def9cb3U, 0x00000000U, 0xc315bd66U, 0xc545c939U,
        0x46b4ac49U, 0x3f800000U, 0x3fadd2c7U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
        0x401a8278U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0xbf800000U,
        0x00000000U, 0x00000000U, 0x40a00000U, 0x00000000U, 0x3bec7516U, 0x3e90a003U, 0x00000000U,
        0x3f7e3cf0U, 0xbfadd21eU, 0xbaca4261U, 0x00000000U, 0x3bb1cb4bU, 0xbae1fc8aU, 0x401972c9U,
        0x00000000U, 0xbdef9cb3U, 0xc34b588fU, 0xc5eebfb2U, 0x40a00000U, 0xc6b4ac49U, 0x00000000U,
        0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
        0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
        0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
        0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
        0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
        0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
        0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x3bed7200U, 0x3e90a4dbU,
        0x00000000U, 0x3f7e3cd0U, 0xbfadd21cU, 0xbacb1fc5U, 0x00000000U, 0x3bb289b1U, 0xbae2f4f9U,
        0x401972b6U, 0x00000000U, 0xbdefa4b7U, 0xc34be8c6U, 0xc5ee9abfU, 0x40a00000U, 0xc6b485a1U,
        0x3baed995U, 0x3defa76dU, 0xbf7e3cd0U, 0x00000000U, 0xbf7fff04U, 0xba284607U, 0xbbb289b1U,
        0x00000000U, 0xbaa72062U, 0x3f7e3dbcU, 0x3defa4b7U, 0x00000000U, 0xc3162798U, 0xc545aa9dU,
        0x46b485a1U, 0x3f800000U,
    };
    std::array<u32, 0xc0 / 4> projection{};
    std::copy_n(fragment_camera.begin(), projection.size(), projection.begin());
    std::array<u32, 0x11c / 4> object{};
    std::array<u32, 0x12c / 4> fragment_object{};
    std::array<u32, 0x394 / 4> camera{};
    object[0x118 / 4] = fragment_object[0x118 / 4] = fragment_object[0x128 / 4] =
        std::bit_cast<u32>(1.0f);
    camera[0x370 / 4] = std::bit_cast<u32>(0.0002604166802484542f);
    camera[0x374 / 4] = std::bit_cast<u32>(-0.00046296295477077365f);
    camera[0x378 / 4] = std::bit_cast<u32>(-0.0002604166802484542f);
    camera[0x37c / 4] = std::bit_cast<u32>(0.00046296295477077365f);
    const auto accepted = [&] {
        return FE::StarFoxGameplayJitter(object, camera, fragment_object, projection,
                                         fragment_camera);
    };
    const auto jitter = accepted();
    REQUIRE(jitter);
    REQUIRE((*jitter)[0] == 0.25f);
    REQUIRE((*jitter)[1] == 0.25f);

    const auto reject_word = [&](auto& buffer, u32 offset, u32 value) {
        const u32 old = std::exchange(buffer[offset / 4], value);
        REQUIRE_FALSE(accepted());
        buffer[offset / 4] = old;
    };
    for (const u32 offset : {0x7c, 0xac}) {
        reject_word(object, offset, 1);
    }
    for (const u32 offset : {0x334, 0x390}) {
        reject_word(camera, offset, 1);
    }
    for (const u32 offset : {0x110, 0x114, 0x118}) {
        reject_word(object, offset, std::bit_cast<u32>(0.5f));
        reject_word(fragment_object, offset, std::bit_cast<u32>(0.5f));
    }
    for (const u32 offset : {0x120, 0x124, 0x128}) {
        reject_word(fragment_object, offset, std::bit_cast<u32>(0.5f));
    }
    for (const u32 offset : {0x40, 0x54, 0x78}) {
        reject_word(projection, offset, std::bit_cast<u32>(std::numeric_limits<f32>::infinity()));
        reject_word(projection, offset, 0);
    }
    reject_word(projection, 0x60, camera[0x370 / 4]);
    reject_word(fragment_camera, 0x180, std::bit_cast<u32>(0.01f));
    reject_word(fragment_camera, 0x80, std::bit_cast<u32>(0.01f));
    reject_word(fragment_camera, 0x1c0, std::bit_cast<u32>(std::numeric_limits<f32>::infinity()));
    for (const u32 offset : {0x370, 0x374, 0x378, 0x37c}) {
        reject_word(camera, offset, std::bit_cast<u32>(std::numeric_limits<f32>::quiet_NaN()));
        reject_word(camera, offset, std::bit_cast<u32>(std::numeric_limits<f32>::infinity()));
        reject_word(camera, offset, std::bit_cast<u32>(0.01f));
    }
    std::array<std::span<const u32>, 5> buffers{object, camera, fragment_object, projection,
                                                fragment_camera};
    for (size_t i = 0; i < buffers.size(); ++i) {
        auto truncated = buffers;
        truncated[i] = truncated[i].first(truncated[i].size() - 1);
        REQUIRE_FALSE(FE::StarFoxGameplayJitter(truncated[0], truncated[1], truncated[2],
                                                truncated[3], truncated[4]));
    }
    REQUIRE(accepted());
}
