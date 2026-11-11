// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <vector>
#include <QCheckBox>
#include <QLabel>
#include <QSlider>
#include <qnamespace.h>
#include "common/settings.h"
#include "core/core.h"
#include "qt_common/config/shared_translation.h"
#include "ui_configure_graphics_extensions.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_graphics_extensions.h"
#include "yuzu/configuration/graphics_settings.h"
#include "yuzu/configuration/shared_widget.h"

ConfigureGraphicsExtensions::ConfigureGraphicsExtensions(
    const Core::System& system_, std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group_,
    const ConfigurationShared::Builder& builder, QWidget* parent)
    : Tab(group_, parent), ui{std::make_unique<Ui::ConfigureGraphicsExtensions>()},
      system{system_} {

    ui->setupUi(this);

    Setup(builder);

    SetConfiguration();
}

ConfigureGraphicsExtensions::~ConfigureGraphicsExtensions() = default;

void ConfigureGraphicsExtensions::SetConfiguration() {}

void ConfigureGraphicsExtensions::Setup(const ConfigurationShared::Builder& builder) {
    for (auto* setting : ConfigurationShared::GetGraphicsSettings(
             ConfigurationShared::GraphicsPage::Compatibility)) {
        ConfigurationShared::Widget* widget = [&]() {
            if (setting->Id() == Settings::values.sample_shading.Id()) {
                // TODO(crueter): should support this natively perhaps?
                return builder.BuildWidget(
                    setting, apply_funcs, ConfigurationShared::RequestType::Slider, true, 1.0f,
                    nullptr, tr("%", "Sample Shading percentage (e.g. 50%)"));
            } else {
                return builder.BuildWidget(setting, apply_funcs);
            }
        }();

        if (widget == nullptr) {
            continue;
        }
        if (!widget->Valid()) {
            widget->deleteLater();
            continue;
        }

        auto* layout = ConfigurationShared::GetGraphicsSection(*setting) ==
                               ConfigurationShared::GraphicsSection::Vulkan
                           ? ui->vulkan_layout
                           : ui->hacks_layout;
        layout->addWidget(widget);

#ifdef __APPLE__
        if (setting->Id() == Settings::values.dyna_state.Id()) {
            widget->setEnabled(false);
            widget->setToolTip(tr("Extended Dynamic State is disabled on macOS due to MoltenVK "
                                  "compatibility issues that cause black screens."));
        }
#endif
    }
}

void ConfigureGraphicsExtensions::ApplyConfiguration() {
    const bool is_powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(is_powered_on);
    }
}

void ConfigureGraphicsExtensions::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureGraphicsExtensions::RetranslateUI() {
    ui->retranslateUi(this);
}
