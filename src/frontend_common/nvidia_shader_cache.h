// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace FrontendCommon {

// Call before loading the graphics driver. Explicit NVIDIA cache settings are preserved.
void ConfigureNvidiaShaderDiskCache(const char* xdg_cache_home, const char* user_home);

} // namespace FrontendCommon
