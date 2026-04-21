#pragma once

#include <windows.h>

#include <string>

namespace minimal_taskbar_monitor {

struct MetricsSnapshot {
    int cpu_percent{-1};
    int memory_percent{-1};
    int gpu_percent{-1};
    unsigned long long upload_bytes_per_second{0};
    unsigned long long download_bytes_per_second{0};
};

struct DisplayLines {
    std::wstring line1;
    std::wstring line2;
};

class SystemMetrics {
public:
    SystemMetrics();
    ~SystemMetrics();

    MetricsSnapshot Sample();

private:
    int SampleCpuPercent();
    int SampleMemoryPercent() const;
    int SampleGpuPercent();
    void SampleNetwork(unsigned long long& download_bytes_per_second,
                       unsigned long long& upload_bytes_per_second);

    bool QueryNetworkTotals(unsigned long long& total_in_bytes,
                            unsigned long long& total_out_bytes) const;

    bool cpu_initialized_{false};
    ULARGE_INTEGER last_idle_time_{};
    ULARGE_INTEGER last_kernel_time_{};
    ULARGE_INTEGER last_user_time_{};

    bool network_initialized_{false};
    unsigned long long last_total_in_bytes_{0};
    unsigned long long last_total_out_bytes_{0};
    ULONGLONG last_network_tick_{0};

    void* pdh_query_{nullptr};
    void* pdh_counter_{nullptr};
    bool gpu_query_initialized_{false};
};

DisplayLines FormatMetricsLines(const MetricsSnapshot& snapshot);
DisplayLines GetMetricsSampleLines();
bool IsLightTaskbarTheme();

}  // namespace minimal_taskbar_monitor
