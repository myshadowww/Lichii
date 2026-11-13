// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "core/core.h"
#include "shader_recompiler/exception.h"
#include "tests/core/system_test_context.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/shader_environment.h"
#include "video_core/shader_warm/shader_archives.h"
#include "video_core/shader_warm/shader_warm_builder.h"
#include "video_core/shader_warm/warm_records.h"

namespace {

namespace ShaderWarm = VideoCommon::ShaderWarm;
using Settings::PrecacheShaders;

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("eden-shader-precache-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void WriteBytes(const std::filesystem::path& file, std::string_view bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // Anonymous namespace

TEST_CASE("ShaderPrecache: the setting decides whether the boot precaches", "[video_core]") {
    STATIC_REQUIRE_FALSE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Off, false));
    STATIC_REQUIRE_FALSE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Off, true));
    STATIC_REQUIRE_FALSE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Ask, false));
    STATIC_REQUIRE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Ask, true));
    STATIC_REQUIRE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Always, false));
    STATIC_REQUIRE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Always, true));
}

TEST_CASE("ShaderPrecache: launch requests are taken once", "[video_core]") {
    ShaderWarm::RequestWarmBeforeStart(true);
    REQUIRE(ShaderWarm::TakeWarmBeforeStart());
    REQUIRE_FALSE(ShaderWarm::TakeWarmBeforeStart());

    ShaderWarm::RequestSkipWarm();
    REQUIRE(ShaderWarm::TakeSkipWarm());
    REQUIRE_FALSE(ShaderWarm::TakeSkipWarm());

    // A skip pressed during an earlier launch does not skip the next one.
    ShaderWarm::RequestSkipWarm();
    ShaderWarm::RequestWarmBeforeStart(true);
    REQUIRE_FALSE(ShaderWarm::TakeSkipWarm());
    REQUIRE(ShaderWarm::TakeWarmBeforeStart());
}

TEST_CASE("ShaderPrecache: the marker matches only the same file and driver", "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    REQUIRE_FALSE(ShaderWarm::CurrentWarmStamp(warm, 0, 1, 2, 3).has_value());
    REQUIRE_FALSE(ShaderWarm::ReadWarmMarker(warm).has_value());

    WriteBytes(warm, "precache");
    const auto stamp = ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53);
    REQUIRE(stamp.has_value());
    REQUIRE(stamp->size == 8);
    REQUIRE(ShaderWarm::WriteWarmMarker(warm, *stamp));
    REQUIRE(ShaderWarm::WarmMarkerPath(warm) == dir.path / "warm-compute.done");

    const auto marker = ShaderWarm::ReadWarmMarker(warm);
    REQUIRE(marker.has_value());
    REQUIRE(marker->entries == 12);
    // The entry count is informative: a stamp taken before reading the file still matches.
    REQUIRE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 0, 0x1234, 0x2206, 53)));
    REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1235, 0x2206, 53)));
    REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2207, 53)));
    REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 54)));
    // Another shader lowering (an update that changed the recompiler) builds the programs again.
    REQUIRE_FALSE(
        marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53, 0x77)));

    // Built again with the same contents: still done.
    WriteBytes(warm, "precache");
    REQUIRE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53)));

    WriteBytes(warm, "a rebuilt precache");
    REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53)));
}

