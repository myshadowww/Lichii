// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "frontend_common/nvidia_shader_cache.h"

namespace {

constexpr const char* CachePath = "__GL_SHADER_DISK_CACHE_PATH";
constexpr const char* CacheSize = "__GL_SHADER_DISK_CACHE_SIZE";

struct CacheFixture {
    std::array<std::optional<std::string>, 2> previous;
    std::filesystem::path root;

    CacheFixture() {
        auto pattern =
            (std::filesystem::temp_directory_path() / "lichii-nvidia-cache-XXXXXX").string();
        const char* directory = mkdtemp(pattern.data());
        REQUIRE(directory != nullptr);
        root = directory;
        size_t index = 0;
        for (const char* key : {CachePath, CacheSize}) {
            if (const char* value = std::getenv(key)) {
                previous[index] = value;
            }
            REQUIRE(unsetenv(key) == 0);
            ++index;
        }
    }

    ~CacheFixture() {
        size_t index = 0;
        for (const char* key : {CachePath, CacheSize}) {
            if (previous[index]) {
                setenv(key, previous[index]->c_str(), 1);
            } else {
                unsetenv(key);
            }
            ++index;
        }
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

std::optional<std::string> Environment(const char* key) {
    const char* value = std::getenv(key);
    return value ? std::optional<std::string>{value} : std::nullopt;
}

} // namespace

TEST_CASE("NVIDIA driver cache uses an isolated XDG directory and keeps its contents",
          "[frontend][nvidia_cache]") {
    CacheFixture f;
    const auto cache = f.root / "cache";
    FrontendCommon::ConfigureNvidiaShaderDiskCache(cache.c_str(), nullptr);
    const auto expected = cache / "lichii" / "nvidia";
    CHECK(Environment(CachePath) == expected.string());
    CHECK(Environment(CacheSize) == "10737418240");
    REQUIRE(std::filesystem::is_directory(expected));
    std::ofstream{expected / "existing-cache"} << "keep";
    FrontendCommon::ConfigureNvidiaShaderDiskCache(cache.c_str(), nullptr);
    CHECK(std::filesystem::file_size(expected / "existing-cache") == 4);
}

TEST_CASE("NVIDIA driver cache falls back only to an absolute user directory",
          "[frontend][nvidia_cache]") {
    CacheFixture f;
    SECTION("missing XDG directory") {
        FrontendCommon::ConfigureNvidiaShaderDiskCache(nullptr, f.root.c_str());
        CHECK(Environment(CachePath) == (f.root / ".cache/lichii/nvidia").string());
    }
    SECTION("relative XDG directory") {
        FrontendCommon::ConfigureNvidiaShaderDiskCache("relative-cache", f.root.c_str());
        CHECK(Environment(CachePath) == (f.root / ".cache/lichii/nvidia").string());
    }
    SECTION("no usable root") {
        FrontendCommon::ConfigureNvidiaShaderDiskCache("relative-cache", "relative-user");
        CHECK_FALSE(Environment(CachePath));
        CHECK(Environment(CacheSize) == "10737418240");
    }
}

TEST_CASE("NVIDIA driver cache preserves explicit settings independently",
          "[frontend][nvidia_cache]") {
    CacheFixture f;
    SECTION("custom location") {
        REQUIRE(setenv(CachePath, "/custom/cache", 1) == 0);
        FrontendCommon::ConfigureNvidiaShaderDiskCache(f.root.c_str(), nullptr);
        CHECK(Environment(CachePath) == "/custom/cache");
        CHECK(Environment(CacheSize) == "10737418240");
        CHECK_FALSE(std::filesystem::exists(f.root / "lichii"));
    }
    SECTION("custom size") {
        REQUIRE(setenv(CacheSize, "2147483648", 1) == 0);
        FrontendCommon::ConfigureNvidiaShaderDiskCache(f.root.c_str(), nullptr);
        CHECK(Environment(CachePath) == (f.root / "lichii/nvidia").string());
        CHECK(Environment(CacheSize) == "2147483648");
    }
    SECTION("explicit empty values") {
        REQUIRE(setenv(CachePath, "", 1) == 0);
        REQUIRE(setenv(CacheSize, "", 1) == 0);
        FrontendCommon::ConfigureNvidiaShaderDiskCache(f.root.c_str(), nullptr);
        CHECK(Environment(CachePath) == "");
        CHECK(Environment(CacheSize) == "");
        CHECK_FALSE(std::filesystem::exists(f.root / "lichii"));
    }
}

TEST_CASE("NVIDIA driver cache does not publish an unusable directory",
          "[frontend][nvidia_cache]") {
    CacheFixture f;
    std::ofstream{f.root / "lichii"} << "not a directory";
    FrontendCommon::ConfigureNvidiaShaderDiskCache(f.root.c_str(), nullptr);
    CHECK_FALSE(Environment(CachePath));
    CHECK(Environment(CacheSize) == "10737418240");
}
