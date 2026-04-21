#include "system_metrics.h"

#include <iphlpapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <winreg.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <map>
#include <string>
#include <vector>

namespace minimal_taskbar_monitor {

namespace {

constexpr wchar_t kGpuCounterPath[] = L"\\GPU Engine(*)\\Utilization Percentage";
constexpr wchar_t kThemeRegistryPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr wchar_t kThemeRegistryValue[] = L"SystemUsesLightTheme";

unsigned long long ToUnsignedLongLong(const FILETIME& value) {
    ULARGE_INTEGER combined{};
    combined.LowPart = value.dwLowDateTime;
    combined.HighPart = value.dwHighDateTime;
    return combined.QuadPart;
}

std::wstring FormatSpeed(unsigned long long bytes_per_second) {
    double value = static_cast<double>(bytes_per_second);
    const wchar_t* units[] = {L"B/s", L"KB/s", L"MB/s", L"GB/s"};
    size_t unit_index = 0;

    while (value >= 1024.0 && unit_index + 1 < _countof(units)) {
        value /= 1024.0;
        ++unit_index;
    }

    wchar_t buffer[64]{};
    if (value >= 100.0 || unit_index == 0) {
        swprintf_s(buffer, L"%.0f%ls", value, units[unit_index]);
    } else {
        swprintf_s(buffer, L"%.1f%ls", value, units[unit_index]);
    }

    return buffer;
}

bool QueryThemeValue(DWORD& value) {
    DWORD value_size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER,
                        kThemeRegistryPath,
                        kThemeRegistryValue,
                        RRF_RT_REG_DWORD,
                        nullptr,
                        &value,
                        &value_size) == ERROR_SUCCESS;
}

}  // namespace

SystemMetrics::SystemMetrics() {
    auto* query = new PDH_HQUERY{};
    auto* counter = new PDH_HCOUNTER{};
    pdh_query_ = query;
    pdh_counter_ = counter;

    if (PdhOpenQueryW(nullptr, 0, query) == ERROR_SUCCESS) {
        PDH_STATUS status = PdhAddCounterW(*query, kGpuCounterPath, 0, counter);
        if (status != ERROR_SUCCESS) {
            status = PdhAddEnglishCounterW(*query, kGpuCounterPath, 0, counter);
        }
        if (status == ERROR_SUCCESS) {
            PdhCollectQueryData(*query);
            gpu_query_initialized_ = true;
        }
    }
}

SystemMetrics::~SystemMetrics() {
    auto* query = static_cast<PDH_HQUERY*>(pdh_query_);
    auto* counter = static_cast<PDH_HCOUNTER*>(pdh_counter_);

    if (query != nullptr && *query != nullptr) {
        PdhCloseQuery(*query);
    }

    delete counter;
    delete query;
}

MetricsSnapshot SystemMetrics::Sample() {
    MetricsSnapshot snapshot{};
    snapshot.cpu_percent = SampleCpuPercent();
    snapshot.memory_percent = SampleMemoryPercent();
    snapshot.gpu_percent = SampleGpuPercent();
    SampleNetwork(snapshot.download_bytes_per_second, snapshot.upload_bytes_per_second);
    return snapshot;
}

int SystemMetrics::SampleCpuPercent() {
    FILETIME idle_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    if (!GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
        return -1;
    }

    const unsigned long long idle = ToUnsignedLongLong(idle_time);
    const unsigned long long kernel = ToUnsignedLongLong(kernel_time);
    const unsigned long long user = ToUnsignedLongLong(user_time);

    if (!cpu_initialized_) {
        last_idle_time_.QuadPart = idle;
        last_kernel_time_.QuadPart = kernel;
        last_user_time_.QuadPart = user;
        cpu_initialized_ = true;
        return 0;
    }

    const unsigned long long idle_delta = idle - last_idle_time_.QuadPart;
    const unsigned long long kernel_delta = kernel - last_kernel_time_.QuadPart;
    const unsigned long long user_delta = user - last_user_time_.QuadPart;
    const unsigned long long total_delta = kernel_delta + user_delta;

    last_idle_time_.QuadPart = idle;
    last_kernel_time_.QuadPart = kernel;
    last_user_time_.QuadPart = user;

    if (total_delta == 0) {
        return 0;
    }

    const double used_ratio =
        static_cast<double>(total_delta - idle_delta) / static_cast<double>(total_delta);
    const int percent = static_cast<int>(std::lround(used_ratio * 100.0));
    return std::clamp(percent, 0, 100);
}

int SystemMetrics::SampleMemoryPercent() const {
    MEMORYSTATUSEX memory_status{};
    memory_status.dwLength = sizeof(memory_status);
    if (!GlobalMemoryStatusEx(&memory_status)) {
        return -1;
    }
    return static_cast<int>(memory_status.dwMemoryLoad);
}

