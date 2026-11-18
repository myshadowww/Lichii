// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

namespace Settings {
class BasicSetting;
}

namespace ConfigurationShared {

enum class GraphicsPage { Graphics, Advanced, Compatibility, GameSpecific };

enum class GraphicsSection {
    Api,
    Display,
    ImageQuality,
    Upscaling,
    NeuralRendering,
    FrameGeneration,
    Accuracy,
    Shaders,
    Memory,
    Performance,
    Diagnostics,
    Workarounds,
    Vulkan,
    Ocarina,
    MarioKart,
};

// UI placement is independent of the persisted settings categories and keys.
GraphicsSection GetGraphicsSection(const Settings::BasicSetting& setting);
std::vector<Settings::BasicSetting*> GetGraphicsSettings(GraphicsPage page);

} // namespace ConfigurationShared