TEST_CASE("ShaderPrecache: content stamps ignore timestamps and cover every file chunk",
          "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    // Cross the 8 MiB read boundary so a change in the last chunk must invalidate the marker.
    std::string bytes((8u << 20) + 17, 'a');
    WriteBytes(warm, bytes);
    const auto original_time = std::filesystem::last_write_time(warm);
    const auto original = ShaderWarm::CurrentWarmStamp(warm, 12, 1, 2, 53, 7);
    REQUIRE(original.has_value());

    std::filesystem::last_write_time(warm, original_time + std::chrono::seconds{10});
    const auto touched = ShaderWarm::CurrentWarmStamp(warm, 12, 1, 2, 53, 7);
    REQUIRE(touched.has_value());
    REQUIRE(original->Matches(*touched));

    bytes.back() = 'b';
    WriteBytes(warm, bytes);
    std::filesystem::last_write_time(warm, original_time);
    const auto changed = ShaderWarm::CurrentWarmStamp(warm, 12, 1, 2, 53, 7);
    REQUIRE(changed.has_value());
    REQUIRE(changed->size == original->size);
    REQUIRE_FALSE(original->Matches(*changed));

    std::filesystem::resize_file(warm, bytes.size() - 1);
    const auto truncated = ShaderWarm::CurrentWarmStamp(warm, 12, 1, 2, 53, 7);
    REQUIRE(truncated.has_value());
    REQUIRE_FALSE(changed->Matches(*truncated));
    REQUIRE_FALSE(ShaderWarm::CurrentWarmStamp(dir.path, 12, 1, 2, 53, 7).has_value());
}

TEST_CASE("ShaderPrecache: old and incomplete markers must be rebuilt", "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    const auto marker = ShaderWarm::WarmMarkerPath(warm);
    for (const auto text : {"8 12345 12 1 2 53\n", "v2 8 12345 12 1 2 53\n",
                            "v2 8 12345 12 1 2 53 broken\n", "v3 8 12345 12 1 2 53 7\n"}) {
        INFO(text);
        WriteBytes(marker, text);
        REQUIRE_FALSE(ShaderWarm::ReadWarmMarker(warm).has_value());
    }
    WriteBytes(marker, "v2 8 12345 12 1 2 53 7\n");
    REQUIRE(ShaderWarm::ReadWarmMarker(warm).has_value());
}

TEST_CASE("ShaderPrecache: a rebuild with the same contents keeps the marker", "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    const auto rebuilt = dir.path / "warm-compute.tmp";
    WriteBytes(warm, "precache");
    REQUIRE(ShaderWarm::WriteWarmMarker(
        warm, *ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53)));

    WriteBytes(rebuilt, "precache");
    ShaderWarm::DropStaleWarmMarker(warm, rebuilt);
    REQUIRE(ShaderWarm::ReadWarmMarker(warm).has_value());

    WriteBytes(rebuilt, "a rebuilt precache");
    ShaderWarm::DropStaleWarmMarker(warm, rebuilt);
    REQUIRE_FALSE(std::filesystem::exists(ShaderWarm::WarmMarkerPath(warm)));

    // No marker left and nothing to replace the file with: nothing to do.
    ShaderWarm::DropStaleWarmMarker(warm, dir.path / "missing.tmp");
    REQUIRE_FALSE(ShaderWarm::ReadWarmMarker(warm).has_value());
}

TEST_CASE("ShaderPrecache: precached pipelines are kept per driver", "[video_core]") {
    TempDir dir;
    const auto file = dir.path / "precache-graphics.bin";
    const ShaderWarm::DriverStamp stamp{0x1234, 0x2206, 53};
    REQUIRE(ShaderWarm::ReadPrecachedPipelines(file, stamp).empty());

    const std::unordered_set<u64> hashes{1, 0xfedcba9876543210ULL, 42};
    REQUIRE(ShaderWarm::WritePrecachedPipelines(file, stamp, hashes));
    REQUIRE(ShaderWarm::ReadPrecachedPipelines(file, stamp) == hashes);
    REQUIRE(ShaderWarm::ReadPrecachedPipelines(file, {0x1235, 0x2206, 53}).empty());
    REQUIRE(ShaderWarm::ReadPrecachedPipelines(file, {0x1234, 0x2207, 53}).empty());
    REQUIRE(ShaderWarm::ReadPrecachedPipelines(file, {0x1234, 0x2206, 54}).empty());

    // A truncated file keeps the hashes it holds in full.
    std::filesystem::resize_file(file, std::filesystem::file_size(file) - 4);
    REQUIRE(ShaderWarm::ReadPrecachedPipelines(file, stamp).size() == 2);
}

