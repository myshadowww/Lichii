// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <filesystem>
#include <vector>

namespace NeuralModelSetup {

struct SearchDirectories {
    std::filesystem::path runtime;
    std::filesystem::path data;
    std::filesystem::path downloads;
    std::filesystem::path home;
};

inline std::vector<std::filesystem::path> FindSources(const SearchDirectories& directories) {
    constexpr auto filename = "nvngx_dlssnr.dll";
    std::vector<std::filesystem::path> sources;
    const auto add = [&](const std::filesystem::path& path) {
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error) &&
            std::ranges::find(sources, path) == sources.end()) {
            sources.push_back(path);
        }
    };
    if (!directories.runtime.empty())
        add(directories.runtime / filename);
    if (!directories.data.empty())
        add(directories.data / "dlssnr" / "binaries" / filename);
    if (directories.downloads.empty())
        return sources;
    add(directories.downloads / filename);

    // XDG can map Downloads to Home when the download directory is disabled.
    std::error_code error;
    if (directories.downloads.lexically_normal() == directories.home.lexically_normal() ||
        std::filesystem::equivalent(directories.downloads, directories.home, error)) {
        return sources;
    }

    // Extracted archives may have any name. Bound the background search and do not
    // follow directory symlinks outside Downloads.
    constexpr int max_depth = 3;
    constexpr std::size_t max_entries = 4096;
    error.clear();
    std::filesystem::recursive_directory_iterator entry{
        directories.downloads, std::filesystem::directory_options::skip_permission_denied, error};
    const std::filesystem::recursive_directory_iterator end;
    for (std::size_t visited = 0; !error && entry != end && visited < max_entries;
         ++visited, entry.increment(error)) {
        if (entry.depth() >= max_depth)
            entry.disable_recursion_pending();
        if (entry->path().filename() == filename)
            add(entry->path());
    }
    return sources;
}

} // namespace NeuralModelSetup
