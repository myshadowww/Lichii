// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <filesystem>
#include <system_error>

#include "frontend_common/nvidia_shader_cache.h"

namespace FrontendCommon {

void ConfigureNvidiaShaderDiskCache(const char* xdg_cache_home, const char* user_home) {
    // Give Lichii its own driver cache so other applications cannot trim it to their limit.
    if (std::getenv("__GL_SHADER_DISK_CACHE_PATH") == nullptr) {
        std::filesystem::path base;
        if (xdg_cache_home != nullptr && xdg_cache_home[0] == '/') {
            base = xdg_cache_home;
        } else if (user_home != nullptr && user_home[0] == '/') {
            base = std::filesystem::path{user_home} / ".cache";
        }
        if (!base.empty()) {
            const auto folder = base / "lichii" / "nvidia";
            std::error_code error;
            std::filesystem::create_directories(folder, error);
            if (!error) {
                setenv("__GL_SHADER_DISK_CACHE_PATH", folder.c_str(), 0);
            }
        }
    }
    setenv("__GL_SHADER_DISK_CACHE_SIZE", "10737418240", 0);
}

} // namespace FrontendCommon