TEST_CASE("ShaderPrecache: the entry count belongs to a file of one size", "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    WriteBytes(warm, "0123456789");
    REQUIRE(ShaderWarm::ReadWarmCount(warm) == 0);

    WriteBytes(ShaderWarm::WarmCountPath(warm), "345 10\n");
    REQUIRE(ShaderWarm::ReadWarmCount(warm) == 345);

    WriteBytes(warm, "01234567890");
    REQUIRE(ShaderWarm::ReadWarmCount(warm) == 0);
}

TEST_CASE("ShaderPrecache: a file from an older builder or another cache version is stale",
          "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    REQUIRE(ShaderWarm::WarmFilesStale(warm).has_value());

    const auto header = [](u32 version) {
        std::string bytes(VideoCommon::PipelineCacheMagic.begin(),
                          VideoCommon::PipelineCacheMagic.end());
        bytes.append(reinterpret_cast<const char*>(&version), sizeof(version));
        return bytes;
    };
    WriteBytes(warm, header(ShaderWarm::WarmCacheVersion));
    const auto count = ShaderWarm::WarmCountPath(warm);
    // Count files written before the builder version was recorded have two fields.
    WriteBytes(count, "0 12\n");
    REQUIRE(ShaderWarm::WarmFilesStale(warm).has_value());
    WriteBytes(count, fmt::format("0 12 {}\n", ShaderWarm::WarmBuilderVersion - 1));
    REQUIRE(ShaderWarm::WarmFilesStale(warm).has_value());
    WriteBytes(count, fmt::format("0 12 {}\n", ShaderWarm::WarmBuilderVersion));
    REQUIRE_FALSE(ShaderWarm::WarmFilesStale(warm).has_value());

    WriteBytes(count, fmt::format("0 13 {}\n", ShaderWarm::WarmBuilderVersion));
    REQUIRE(ShaderWarm::WarmFilesStale(warm).has_value());
    WriteBytes(count, fmt::format("0 12 {}\n", ShaderWarm::WarmBuilderVersion));
    // The emulator loads a file of a compatible older version, so it is not built again.
    const auto& compatible = VideoCommon::CompatibleVulkanPipelineCacheVersions;
    for (const auto version : compatible) {
        CAPTURE(version);
        WriteBytes(warm, header(version));
        REQUIRE_FALSE(ShaderWarm::WarmFilesStale(warm).has_value());
    }
    WriteBytes(warm, header(0));
    REQUIRE(ShaderWarm::WarmFilesStale(warm).has_value());
    auto malformed = header(ShaderWarm::WarmCacheVersion);
    malformed.front() = '!';
    WriteBytes(warm, malformed);
    REQUIRE(ShaderWarm::WarmFilesStale(warm).has_value());
    malformed = header(ShaderWarm::WarmCacheVersion);
    malformed.pop_back();
    WriteBytes(warm, malformed);
    REQUIRE(ShaderWarm::WarmFilesStale(warm).has_value());
}

TEST_CASE("ShaderPrecache: a loose dump builds from its directory or main path",
          "[video_core]") {
    ScopedSystemTestContext context;
    Core::System system;
    TempDir dir;
    const auto exefs = dir.path / "exefs";
    const auto effects = dir.path / "romfs" / "Effect" / "Particle";
    std::filesystem::create_directories(exefs);
    std::filesystem::create_directories(effects);
    // The archive scan only needs a recognizable ExeFS and a RomFS, not executable code.
    WriteBytes(exefs / "main", "");
    WriteBytes(exefs / "main.npdm", "");
    std::string effect(64, '\0');
    effect.replace(0, 4, "VFXB");
    WriteBytes(effects / "Magic.ptcl", effect);

    ShaderWarm::BuildOptions options;
    options.threads = 1;
    std::optional<ShaderWarm::WarmStamp> first;
    for (const auto& input : {exefs, exefs / "main"}) {
        INFO(input.string());
        options.output_dir = dir.path / (first ? "from-main" : "from-directory");
        const auto result = ShaderWarm::BuildWarmFilesWith(system, input, 0, options, {}, {});
        INFO(result.error);
        REQUIRE(result.ok);
        REQUIRE(result.archives == 1);
        REQUIRE(result.compute_programs == 0);
        REQUIRE(result.skipped == 0);
        const auto warm = options.output_dir / "warm-compute.bin";
        REQUIRE_FALSE(ShaderWarm::WarmFilesStale(warm).has_value());
        const auto stamp = ShaderWarm::CurrentWarmStamp(warm, 0, 0, 0,
                                                       ShaderWarm::WarmCacheVersion);
        REQUIRE(stamp.has_value());
        if (first) {
            REQUIRE(first->Matches(*stamp));
        } else {
            first = stamp;
        }
    }
}

