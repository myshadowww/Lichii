// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <thread>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include "common/fs/path_util.h"
#include "frontend_common/play_time_manager.h"

namespace {

class PlayTimeDirectory {
public:
    PlayTimeDirectory()
        : original{Common::FS::GetEdenPath(Common::FS::EdenPath::PlayTimeDir)},
          path{std::filesystem::temp_directory_path() /
               fmt::format("lichii-playtime-{}", std::random_device{}())} {
        REQUIRE(std::filesystem::create_directory(path));
        Common::FS::SetEdenPath(Common::FS::EdenPath::PlayTimeDir, path);
    }
    ~PlayTimeDirectory() {
        Common::FS::SetEdenPath(Common::FS::EdenPath::PlayTimeDir, original);
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::string Read() const {
        std::ifstream file{path / "playtime.bin", std::ios::binary};
        return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    }
    const std::filesystem::path original;
    const std::filesystem::path path;
};

} // namespace

TEST_CASE("Playtime saves and removals survive reopening", "[playtime]") {
    PlayTimeDirectory directory;
    {
        PlayTime::PlayTimeManager manager;
        manager.SetPlayTime(10, 123);
        manager.SetPlayTime(20, 456);
        manager.SetPlayTime(0, 999);
        manager.ResetProgramPlayTime(10);
    }
    PlayTime::PlayTimeManager reopened;
    REQUIRE(reopened.GetPlayTime(10) == 0);
    REQUIRE(reopened.GetPlayTime(20) == 456);
    REQUIRE(reopened.GetPlayTime(0) == 0);
    REQUIRE_FALSE(std::filesystem::exists(directory.path / "playtime.bin.tmp"));
}

TEST_CASE("A failed playtime staging open retains the previous database", "[playtime]") {
    PlayTimeDirectory directory;
    PlayTime::PlayTimeManager manager;
    manager.SetPlayTime(10, 123);
    const auto previous = directory.Read();
    REQUIRE_FALSE(previous.empty());
    const auto temporary = directory.path / "playtime.bin.tmp";
    REQUIRE(std::filesystem::create_directory(temporary));
    manager.SetPlayTime(10, 456);
    REQUIRE(manager.GetPlayTime(10) == 456);
    REQUIRE(directory.Read() == previous);
    REQUIRE(std::filesystem::remove(temporary));
    manager.SetPlayTime(20, 789);
    PlayTime::PlayTimeManager reopened;
    REQUIRE(reopened.GetPlayTime(10) == 456);
    REQUIRE(reopened.GetPlayTime(20) == 789);
}

#ifndef _WIN32
TEST_CASE("A failed playtime staging flush retains the previous database", "[playtime]") {
    if (!std::filesystem::exists("/dev/full")) {
        SKIP("Requires /dev/full to fail buffered writes");
    }
    PlayTimeDirectory directory;
    PlayTime::PlayTimeManager manager;
    manager.SetPlayTime(10, 123);
    const auto previous = directory.Read();
    REQUIRE_FALSE(previous.empty());
    const auto temporary = directory.path / "playtime.bin.tmp";
    std::filesystem::create_symlink("/dev/full", temporary);
    manager.SetPlayTime(10, 456);
    REQUIRE(manager.GetPlayTime(10) == 456);
    REQUIRE(directory.Read() == previous);
    REQUIRE_FALSE(std::filesystem::exists(temporary));
}
#endif

TEST_CASE("Concurrent playtime updates and reads preserve complete saved snapshots", "[playtime]") {
    PlayTimeDirectory directory;
    constexpr u64 Writers = 4;
    constexpr u64 Updates = 24;
    {
        PlayTime::PlayTimeManager manager;
        manager.SetPlayTime(100, 1);
        manager.SetProgramId(100);
        manager.Start();
        std::atomic<bool> failed{};
        std::atomic<bool> started{};
        std::jthread reader{[&](std::stop_token stop) {
            std::array<u64, Writers> previous{};
            started.store(true);
            while (!stop.stop_requested()) {
                for (u64 i = 0; i < Writers; ++i) {
                    const auto value = manager.GetPlayTime(i + 1);
                    if (value < previous[i] || value > Updates) {
                        failed.store(true);
                    }
                    previous[i] = value;
                }
                // Every rename exposes a complete array of two-u64 database records.
                const auto snapshot = directory.Read();
                if (snapshot.empty() || snapshot.size() % (2 * sizeof(u64)) != 0) {
                    failed.store(true);
                }
                manager.SetProgramId(100);
                std::this_thread::yield();
            }
        }};
        while (!started.load()) {
            std::this_thread::yield();
        }
        std::vector<std::jthread> writers;
        for (u64 id = 1; id <= Writers; ++id) {
            writers.emplace_back([&, id] {
                for (u64 value = 1; value <= Updates; ++value) {
                    manager.SetPlayTime(id, value);
                }
            });
        }
        writers.clear();
        manager.Stop();
        reader.request_stop();
        reader.join();
        REQUIRE_FALSE(failed.load());
        for (u64 id = 1; id <= Writers; ++id) {
            REQUIRE(manager.GetPlayTime(id) == Updates);
        }
    }
    PlayTime::PlayTimeManager reopened;
    for (u64 id = 1; id <= Writers; ++id) {
        REQUIRE(reopened.GetPlayTime(id) == Updates);
    }
    REQUIRE_FALSE(std::filesystem::exists(directory.path / "playtime.bin.tmp"));
}
