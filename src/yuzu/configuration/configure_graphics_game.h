// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <vector>
#include "yuzu/configuration/configuration_shared.h"

namespace Core {
class System;
}
namespace Ui {
class ConfigureGraphicsGame;
}
namespace ConfigurationShared {
class Builder;
}

class ConfigureGraphicsGame : public ConfigurationShared::Tab {
    Q_OBJECT

public:
    explicit ConfigureGraphicsGame(const Core::System& system_,
                                   std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group,
                                   const ConfigurationShared::Builder& builder,
                                   QWidget* parent = nullptr);
    ~ConfigureGraphicsGame() override;

    void ApplyConfiguration() override;
    void SetConfiguration() override;

private:
    void changeEvent(QEvent* event) override;

    std::unique_ptr<Ui::ConfigureGraphicsGame> ui;
    const Core::System& system;
    std::vector<std::function<void(bool)>> apply_funcs;
};