TEST_CASE("ShaderPrecache: a compute entry reads back like a pipeline cache entry",
          "[video_core]") {
    ShaderWarm::LoweredCode lowered;
    lowered.code.resize(16 * sizeof(u64));
    for (size_t i = 0; i < lowered.code.size(); ++i) {
        lowered.code[i] = static_cast<u8>(i * 7 + 1);
    }
    lowered.texture_buffer = 3;
    const ShaderWarm::ComputeControl control{{64, 2, 1}, 1024, 256};
    ShaderWarm::ComputeKey key_bytes{};
    const auto entry = ShaderWarm::ComputeEntry(lowered, control, key_bytes);

    TempDir dir;
    const auto file = dir.path / "entry.bin";
    WriteBytes(file, std::string_view{reinterpret_cast<const char*>(entry.data()), entry.size()});

    std::ifstream in(file, std::ios::binary);
    in.exceptions(std::ifstream::failbit);
    u32 num_envs{};
    in.read(reinterpret_cast<char*>(&num_envs), sizeof(num_envs));
    REQUIRE(num_envs == 1);
    VideoCommon::FileEnvironment env;
    env.Deserialize(in, static_cast<std::streamoff>(entry.size()));
    Vulkan::ComputePipelineCacheKey key{};
    static_assert(sizeof(key) == sizeof(key_bytes));
    in.read(reinterpret_cast<char*>(&key), sizeof(key));
    REQUIRE(in.tellg() == static_cast<std::streamoff>(entry.size()));

    REQUIRE(env.ShaderStage() == Shader::Stage::Compute);
    REQUIRE(env.WorkgroupSize() == control.workgroup);
    REQUIRE(env.SharedMemorySize() == control.shared_memory);
    REQUIRE(env.LocalMemorySize() == control.local_memory);
    REQUIRE(env.TextureBoundBuffer() == 3);
    REQUIRE(std::memcmp(&key, key_bytes.data(), sizeof(key)) == 0);
    REQUIRE(key.unique_hash == ShaderWarm::StageHash(lowered.code));
    REQUIRE(key.shared_memory_size == control.shared_memory);
    REQUIRE(key.workgroup_size == control.workgroup);

    // The entry has no recorded reads: without guesses they are errors, with them the guesses.
    REQUIRE_THROWS_AS(env.ReadCbufValue(0, 0x40), Shader::LogicError);
    static const VideoCommon::GuessMissing guess{};
    env.SetGuessMissing(&guess);
    REQUIRE(env.ReadCbufValue(0, 0x40) == guess.cbuf_bank0);
    REQUIRE(env.ReadCbufValue(3, 0x40) == guess.cbuf_other);
    REQUIRE(env.ReadTextureType(5) == guess.texture_type);
    REQUIRE(env.IsCbufStorageBufferPointer(0, 0x10) == guess.storage_pointer_bank0);
    REQUIRE(env.IsCbufStorageBufferPointer(1, 0x10) == guess.storage_pointer_other);
}

