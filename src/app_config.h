#pragma once

#include "system_metrics.h"

#include <string>

namespace minimal_taskbar_monitor {

struct AppConfig {
    MetricVisibility visible_metrics{};
    NetworkDisplayUnit network_display_unit{NetworkDisplayUnit::kBitsPerSecond};
};

AppConfig LoadAppConfig();
bool SaveAppConfig(const AppConfig& config);
std::wstring GetAppConfigPath();

}  // namespace minimal_taskbar_monitor
