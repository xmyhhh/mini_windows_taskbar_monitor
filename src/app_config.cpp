#include "app_config.h"

#include <filesystem>
#include <string_view>

namespace minimal_taskbar_monitor {

namespace {

constexpr wchar_t kConfigFileName[] = L"config.json";

bool ReadBoolValue(const std::string& content, std::string_view key, bool default_value) {
    const std::string quoted_key = "\"" + std::string(key) + "\"";
    const size_t key_pos = content.find(quoted_key);
    if (key_pos == std::string::npos) {
        return default_value;
    }

    size_t colon_pos = content.find(':', key_pos + quoted_key.size());
    if (colon_pos == std::string::npos) {
        return default_value;
    }
    ++colon_pos;

    while (colon_pos < content.size() &&
           (content[colon_pos] == ' ' || content[colon_pos] == '\t' || content[colon_pos] == '\r' ||
            content[colon_pos] == '\n')) {
        ++colon_pos;
    }

    if (content.compare(colon_pos, 4, "true") == 0) {
        return true;
    }
    if (content.compare(colon_pos, 5, "false") == 0) {
        return false;
    }
    return default_value;
}

std::string ReadStringValue(const std::string& content,
                            std::string_view key,
                            std::string_view default_value) {
    const std::string quoted_key = "\"" + std::string(key) + "\"";
    const size_t key_pos = content.find(quoted_key);
    if (key_pos == std::string::npos) {
        return std::string(default_value);
    }

    size_t colon_pos = content.find(':', key_pos + quoted_key.size());
    if (colon_pos == std::string::npos) {
        return std::string(default_value);
    }
    ++colon_pos;

    while (colon_pos < content.size() &&
           (content[colon_pos] == ' ' || content[colon_pos] == '\t' || content[colon_pos] == '\r' ||
            content[colon_pos] == '\n')) {
        ++colon_pos;
    }

    if (colon_pos >= content.size() || content[colon_pos] != '"') {
        return std::string(default_value);
    }
    ++colon_pos;

    const size_t end_quote_pos = content.find('"', colon_pos);
    if (end_quote_pos == std::string::npos) {
        return std::string(default_value);
    }

    return content.substr(colon_pos, end_quote_pos - colon_pos);
}

std::string SerializeBool(const char* key, bool value) {
    return std::string("  \"") + key + "\": " + (value ? "true" : "false");
}

std::string SerializeString(const char* key, std::string_view value) {
    return std::string("  \"") + key + "\": \"" + std::string(value) + "\"";
}

NetworkDisplayUnit ParseNetworkDisplayUnit(std::string_view value) {
    if (value == "bytes") {
        return NetworkDisplayUnit::kBytesPerSecond;
    }
    return NetworkDisplayUnit::kBitsPerSecond;
}

std::string_view SerializeNetworkDisplayUnit(NetworkDisplayUnit value) {
    switch (value) {
    case NetworkDisplayUnit::kBytesPerSecond:
        return "bytes";
    case NetworkDisplayUnit::kBitsPerSecond:
    default:
        return "bits";
    }
}

}  // namespace

std::wstring GetAppConfigPath() {
    std::wstring executable_path(MAX_PATH, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size()));
    if (length == 0 || length >= executable_path.size()) {
        return L"config.json";
    }

    executable_path.resize(length);
    const std::filesystem::path executable_directory =
        std::filesystem::path(executable_path).parent_path();
    return (executable_directory / kConfigFileName).wstring();
}

AppConfig LoadAppConfig() {
    AppConfig config{};
    const std::filesystem::path config_path = GetAppConfigPath();
    HANDLE file_handle =
        CreateFileW(config_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_handle == INVALID_HANDLE_VALUE) {
        return config;
    }

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file_handle, &file_size) || file_size.QuadPart <= 0 ||
        file_size.QuadPart > 64 * 1024) {
        CloseHandle(file_handle);
        return config;
    }

    std::string content(static_cast<size_t>(file_size.QuadPart), '\0');
    DWORD bytes_read = 0;
    const BOOL read_ok = ReadFile(file_handle, content.data(),
                                  static_cast<DWORD>(content.size()), &bytes_read, nullptr);
    CloseHandle(file_handle);
    if (!read_ok) {
        return config;
    }
    content.resize(bytes_read);

    config.visible_metrics.show_cpu =
        ReadBoolValue(content, "show_cpu", config.visible_metrics.show_cpu);
    config.visible_metrics.show_memory =
        ReadBoolValue(content, "show_memory", config.visible_metrics.show_memory);
    config.visible_metrics.show_upload =
        ReadBoolValue(content, "show_upload", config.visible_metrics.show_upload);
    config.visible_metrics.show_download =
        ReadBoolValue(content, "show_download", config.visible_metrics.show_download);
    config.visible_metrics.show_gpu =
        ReadBoolValue(content, "show_gpu", config.visible_metrics.show_gpu);
    config.visible_metrics.show_disk_read =
        ReadBoolValue(content, "show_disk_read", config.visible_metrics.show_disk_read);
    config.visible_metrics.show_disk_write =
        ReadBoolValue(content, "show_disk_write", config.visible_metrics.show_disk_write);
    config.network_display_unit = ParseNetworkDisplayUnit(
        ReadStringValue(content, "network_unit", "bits"));
    return config;
}

bool SaveAppConfig(const AppConfig& config) {
    const std::filesystem::path config_path = GetAppConfigPath();
    std::error_code error_code;
    const std::filesystem::path parent_path = config_path.parent_path();
    if (!parent_path.empty()) {
        std::filesystem::create_directories(parent_path, error_code);
    }
    if (error_code) {
        return false;
    }

    std::string content;
    content += "{\n";
    content += SerializeBool("show_cpu", config.visible_metrics.show_cpu) + ",\n";
    content += SerializeBool("show_memory", config.visible_metrics.show_memory) + ",\n";
    content += SerializeBool("show_upload", config.visible_metrics.show_upload) + ",\n";
    content += SerializeBool("show_download", config.visible_metrics.show_download) + ",\n";
    content += SerializeBool("show_gpu", config.visible_metrics.show_gpu) + ",\n";
    content += SerializeBool("show_disk_read", config.visible_metrics.show_disk_read) + ",\n";
    content += SerializeBool("show_disk_write", config.visible_metrics.show_disk_write) + ",\n";
    content += SerializeString("network_unit",
                               SerializeNetworkDisplayUnit(config.network_display_unit)) + "\n";
    content += "}\n";

    HANDLE file_handle =
        CreateFileW(config_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD bytes_written = 0;
    const BOOL write_ok = WriteFile(file_handle, content.data(),
                                    static_cast<DWORD>(content.size()), &bytes_written, nullptr);
    CloseHandle(file_handle);
    return write_ok != FALSE && bytes_written == content.size();
}

}  // namespace minimal_taskbar_monitor