TEST_CASE("ShaderPrecache: a program naming no texture bank reads bank 0", "[video_core]") {
    ShaderWarm::LoweredCode lowered;
    lowered.code.resize(16 * sizeof(u64));
    ShaderWarm::ComputeKey key{};
    const auto entry = ShaderWarm::ComputeEntry(lowered, {{1, 1, 1}, 0, 0}, key);
    u32 texture_bound{};
    // num_envs, then six u64 counts and local memory before the texture bank.
    std::memcpy(&texture_bound, entry.data() + 4 + 6 * 8 + 4, sizeof(texture_bound));
    REQUIRE(texture_bound == 0);
}

TEST_CASE("ShaderPrecache: every container the archive walker reads is discovered",
          "[video_core]") {
    using ShaderWarm::ClassifyGameFile;
    using ShaderWarm::GameFileKind;
    for (const char* path : {"Shader/Model.product.nin_ounce_nvn2.bfsha.zs",
                             "Shader/Model.bfsha", "Pack/Shaders.nin_ounce_nvn2.release.sarc.zs",
                             "Pack/Shaders.release.sarc", "Shader/Loose.bnsh",
                             "Shader/Loose.bnsh.zs", "Shader/Bundle.cgsb", "Shader/Bundle.cgsb.zs",
                             "Shader/Program.shn", "Shader/Program.shn.zs", "Shader/Archive.sharcb",
                             "Shader/Archive.sharcb.zs", "Shader/UPPER.BNSH", "Effect/Fx.bfsha.zs",
                             "Shaders/Model.bfsha.zst"}) {
        INFO(path);
        REQUIRE(ClassifyGameFile(path) == GameFileKind::ShaderArchive);
    }
    REQUIRE(ClassifyGameFile("Effect/Fx.nin_ounce_nvn2.vsetb.byml.zs") == GameFileKind::Effect);
    REQUIRE(ClassifyGameFile("Effect/static.Nin_Ounce_NVN2.esetb.byml.zs") == GameFileKind::Effect);
    REQUIRE(ClassifyGameFile("Effect/Fx.ESETB.BYML.ZS") == GameFileKind::Effect);
    REQUIRE(ClassifyGameFile("Effect/Particle/Magic.ptcl.zst") == GameFileKind::Effect);
    REQUIRE(ClassifyGameFile("Effect/Particle/Magic.ptcl") == GameFileKind::Effect);
    REQUIRE(ClassifyGameFile("Pack/ZsDic.pack.zs") == GameFileKind::ZstdDictionaries);
    for (const char* path : {"Effect/Sub/Fx.vsetb.byml.zs", "Model/Fx.vsetb.byml.zs",
                             "Effect/Fx.vsetb.byml", "Effect/Sub/Fx.esetb.byml.zs",
                             "Model/Tex.bntx.zs", "Model/Mesh.bfres.zs", "Asset/assets_a000.dat",
                             "Bin/Nvn2-upper_left.ndsr", "Shader/notes.shn.txt", "Shader/bnsh"}) {
        INFO(path);
        REQUIRE(ClassifyGameFile(path) == GameFileKind::Other);
    }
}

TEST_CASE("ShaderPrecache: a particle effect without shader programs is not an error",
          "[video_core]") {
    std::vector<u8> effect(64, 0);
    std::memcpy(effect.data(), "VFXB", 4);
    size_t records = 0;
    size_t errors = 0;
    REQUIRE_NOTHROW(ShaderWarm::WalkArchive(
        effect, "Magic.ptcl", nullptr, [&](const ShaderWarm::ShaderRecord&) { ++records; },
        [&](const std::string&) { ++errors; }));
    REQUIRE(records == 0);
    REQUIRE(errors == 0);
}

TEST_CASE("ShaderPrecache: programs that cannot be lowered are refused", "[video_core]") {
    ShaderWarm::LoweredCode lowered;
    REQUIRE(ShaderWarm::LowerProgram({}, lowered).has_value());
    const std::array<u8, 15> partial{};
    REQUIRE(ShaderWarm::LowerProgram(partial, lowered).has_value());
    // One instruction with no exit: not a complete program.
    const std::array<u8, 16> no_exit{};
    REQUIRE(ShaderWarm::LowerProgram(no_exit, lowered).has_value());
    REQUIRE(lowered.code.empty());
}
