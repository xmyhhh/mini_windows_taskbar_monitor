#pragma once

#include "system_metrics.h"

#include <string>

namespace minimal_taskbar_monitor {

enum class PopupActivationMode {
    kHover,
    kClick,
};

struct AppConfig {
    MetricVisibility visible_metrics{};
    NetworkDisplayUnit network_display_unit{NetworkDisplayUnit::kBitsPerSecond};
    PopupActivationMode popup_activation_mode{PopupActivationMode::kHover};
    unsigned int sample_interval_seconds{1};
};

AppConfig LoadAppConfig();
bool SaveAppConfig(const AppConfig& config);
std::wstring GetAppConfigPath();

}  // namespace minimal_taskbar_monitor
