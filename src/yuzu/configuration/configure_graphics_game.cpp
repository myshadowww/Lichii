// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QEvent>
#include "core/core.h"
#include "ui_configure_graphics_game.h"
#include "yuzu/configuration/configure_graphics_game.h"
#include "yuzu/configuration/graphics_settings.h"
#include "yuzu/configuration/shared_widget.h"

ConfigureGraphicsGame::ConfigureGraphicsGame(
    const Core::System& system_, std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group,
    const ConfigurationShared::Builder& builder, QWidget* parent)
    : Tab(group, parent), ui{std::make_unique<Ui::ConfigureGraphicsGame>()}, system{system_} {
    ui->setupUi(this);
    for (auto* setting : ConfigurationShared::GetGraphicsSettings(
             ConfigurationShared::GraphicsPage::GameSpecific)) {
        auto* widget = builder.BuildWidget(setting, apply_funcs);
        if (!widget) {
            continue;
        }
        if (!widget->Valid()) {
            widget->deleteLater();
            continue;
        }
        auto* layout = ConfigurationShared::GetGraphicsSection(*setting) ==
                               ConfigurationShared::GraphicsSection::Ocarina
                           ? ui->ocarina_layout
                           : ui->mario_kart_layout;
        layout->addWidget(widget);
    }
}

ConfigureGraphicsGame::~ConfigureGraphicsGame() = default;

void ConfigureGraphicsGame::SetConfiguration() {}

void ConfigureGraphicsGame::ApplyConfiguration() {
    for (const auto& apply : apply_funcs) {
        apply(system.IsPoweredOn());
    }
}

void ConfigureGraphicsGame::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        ui->retranslateUi(this);
    }
    QWidget::changeEvent(event);
}