int SystemMetrics::SampleGpuPercent() {
    if (!gpu_query_initialized_) {
        return -1;
    }

    auto* query = static_cast<PDH_HQUERY*>(pdh_query_);
    auto* counter = static_cast<PDH_HCOUNTER*>(pdh_counter_);
    if (PdhCollectQueryData(*query) != ERROR_SUCCESS) {
        return -1;
    }

    DWORD buffer_size = 0;
    DWORD item_count = 0;
    PDH_STATUS status =
        PdhGetFormattedCounterArrayW(*counter, PDH_FMT_DOUBLE, &buffer_size, &item_count, nullptr);
    if (status != PDH_MORE_DATA || buffer_size == 0) {
        return -1;
    }

    std::vector<BYTE> buffer(buffer_size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    status = PdhGetFormattedCounterArrayW(
        *counter, PDH_FMT_DOUBLE, &buffer_size, &item_count, items);
    if (status != ERROR_SUCCESS) {
        return -1;
    }

    std::map<std::wstring, double> usage_by_engine;
    for (DWORD i = 0; i < item_count; ++i) {
        std::wstring engine_name = items[i].szName ? items[i].szName : L"";
        const size_t suffix_pos = engine_name.rfind(L'_');
        if (suffix_pos != std::wstring::npos && suffix_pos + 1 < engine_name.size()) {
            engine_name = engine_name.substr(suffix_pos + 1);
        }
        usage_by_engine[engine_name] += items[i].FmtValue.doubleValue;
    }

    double max_usage = 0.0;
    for (const auto& [_, usage] : usage_by_engine) {
        max_usage = std::max(max_usage, usage);
    }

    return std::clamp(static_cast<int>(std::lround(max_usage)), 0, 100);
}

void SystemMetrics::SampleNetwork(unsigned long long& download_bytes_per_second,
                                  unsigned long long& upload_bytes_per_second) {
    unsigned long long total_in_bytes = 0;
    unsigned long long total_out_bytes = 0;
    if (!QueryNetworkTotals(total_in_bytes, total_out_bytes)) {
        download_bytes_per_second = 0;
        upload_bytes_per_second = 0;
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (!network_initialized_) {
        last_total_in_bytes_ = total_in_bytes;
        last_total_out_bytes_ = total_out_bytes;
        last_network_tick_ = now;
        network_initialized_ = true;
        download_bytes_per_second = 0;
        upload_bytes_per_second = 0;
        return;
    }

    const ULONGLONG elapsed_ms = now - last_network_tick_;
    if (elapsed_ms == 0) {
        download_bytes_per_second = 0;
        upload_bytes_per_second = 0;
        return;
    }

    const unsigned long long in_delta = total_in_bytes - last_total_in_bytes_;
    const unsigned long long out_delta = total_out_bytes - last_total_out_bytes_;

    download_bytes_per_second = (in_delta * 1000ULL) / elapsed_ms;
    upload_bytes_per_second = (out_delta * 1000ULL) / elapsed_ms;

    last_total_in_bytes_ = total_in_bytes;
    last_total_out_bytes_ = total_out_bytes;
    last_network_tick_ = now;
}

bool SystemMetrics::QueryNetworkTotals(unsigned long long& total_in_bytes,
                                       unsigned long long& total_out_bytes) const {
    total_in_bytes = 0;
    total_out_bytes = 0;

    ULONG buffer_size = 0;
    if (GetIfTable(nullptr, &buffer_size, FALSE) != ERROR_INSUFFICIENT_BUFFER || buffer_size == 0) {
        return false;
    }

    std::vector<BYTE> buffer(buffer_size);
    auto* interface_table = reinterpret_cast<MIB_IFTABLE*>(buffer.data());
    if (GetIfTable(interface_table, &buffer_size, FALSE) != NO_ERROR) {
        return false;
    }

    for (DWORD i = 0; i < interface_table->dwNumEntries; ++i) {
        const MIB_IFROW& row = interface_table->table[i];
        if (row.dwType == IF_TYPE_SOFTWARE_LOOPBACK || row.dwType == IF_TYPE_TUNNEL) {
            continue;
        }
        if (row.dwOperStatus != IF_OPER_STATUS_OPERATIONAL &&
            row.dwOperStatus != IF_OPER_STATUS_CONNECTED) {
            continue;
        }
        total_in_bytes += row.dwInOctets;
        total_out_bytes += row.dwOutOctets;
    }

    return true;
}

DisplayLines FormatMetricsLines(const MetricsSnapshot& snapshot) {
    DisplayLines lines{};
    const std::wstring up_text = FormatSpeed(snapshot.upload_bytes_per_second);
    const std::wstring down_text = FormatSpeed(snapshot.download_bytes_per_second);

    wchar_t line1_buffer[96]{};
    swprintf_s(line1_buffer,
               L"CPU %d%%  MEM %d%%",
               std::max(snapshot.cpu_percent, 0),
               std::max(snapshot.memory_percent, 0));
    lines.line1 = line1_buffer;

    wchar_t line2_buffer[160]{};
    if (snapshot.gpu_percent >= 0) {
        swprintf_s(line2_buffer,
                   L"UP %ls  DN %ls  GPU %d%%",
                   up_text.c_str(),
                   down_text.c_str(),
                   snapshot.gpu_percent);
    } else {
        swprintf_s(line2_buffer,
                   L"UP %ls  DN %ls  GPU --",
                   up_text.c_str(),
                   down_text.c_str());
    }
    lines.line2 = line2_buffer;

    return lines;
}

DisplayLines GetMetricsSampleLines() {
    return {L"CPU 100%  MEM 100%", L"UP 99.9GB/s  DN 99.9GB/s  GPU 100%"};
}

bool IsLightTaskbarTheme() {
    DWORD value = 0;
    if (!QueryThemeValue(value)) {
        return false;
    }
    return value != 0;
}

}  // namespace minimal_taskbar_monitor
