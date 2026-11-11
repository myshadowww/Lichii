// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <fstream>
#include <random>
#include <catch2/catch_test_macros.hpp>
#include "yuzu/configuration/neural_model_sources.h"

namespace {
namespace fs = std::filesystem;

struct ModelDirectories {
    fs::path root = fs::temp_directory_path() /
                    ("lichii-model-sources-" + std::to_string(std::random_device{}()));
    NeuralModelSetup::SearchDirectories paths{
        .runtime = root / "portable profile" / "ngx",
        .data = root / "relocated data",
        .downloads = root / fs::path{u8"Téléchargements 模型"},
        .home = root / "home",
    };

    ModelDirectories() {
        fs::create_directories(paths.home);
    }
    ~ModelDirectories() {
        std::error_code error;
        fs::remove_all(root, error);
    }
    fs::path Add(const fs::path& directory) {
        fs::create_directories(directory);
        const auto path = directory / "nvngx_dlssnr.dll";
        std::ofstream{path} << "discovery fixture; not a valid model";
        return path;
    }
};
} // namespace

TEST_CASE("Neural model discovery uses supplied profile and download locations",
          "[neural_model_sources]") {
    ModelDirectories fixture;
    const auto profile = fixture.Add(fixture.paths.runtime);
    const auto layer = fixture.Add(fixture.paths.data / "dlssnr" / "binaries");
    const auto direct = fixture.Add(fixture.paths.downloads);
    const auto extracted =
        fixture.Add(fixture.paths.downloads / "renamed archive v2" / "runtime files");
    const std::vector expected{profile, layer, direct, extracted};
    CHECK(NeuralModelSetup::FindSources(fixture.paths) == expected);
}

TEST_CASE("Neural model discovery does not assume a Downloads folder name",
          "[neural_model_sources]") {
    ModelDirectories fixture;
    const auto selected = fixture.Add(fixture.paths.downloads / "arbitrary extraction");
    fixture.Add(fixture.paths.home / "Downloads");
    const std::vector expected{selected};
    CHECK(NeuralModelSetup::FindSources(fixture.paths) == expected);
}

TEST_CASE("Neural model discovery tolerates missing roots and ignores directories named as DLLs",
          "[neural_model_sources]") {
    ModelDirectories fixture;
    CHECK(NeuralModelSetup::FindSources(fixture.paths).empty());
    fs::create_directories(fixture.paths.downloads / "nvngx_dlssnr.dll");
    std::ofstream{fixture.paths.downloads / "nvngx_dlss.dll"} << "a different model";
    CHECK(NeuralModelSetup::FindSources(fixture.paths).empty());
    CHECK(NeuralModelSetup::FindSources({}).empty());
}

TEST_CASE("Neural model discovery bounds extracted directory depth", "[neural_model_sources]") {
    ModelDirectories fixture;
    const auto shallow = fixture.Add(fixture.paths.downloads / "one" / "two" / "three");
    fixture.Add(fixture.paths.downloads / "one" / "two" / "three" / "four");
    const std::vector expected{shallow};
    CHECK(NeuralModelSetup::FindSources(fixture.paths) == expected);
}

TEST_CASE("Neural model discovery checks only direct files when Downloads maps to Home",
          "[neural_model_sources]") {
    ModelDirectories fixture;
    fixture.paths.downloads = fixture.paths.home;
    const auto direct = fixture.Add(fixture.paths.home);
    fixture.Add(fixture.paths.home / "unrelated folder");
    const std::vector expected{direct};
    CHECK(NeuralModelSetup::FindSources(fixture.paths) == expected);
}

TEST_CASE("Neural model discovery does not follow nested directory symlinks",
          "[neural_model_sources]") {
    ModelDirectories fixture;
    fixture.Add(fixture.root / "elsewhere");
    fs::create_directories(fixture.paths.downloads);
    std::error_code error;
    fs::create_directory_symlink(fixture.root / "elsewhere", fixture.paths.downloads / "link", error);
    if (error)
        SKIP("Directory symlink creation is unavailable");
    CHECK(NeuralModelSetup::FindSources(fixture.paths).empty());
}
