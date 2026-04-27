#include "app_config.h"
#include "process_monitor.h"
#include "resource.h"
#include "system_metrics.h"
#include "taskbar_embedder.h"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace minimal_taskbar_monitor {

namespace {

constexpr wchar_t kControllerClassName[] = L"MinimalTaskbarMonitorControllerWindow";
constexpr wchar_t kWidgetClassName[] = L"MinimalTaskbarMonitorWidgetWindow";
constexpr wchar_t kHoverPopupClassName[] = L"MinimalTaskbarMonitorHoverPopupWindow";
constexpr UINT_PTR kSampleTimerId = 1;
constexpr UINT_PTR kLayoutTimerId = 2;
constexpr UINT_PTR kReattachTimerId = 3;
constexpr UINT_PTR kHoverHideTimerId = 4;
constexpr UINT kLayoutIntervalMs = 1000;
constexpr UINT kReattachDelayMs = 600;
constexpr UINT kHoverHideDelayMs = 180;
constexpr UINT kTrayIconCallbackMessage = WM_APP + 1;
constexpr UINT kTrayIconId = 1;
constexpr UINT kExitCommandId = 1001;
constexpr UINT kAutoStartCommandId = 1002;
constexpr UINT kNetworkUnitsBitsCommandId = 1003;
constexpr UINT kNetworkUnitsBytesCommandId = 1004;
constexpr UINT kPopupModeHoverCommandId = 1005;
constexpr UINT kPopupModeClickCommandId = 1006;
constexpr UINT kSampleInterval1sCommandId = 1007;
constexpr UINT kSampleInterval2sCommandId = 1008;
constexpr UINT kSampleInterval5sCommandId = 1009;
constexpr UINT kSampleInterval10sCommandId = 1010;
constexpr UINT kMetricCpuCommandId = 1101;
constexpr UINT kMetricMemoryCommandId = 1102;
constexpr UINT kMetricUploadCommandId = 1103;
constexpr UINT kMetricDownloadCommandId = 1104;
constexpr UINT kMetricGpuCommandId = 1105;
constexpr UINT kMetricDiskReadCommandId = 1106;
constexpr UINT kMetricDiskWriteCommandId = 1107;
constexpr wchar_t kRunRegistryPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"MinimalTaskbarMonitor";
constexpr int kHoverPopupMaxVisibleRows = 14;
constexpr int kHoverPopupWheelRows = 3;

bool IsSupportedSampleIntervalSeconds(unsigned int seconds) {
    return seconds == 1 || seconds == 2 || seconds == 5 || seconds == 10;
}

UINT GetSampleTimerIntervalMs(unsigned int seconds) {
    if (!IsSupportedSampleIntervalSeconds(seconds)) {
        seconds = 1;
    }
    return seconds * 1000u;
}

struct WidgetPalette {
    COLORREF background;
    COLORREF border;
    COLORREF primary_text;
    COLORREF secondary_text;
};

struct HoverPopupPalette {
    COLORREF background;
    COLORREF border;
    COLORREF title_text;
    COLORREF primary_text;
    COLORREF secondary_text;
    COLORREF warning_text;
    COLORREF danger_text;
    COLORREF accent;
    COLORREF header_fill;
    COLORREF row_highlight;
    COLORREF search_fill;
    COLORREF search_border;
    COLORREF search_active_border;
    COLORREF search_placeholder;
};

enum class HoverPopupSortMode {
    kDefault,
    kCpu,
    kMemory,
    kGpu,
    kVram,
    kIo,
    kNetwork
};

struct HoverPopupTableLayout {
    RECT search_rect{};
    RECT header_rect{};
    RECT list_rect{};
    RECT rank_rect{};
    RECT name_rect{};
    RECT cpu_rect{};
    RECT mem_rect{};
    RECT gpu_rect{};
    RECT vram_rect{};
    RECT io_rect{};
    RECT net_rect{};
};

struct BgraPixel {
    BYTE blue;
    BYTE green;
    BYTE red;
    BYTE alpha;
};

int ScaleByDpi(UINT dpi, int value) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

WidgetPalette GetWidgetPalette(bool light_theme) {
    if (light_theme) {
        return {RGB(244, 246, 248), RGB(211, 216, 222), RGB(24, 28, 32), RGB(84, 91, 99)};
    }
    return {RGB(36, 39, 45), RGB(67, 72, 80), RGB(245, 247, 250), RGB(181, 188, 198)};
}

HoverPopupPalette GetHoverPopupPalette(bool light_theme) {
    if (light_theme) {
        return {RGB(251, 252, 254),
                RGB(212, 218, 226),
                RGB(17, 23, 31),
                RGB(36, 43, 51),
                RGB(102, 110, 120),
                RGB(182, 118, 0),
                RGB(196, 64, 52),
                RGB(55, 118, 206),
                RGB(240, 244, 249),
                RGB(247, 249, 252),
                RGB(214, 220, 228),
                RGB(55, 118, 206),
                RGB(132, 140, 150)};
    }

    return {RGB(28, 31, 36),
            RGB(68, 74, 83),
            RGB(246, 248, 250),
            RGB(223, 228, 233),
            RGB(152, 160, 171),
            RGB(255, 188, 84),
            RGB(255, 120, 120),
            RGB(126, 182, 255),
            RGB(38, 43, 50),
            RGB(33, 37, 43),
            RGB(72, 79, 89),
            RGB(126, 182, 255),
            RGB(131, 139, 149)};
}

enum class HoverAlertLevel {
    kNone,
    kWarning,
    kDanger
};

SIZE MeasureText(HDC dc, const std::wstring& text) {
    SIZE text_size{};
    if (!text.empty()) {
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &text_size);
    }
    return text_size;
}

std::vector<int> MeasureColumnWidths(HDC dc, const std::vector<DisplayLines::Column>& columns) {
    std::vector<int> column_widths;
    column_widths.reserve(columns.size());

    for (const auto& column : columns) {
        const SIZE top_size = MeasureText(dc, column.top_text);
        const SIZE bottom_size = MeasureText(dc, column.bottom_text);
        column_widths.push_back(std::max(top_size.cx, bottom_size.cx));
    }

    return column_widths;
}

int SumColumnWidths(const std::vector<int>& column_widths, int column_gap) {
    int total_width = 0;
    for (size_t i = 0; i < column_widths.size(); ++i) {
        if (i != 0) {
            total_width += column_gap;
        }
        total_width += column_widths[i];
    }
    return total_width;
}

std::wstring FormatRate(unsigned long long bytes_per_second) {
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

std::wstring FormatNetworkRate(unsigned long long bytes_per_second,
                               NetworkDisplayUnit network_display_unit) {
    return FormatNetworkRateForDisplay(bytes_per_second, network_display_unit);
}

const wchar_t* GetNetworkUnitFooterText(NetworkDisplayUnit network_display_unit) {
    switch (network_display_unit) {
    case NetworkDisplayUnit::kBytesPerSecond:
        return L"* VRAM shows dedicated GPU memory. NET is shown in B/s and estimated from IO-other activity.";
    case NetworkDisplayUnit::kBitsPerSecond:
    default:
        return L"* VRAM shows dedicated GPU memory. NET is shown in bit/s and estimated from IO-other activity.";
    }
}

std::wstring FormatBytes(unsigned long long byte_count) {
    double value = static_cast<double>(byte_count);
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
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

std::wstring FormatPercentValue(double percent) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%.0f%%", std::max(percent, 0.0));
    return buffer;
}

std::wstring ToLowerCopy(const std::wstring& text) {
    std::wstring lower = text;
    std::transform(lower.begin(),
                   lower.end(),
                   lower.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return lower;
}

bool ContainsCaseInsensitive(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) {
        return true;
    }

    return ToLowerCopy(haystack).find(ToLowerCopy(needle)) != std::wstring::npos;
}

HoverAlertLevel GetUsageAlertLevel(double value, double warning_threshold, double danger_threshold) {
    if (value >= danger_threshold) {
        return HoverAlertLevel::kDanger;
    }
    if (value >= warning_threshold) {
        return HoverAlertLevel::kWarning;
    }
    return HoverAlertLevel::kNone;
}

COLORREF ResolveAlertColor(const HoverPopupPalette& palette,
                           HoverAlertLevel level,
                           COLORREF default_color) {
    switch (level) {
    case HoverAlertLevel::kDanger:
        return palette.danger_text;
    case HoverAlertLevel::kWarning:
        return palette.warning_text;
    case HoverAlertLevel::kNone:
    default:
        return default_color;
    }
}

std::wstring FormatUptime(ULONGLONG uptime_ms) {
    unsigned long long total_minutes = uptime_ms / 60000ULL;
    const unsigned long long days = total_minutes / (24ULL * 60ULL);
    total_minutes %= (24ULL * 60ULL);
    const unsigned long long hours = total_minutes / 60ULL;
    const unsigned long long minutes = total_minutes % 60ULL;

    wchar_t buffer[64]{};
    if (days > 0) {
        swprintf_s(buffer, L"%llud %02lluh %02llum", days, hours, minutes);
    } else {
        swprintf_s(buffer, L"%02lluh %02llum", hours, minutes);
    }
    return buffer;
}

bool IsPointInWindowRect(HWND window_handle, const POINT& screen_point) {
    if (window_handle == nullptr || !IsWindow(window_handle)) {
        return false;
    }

    RECT window_rect{};
    return GetWindowRect(window_handle, &window_rect) != FALSE &&
           PtInRect(&window_rect, screen_point) != FALSE;
}

bool IsNearlyWhite(const Gdiplus::Color& color) {
    return color.GetAlpha() > 0 && color.GetRed() >= 245 && color.GetGreen() >= 245 &&
           color.GetBlue() >= 245;
}

bool FindContentBounds(Gdiplus::Bitmap& bitmap, Gdiplus::Rect& bounds) {
    const UINT width = bitmap.GetWidth();
    const UINT height = bitmap.GetHeight();
    int min_x = static_cast<int>(width);
    int min_y = static_cast<int>(height);
    int max_x = -1;
    int max_y = -1;

    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            Gdiplus::Color color;
            if (bitmap.GetPixel(x, y, &color) != Gdiplus::Ok || IsNearlyWhite(color)) {
                continue;
            }

            min_x = std::min(min_x, static_cast<int>(x));
            min_y = std::min(min_y, static_cast<int>(y));
            max_x = std::max(max_x, static_cast<int>(x));
            max_y = std::max(max_y, static_cast<int>(y));
        }
    }

    if (max_x < min_x || max_y < min_y) {
        return false;
    }

    bounds = Gdiplus::Rect(min_x, min_y, max_x - min_x + 1, max_y - min_y + 1);
    return true;
}

HFONT CreatePreferredUiFont(UINT dpi, int point_size) {
    const LOGFONTW font_template{
        .lfHeight = -ScaleByDpi(dpi, point_size),
        .lfWidth = 0,
        .lfEscapement = 0,
        .lfOrientation = 0,
        .lfWeight = FW_NORMAL,
        .lfItalic = FALSE,
        .lfUnderline = FALSE,
        .lfStrikeOut = FALSE,
        .lfCharSet = DEFAULT_CHARSET,
        .lfOutPrecision = OUT_DEFAULT_PRECIS,
        .lfClipPrecision = CLIP_DEFAULT_PRECIS,
        .lfQuality = CLEARTYPE_NATURAL_QUALITY,
        .lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE,
    };

    const std::array<const wchar_t*, 4> preferred_faces = {
        L"Segoe UI Variable Text", L"Bahnschrift", L"Microsoft YaHei UI", L"Segoe UI"};

    HDC screen_dc = GetDC(nullptr);
    if (screen_dc == nullptr) {
        return CreateFontW(font_template.lfHeight,
                           0,
                           0,
                           0,
                           FW_NORMAL,
                           FALSE,
                           FALSE,
                           FALSE,
                           DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_NATURAL_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE,
                           L"Segoe UI");
    }

    for (const wchar_t* face_name : preferred_faces) {
        LOGFONTW requested_font = font_template;
        wcscpy_s(requested_font.lfFaceName, face_name);

        HFONT font_handle = CreateFontIndirectW(&requested_font);
        if (font_handle == nullptr) {
            continue;
        }

        HGDIOBJ previous_font = SelectObject(screen_dc, font_handle);
        wchar_t selected_face[LF_FACESIZE]{};
        const int face_length = GetTextFaceW(screen_dc, LF_FACESIZE, selected_face);
        SelectObject(screen_dc, previous_font);

        if (face_length > 0 && _wcsicmp(selected_face, face_name) == 0) {
            ReleaseDC(nullptr, screen_dc);
            return font_handle;
        }

        DeleteObject(font_handle);
    }

    ReleaseDC(nullptr, screen_dc);
    return CreateFontW(font_template.lfHeight,
                       0,
                       0,
                       0,
                       FW_NORMAL,
                       FALSE,
                       FALSE,
                       FALSE,
                       DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_NATURAL_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE,
                       L"Segoe UI");
}

int CountVisibleMetrics(const MetricVisibility& visibility) {
    return static_cast<int>(visibility.show_cpu) + static_cast<int>(visibility.show_memory) +
           static_cast<int>(visibility.show_upload) +
           static_cast<int>(visibility.show_download) + static_cast<int>(visibility.show_gpu) +
           static_cast<int>(visibility.show_disk_read) +
           static_cast<int>(visibility.show_disk_write);
}

std::wstring GetExecutablePath() {
    std::wstring path(MAX_PATH, L'\0');
    while (true) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return L"";
        }
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring GetAutoStartCommand() {
    const std::wstring executable_path = GetExecutablePath();
    if (executable_path.empty()) {
        return L"";
    }
    return L"\"" + executable_path + L"\"";
}

bool QueryAutoStartValue(std::wstring& value) {
    DWORD bytes = 0;
    LONG result =
        RegGetValueW(HKEY_CURRENT_USER, kRunRegistryPath, kRunValueName, RRF_RT_REG_SZ, nullptr,
                     nullptr, &bytes);
    if (result != ERROR_SUCCESS || bytes == 0) {
        value.clear();
        return false;
    }

    value.resize(bytes / sizeof(wchar_t));
    result = RegGetValueW(HKEY_CURRENT_USER,
                          kRunRegistryPath,
                          kRunValueName,
                          RRF_RT_REG_SZ,
                          nullptr,
                          value.data(),
                          &bytes);
    if (result != ERROR_SUCCESS) {
        value.clear();
        return false;
    }

    if (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return true;
}

bool IsAutoStartEnabled() {
    std::wstring stored_value;
    if (!QueryAutoStartValue(stored_value)) {
        return false;
    }

    const std::wstring current_command = GetAutoStartCommand();
    if (current_command.empty()) {
        return !stored_value.empty();
    }

    return CompareStringOrdinal(stored_value.c_str(), -1, current_command.c_str(), -1, TRUE) ==
           CSTR_EQUAL;
}

bool SetAutoStartEnabled(bool enabled) {
    HKEY run_key = nullptr;
    const LONG open_result = RegCreateKeyExW(HKEY_CURRENT_USER,
                                             kRunRegistryPath,
                                             0,
                                             nullptr,
                                             0,
                                             KEY_SET_VALUE,
                                             nullptr,
                                             &run_key,
                                             nullptr);
    if (open_result != ERROR_SUCCESS) {
        return false;
    }

    bool success = false;
    if (enabled) {
        const std::wstring command = GetAutoStartCommand();
        if (!command.empty()) {
            const DWORD bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
            success = RegSetValueExW(run_key,
                                     kRunValueName,
                                     0,
                                     REG_SZ,
                                     reinterpret_cast<const BYTE*>(command.c_str()),
                                     bytes) == ERROR_SUCCESS;
        }
    } else {
        const LONG delete_result = RegDeleteValueW(run_key, kRunValueName);
        success = delete_result == ERROR_SUCCESS || delete_result == ERROR_FILE_NOT_FOUND;
    }

    RegCloseKey(run_key);
    return success;
}

}  // namespace

class MonitorApp {
public:
    int Run(HINSTANCE instance_handle, int) {
        instance_handle_ = instance_handle;
        taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");
        app_config_ = LoadAppConfig();
        SaveAppConfig(app_config_);

        RegisterWindowClasses();

        controller_window_ = CreateWindowExW(0,
                                             kControllerClassName,
                                             L"Minimal Taskbar Monitor Controller",
                                             WS_OVERLAPPEDWINDOW,
                                             CW_USEDEFAULT,
                                             CW_USEDEFAULT,
                                             CW_USEDEFAULT,
                                             CW_USEDEFAULT,
                                             nullptr,
                                             nullptr,
                                             instance_handle_,
                                             this);
        if (controller_window_ == nullptr) {
            return 1;
        }

        ShowWindow(controller_window_, SW_HIDE);
        UpdateWindow(controller_window_);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        return static_cast<int>(message.wParam);
    }

private:
    void RegisterWindowClasses() const {
        HICON app_icon = LoadIconW(instance_handle_, MAKEINTRESOURCEW(IDI_APP_ICON));

        WNDCLASSEXW controller_class{};
        controller_class.cbSize = sizeof(controller_class);
        controller_class.lpfnWndProc = &MonitorApp::ControllerWindowProc;
        controller_class.hInstance = instance_handle_;
        controller_class.lpszClassName = kControllerClassName;
        controller_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        controller_class.hIcon = app_icon;
        controller_class.hIconSm = app_icon;
        RegisterClassExW(&controller_class);

        WNDCLASSEXW widget_class{};
        widget_class.cbSize = sizeof(widget_class);
        widget_class.lpfnWndProc = &MonitorApp::WidgetWindowProc;
        widget_class.hInstance = instance_handle_;
        widget_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        widget_class.hbrBackground = nullptr;
        widget_class.lpszClassName = kWidgetClassName;
        widget_class.hIcon = app_icon;
        widget_class.hIconSm = app_icon;
        RegisterClassExW(&widget_class);

        WNDCLASSEXW popup_class{};
        popup_class.cbSize = sizeof(popup_class);
        popup_class.lpfnWndProc = &MonitorApp::HoverPopupWindowProc;
        popup_class.hInstance = instance_handle_;
        popup_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        popup_class.hbrBackground = nullptr;
        popup_class.lpszClassName = kHoverPopupClassName;
        popup_class.hIcon = app_icon;
        popup_class.hIconSm = app_icon;
        RegisterClassExW(&popup_class);
    }

    bool Initialize() {
        last_snapshot_ = metrics_.Sample();
        UpdateDisplayLines(last_snapshot_);
        if (!EnsureWidgetWindow()) {
            return false;
        }
        RefreshFontAndSize();

        ReattachWidget();
        EnsureTrayIcon();

        SetTimer(controller_window_,
                 kSampleTimerId,
                 GetSampleTimerIntervalMs(app_config_.sample_interval_seconds),
                 nullptr);
        SetTimer(controller_window_, kLayoutTimerId, kLayoutIntervalMs, nullptr);
        return true;
    }

    bool EnsureWidgetWindow() {
        if (widget_window_ != nullptr && IsWindow(widget_window_)) {
            return true;
        }

        widget_window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                         kWidgetClassName,
                                         L"Minimal Taskbar Monitor",
                                         WS_POPUP,
                                         0,
                                         0,
                                         0,
                                         0,
                                         nullptr,
                                         nullptr,
                                         instance_handle_,
                                         this);
        return widget_window_ != nullptr;
    }

    bool EnsureHoverPopupWindow() {
        if (hover_popup_window_ != nullptr && IsWindow(hover_popup_window_)) {
            return true;
        }

        hover_popup_window_ =
            CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                            kHoverPopupClassName,
                            L"Minimal Taskbar Monitor Popup",
                            WS_POPUP | WS_VSCROLL,
                            0,
                            0,
                            0,
                            0,
                            nullptr,
                            nullptr,
                            instance_handle_,
                            this);
        return hover_popup_window_ != nullptr;
    }

    bool EnsureTrayIcon() {
        if (tray_icon_added_ || controller_window_ == nullptr || !IsWindow(controller_window_)) {
            return tray_icon_added_;
        }

        if (tray_icon_handle_ == nullptr) {
            tray_icon_handle_ = LoadTrayIcon();
        }

        NOTIFYICONDATAW notify_icon{};
        notify_icon.cbSize = sizeof(notify_icon);
        notify_icon.hWnd = controller_window_;
        notify_icon.uID = kTrayIconId;
        notify_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        notify_icon.uCallbackMessage = kTrayIconCallbackMessage;
        notify_icon.hIcon = tray_icon_handle_;
        wcscpy_s(notify_icon.szTip, L"Minimal Taskbar Monitor");

        tray_icon_added_ = Shell_NotifyIconW(NIM_ADD, &notify_icon) != FALSE;
        return tray_icon_added_;
    }

    void RemoveTrayIcon() {
        if (!tray_icon_added_) {
            return;
        }

        NOTIFYICONDATAW notify_icon{};
        notify_icon.cbSize = sizeof(notify_icon);
        notify_icon.hWnd = controller_window_;
        notify_icon.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &notify_icon);
        tray_icon_added_ = false;
    }

    void Shutdown() {
        is_shutting_down_ = true;
        KillTimer(controller_window_, kSampleTimerId);
        KillTimer(controller_window_, kLayoutTimerId);
        KillTimer(controller_window_, kReattachTimerId);
        KillTimer(controller_window_, kHoverHideTimerId);
        HideHoverPopup();
        RemoveTrayIcon();

        embedder_.Detach(widget_window_);

        if (tray_icon_handle_ != nullptr) {
            DestroyIcon(tray_icon_handle_);
            tray_icon_handle_ = nullptr;
        }

        ShutdownGdiplus();

        if (font_ != nullptr) {
            DeleteObject(font_);
            font_ = nullptr;
        }
        if (popup_font_ != nullptr) {
            DeleteObject(popup_font_);
            popup_font_ = nullptr;
        }
        if (popup_title_font_ != nullptr) {
            DeleteObject(popup_title_font_);
            popup_title_font_ = nullptr;
        }

        if (widget_window_ != nullptr && IsWindow(widget_window_)) {
            DestroyWindow(widget_window_);
            widget_window_ = nullptr;
        }
        if (hover_popup_window_ != nullptr && IsWindow(hover_popup_window_)) {
            DestroyWindow(hover_popup_window_);
            hover_popup_window_ = nullptr;
        }
    }

    void ReattachWidget() {
        HideHoverPopup();
        if (!EnsureWidgetWindow()) {
            SetTimer(controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
            return;
        }

        embedder_.Detach(widget_window_);
        if (!embedder_.Attach(widget_window_)) {
            ShowWindow(widget_window_, SW_HIDE);
            SetTimer(controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
            return;
        }

        RefreshFontAndSize();
        if (!embedder_.RefreshLayout(widget_window_, widget_size_)) {
            ShowWindow(widget_window_, SW_HIDE);
            SetTimer(controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
            return;
        }
        ShowWindow(widget_window_, SW_SHOWNOACTIVATE);
        RequestWidgetRedraw();
    }

    void RefreshFontAndSize() {
        const UINT dpi = embedder_.CurrentDpi();
        if (dpi != current_dpi_ || font_ == nullptr || popup_font_ == nullptr ||
            popup_title_font_ == nullptr) {
            current_dpi_ = dpi;
            if (font_ != nullptr) {
                DeleteObject(font_);
                font_ = nullptr;
            }
            if (popup_font_ != nullptr) {
                DeleteObject(popup_font_);
                popup_font_ = nullptr;
            }
            if (popup_title_font_ != nullptr) {
                DeleteObject(popup_title_font_);
                popup_title_font_ = nullptr;
            }

            font_ = CreatePreferredUiFont(current_dpi_, 12);
            popup_font_ = CreatePreferredUiFont(current_dpi_, 13);
            popup_title_font_ = CreatePreferredUiFont(current_dpi_, 15);
        }

        HDC screen_dc = GetDC(nullptr);
        HFONT active_font =
            font_ != nullptr ? font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ old_font = SelectObject(screen_dc, active_font);
        const DisplayLines sample_lines =
            GetMetricsSampleLines(app_config_.visible_metrics, app_config_.network_display_unit);
        const int column_gap = ScaleByDpi(current_dpi_, 8);
        column_widths_ = MeasureColumnWidths(screen_dc, sample_lines.columns);
        TEXTMETRICW text_metrics{};
        GetTextMetricsW(screen_dc, &text_metrics);

        HFONT popup_font =
            popup_font_ != nullptr ? popup_font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        SelectObject(screen_dc, popup_font);
        TEXTMETRICW popup_text_metrics{};
        GetTextMetricsW(screen_dc, &popup_text_metrics);

        HFONT popup_title_font =
            popup_title_font_ != nullptr ? popup_title_font_
                                         : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        SelectObject(screen_dc, popup_title_font);
        TEXTMETRICW popup_title_metrics{};
        GetTextMetricsW(screen_dc, &popup_title_metrics);

        SelectObject(screen_dc, old_font);
        ReleaseDC(nullptr, screen_dc);

        text_line_height_ = text_metrics.tmHeight;
        popup_text_line_height_ = popup_text_metrics.tmHeight;
        popup_title_line_height_ = popup_title_metrics.tmHeight;
        const int horizontal_padding = ScaleByDpi(current_dpi_, 8);
        const int vertical_padding = ScaleByDpi(current_dpi_, 5);
        const int line_gap = ScaleByDpi(current_dpi_, 2);
        const bool has_second_line = !sample_lines.line2.empty();
        const int content_width = SumColumnWidths(column_widths_, column_gap);

        widget_size_.cx = content_width + horizontal_padding * 2;
        widget_size_.cy =
            std::max<int>((has_second_line ? text_line_height_ * 2 + line_gap : text_line_height_) +
                              vertical_padding * 2,
                         ScaleByDpi(current_dpi_, has_second_line ? 32 : 24));

        UpdateHoverPopupSize();
    }

    void UpdateDisplayLines(const MetricsSnapshot& snapshot) {
        const DisplayLines lines = FormatMetricsLines(
            snapshot, app_config_.visible_metrics, app_config_.network_display_unit);
        line1_text_ = lines.line1;
        line2_text_ = lines.line2;
        display_columns_ = lines.columns;
        has_second_line_ = !line2_text_.empty();
    }

    void DrawWidgetContents(HDC dc, const RECT& client_rect) {
        const bool light_theme = IsLightTaskbarTheme();
        const WidgetPalette palette = GetWidgetPalette(light_theme);
        HBRUSH background_brush = CreateSolidBrush(palette.background);
        FillRect(dc, &client_rect, background_brush);
        DeleteObject(background_brush);
        HBRUSH border_brush = CreateSolidBrush(palette.border);
        FrameRect(dc, &client_rect, border_brush);
        DeleteObject(border_brush);

        SetBkMode(dc, TRANSPARENT);
        HFONT active_font =
            font_ != nullptr ? font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ old_font = SelectObject(dc, active_font);
        const int horizontal_padding = ScaleByDpi(current_dpi_, 8);
        const int vertical_padding = ScaleByDpi(current_dpi_, 4);
        const int line_gap = ScaleByDpi(current_dpi_, 2);
        const int column_gap = ScaleByDpi(current_dpi_, 8);
        const int centered_offset = std::max<int>(
            0, ((client_rect.bottom - client_rect.top) - text_line_height_) / 2);
        const int line1_top =
            has_second_line_ ? client_rect.top + vertical_padding : client_rect.top + centered_offset;

        RECT line1_rect{client_rect.left + horizontal_padding,
                        line1_top,
                        client_rect.right - horizontal_padding,
                        line1_top + text_line_height_};
        RECT line2_rect{client_rect.left + horizontal_padding,
                        client_rect.top + vertical_padding + text_line_height_ + line_gap,
                        client_rect.right - horizontal_padding,
                        client_rect.top + vertical_padding + text_line_height_ * 2 + line_gap};

        if (!display_columns_.empty()) {
            std::vector<int> active_column_widths = column_widths_;
            if (active_column_widths.size() != display_columns_.size()) {
                active_column_widths = MeasureColumnWidths(dc, display_columns_);
            }

            int current_x = client_rect.left + horizontal_padding;
            for (size_t i = 0; i < display_columns_.size(); ++i) {
                const int column_width = active_column_widths[i];
                RECT column_line1_rect{current_x,
                                       line1_rect.top,
                                       current_x + column_width,
                                       line1_rect.bottom};
                RECT column_line2_rect{current_x,
                                       line2_rect.top,
                                       current_x + column_width,
                                       line2_rect.bottom};

                if (!display_columns_[i].top_text.empty()) {
                    SetTextColor(dc, palette.primary_text);
                    DrawTextW(dc,
                              display_columns_[i].top_text.c_str(),
                              static_cast<int>(display_columns_[i].top_text.size()),
                              &column_line1_rect,
                              DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
                }

                if (has_second_line_ && !display_columns_[i].bottom_text.empty()) {
                    SetTextColor(dc, palette.secondary_text);
                    DrawTextW(dc,
                              display_columns_[i].bottom_text.c_str(),
                              static_cast<int>(display_columns_[i].bottom_text.size()),
                              &column_line2_rect,
                              DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
                }

                current_x += column_width + column_gap;
            }
        } else {
            SetTextColor(dc, palette.primary_text);
            DrawTextW(dc,
                      line1_text_.c_str(),
                      static_cast<int>(line1_text_.size()),
                      &line1_rect,
                      DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

            if (has_second_line_) {
                SetTextColor(dc, palette.secondary_text);
                DrawTextW(dc,
                          line2_text_.c_str(),
                          static_cast<int>(line2_text_.size()),
                          &line2_rect,
                          DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
            }
        }

        SelectObject(dc, old_font);
    }

    int GetHoverPopupRowHeight() const {
        return popup_text_line_height_ + ScaleByDpi(current_dpi_, 8);
    }

    int GetHoverPopupSearchHeight() const {
        return popup_text_line_height_ + ScaleByDpi(current_dpi_, 10);
    }

    int GetHoverPopupVisibleRowCount() const {
        return std::max(1,
                        std::min(kHoverPopupMaxVisibleRows,
                                 static_cast<int>(hover_popup_snapshot_.top_processes.size())));
    }

    int GetHoverPopupMaxScrollOffset() const {
        return std::max(0,
                        static_cast<int>(hover_popup_snapshot_.top_processes.size()) -
                            GetHoverPopupVisibleRowCount());
    }

    void ClampHoverPopupScrollOffset() {
        hover_popup_scroll_offset_ =
            std::clamp(hover_popup_scroll_offset_, 0, GetHoverPopupMaxScrollOffset());
    }

    void UpdateHoverPopupScrollBar() {
        if (hover_popup_window_ == nullptr || !IsWindow(hover_popup_window_)) {
            return;
        }

        ClampHoverPopupScrollOffset();
        SCROLLINFO scroll_info{};
        scroll_info.cbSize = sizeof(scroll_info);
        scroll_info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        scroll_info.nMin = 0;
        scroll_info.nMax =
            std::max(0, static_cast<int>(hover_popup_snapshot_.top_processes.size()) - 1);
        scroll_info.nPage = static_cast<UINT>(GetHoverPopupVisibleRowCount());
        scroll_info.nPos = hover_popup_scroll_offset_;
        SetScrollInfo(hover_popup_window_, SB_VERT, &scroll_info, TRUE);
        ShowScrollBar(hover_popup_window_,
                      SB_VERT,
                      hover_popup_snapshot_.top_processes.size() >
                          static_cast<size_t>(GetHoverPopupVisibleRowCount()));
    }

    void SetHoverPopupScrollOffset(int offset) {
        hover_popup_scroll_offset_ = offset;
        ClampHoverPopupScrollOffset();
        UpdateHoverPopupScrollBar();
        RequestHoverPopupRedraw();
    }

    void ScrollHoverPopupBy(int delta_rows) {
        if (delta_rows == 0) {
            return;
        }
        SetHoverPopupScrollOffset(hover_popup_scroll_offset_ + delta_rows);
    }

    void RefreshHoverPopupView(bool reposition = true) {
        ApplyHoverPopupSort();
        UpdateHoverPopupSize();
        UpdateHoverPopupScrollBar();
        if (reposition) {
            PositionHoverPopup();
        }
        RequestHoverPopupRedraw();
    }

    HoverPopupTableLayout ComputeHoverPopupTableLayout(const RECT& client_rect) const {
        const int padding_left = ScaleByDpi(current_dpi_, 16);
        const int padding_right = ScaleByDpi(current_dpi_, 30);
        const int gap = ScaleByDpi(current_dpi_, 8);
        const int line_gap = ScaleByDpi(current_dpi_, 4);
        const int inner_padding = ScaleByDpi(current_dpi_, 6);
        const int row_gap = ScaleByDpi(current_dpi_, 6);
        const int scroll_bar_width = GetSystemMetrics(SM_CXVSCROLL);
        const int content_left = client_rect.left + padding_left;
        const int content_right = client_rect.right - padding_right - scroll_bar_width;
        const int content_top =
            client_rect.top + padding_left + popup_title_line_height_ + gap;
        const int summary_bottom = content_top + popup_text_line_height_ * 3 + line_gap * 2;
        const int search_height = GetHoverPopupSearchHeight();
        const int row_height = GetHoverPopupRowHeight();
        const int visible_rows = GetHoverPopupVisibleRowCount();
        const int search_top = summary_bottom + gap;

        HoverPopupTableLayout layout{};
        layout.search_rect = {content_left, search_top, content_right, search_top + search_height};
        const int table_top = layout.search_rect.bottom + gap;
        layout.header_rect = {content_left,
                              table_top,
                              content_right,
                              table_top + popup_text_line_height_ + ScaleByDpi(current_dpi_, 8)};
        layout.list_rect = {content_left,
                            layout.header_rect.bottom + row_gap,
                            content_right,
                            layout.header_rect.bottom + row_gap + visible_rows * row_height};

        const int rank_width = ScaleByDpi(current_dpi_, 24);
        const int cpu_width = ScaleByDpi(current_dpi_, 56);
        const int mem_width = ScaleByDpi(current_dpi_, 190);
        const int gpu_width = ScaleByDpi(current_dpi_, 54);
        const int vram_width = ScaleByDpi(current_dpi_, 82);
        const int io_width = ScaleByDpi(current_dpi_, 92);
        const int net_width = ScaleByDpi(current_dpi_, 92);
        const int table_gap = ScaleByDpi(current_dpi_, 10);
        const int table_left = layout.header_rect.left + inner_padding;
        const int table_right =
            std::max(table_left, static_cast<int>(layout.header_rect.right) - inner_padding);
        const int available_width = table_right - table_left;
        const int fixed_width = rank_width + cpu_width + mem_width + gpu_width + vram_width +
                                io_width + net_width + table_gap * 6;
        const int name_width = std::max(0, available_width - fixed_width);

        int x = table_left;
        layout.rank_rect = {x, layout.header_rect.top, x + rank_width, layout.header_rect.bottom};
        x = layout.rank_rect.right + table_gap;
        layout.name_rect = {x, layout.header_rect.top, x + name_width, layout.header_rect.bottom};
        x = layout.name_rect.right + table_gap;
        layout.cpu_rect = {x, layout.header_rect.top, x + cpu_width, layout.header_rect.bottom};
        x = layout.cpu_rect.right + table_gap;
        layout.mem_rect = {x, layout.header_rect.top, x + mem_width, layout.header_rect.bottom};
        x = layout.mem_rect.right + table_gap;
        layout.gpu_rect = {x, layout.header_rect.top, x + gpu_width, layout.header_rect.bottom};
        x = layout.gpu_rect.right + table_gap;
        layout.vram_rect = {x, layout.header_rect.top, x + vram_width, layout.header_rect.bottom};
        x = layout.vram_rect.right + table_gap;
        layout.io_rect = {x, layout.header_rect.top, x + io_width, layout.header_rect.bottom};
        x = layout.io_rect.right + table_gap;
        layout.net_rect = {x, layout.header_rect.top, x + net_width, layout.header_rect.bottom};
        return layout;
    }

    std::wstring GetHoverPopupHeaderText(const wchar_t* label, HoverPopupSortMode mode) const {
        std::wstring text = label;
        if (hover_popup_sort_mode_ == mode && mode != HoverPopupSortMode::kDefault) {
            text += L" ↓";
        }
        return text;
    }

    std::wstring GetHoverPopupSubtitle() const {
        const size_t visible_count = hover_popup_snapshot_.top_processes.size();
        const int total_count = hover_popup_base_snapshot_.total_process_count;
        const std::wstring count_suffix =
            hover_popup_search_text_.empty()
                ? (total_count > 0 ? L"  (" + std::to_wstring(total_count) + L" shown)" : L"")
                : (L"  (" + std::to_wstring(visible_count) + L"/" +
                   std::to_wstring(std::max(total_count, 0)) + L")");
        switch (hover_popup_sort_mode_) {
        case HoverPopupSortMode::kCpu:
            return std::wstring(L"Processes sorted by CPU") + count_suffix;
        case HoverPopupSortMode::kMemory:
            return std::wstring(L"Processes sorted by memory (USS -> RSS -> VMS)") + count_suffix;
        case HoverPopupSortMode::kGpu:
            return std::wstring(L"Processes sorted by GPU") + count_suffix;
        case HoverPopupSortMode::kVram:
            return std::wstring(L"Processes sorted by VRAM") + count_suffix;
        case HoverPopupSortMode::kIo:
            return std::wstring(L"Processes sorted by IO") + count_suffix;
        case HoverPopupSortMode::kNetwork:
            return std::wstring(L"Processes sorted by network") + count_suffix;
        case HoverPopupSortMode::kDefault:
        default:
            return std::wstring(L"Processes by blended pressure score") + count_suffix;
        }
    }

    void ApplyHoverPopupSort() {
        hover_popup_snapshot_ = hover_popup_base_snapshot_;
        if (!hover_popup_search_text_.empty()) {
            const std::wstring needle = ToLowerCopy(hover_popup_search_text_);
            std::erase_if(hover_popup_snapshot_.top_processes,
                          [&needle](const ProcessPopupItem& item) {
                              return !ContainsCaseInsensitive(item.name, needle) &&
                                     std::to_wstring(item.pid).find(needle) == std::wstring::npos;
                          });
        }

        if (hover_popup_sort_mode_ == HoverPopupSortMode::kDefault) {
            ClampHoverPopupScrollOffset();
            return;
        }

        auto default_compare = [](const ProcessPopupItem& left, const ProcessPopupItem& right) {
            if (std::abs(left.score - right.score) > 0.01) {
                return left.score > right.score;
            }
            if (std::abs(left.cpu_percent - right.cpu_percent) > 0.01) {
                return left.cpu_percent > right.cpu_percent;
            }
            if (left.uss_bytes != right.uss_bytes) {
                return left.uss_bytes > right.uss_bytes;
            }
            if (left.rss_bytes != right.rss_bytes) {
                return left.rss_bytes > right.rss_bytes;
            }
            if (left.vms_bytes != right.vms_bytes) {
                return left.vms_bytes > right.vms_bytes;
            }
            if (std::abs(left.gpu_percent - right.gpu_percent) > 0.01) {
                return left.gpu_percent > right.gpu_percent;
            }
            if (left.vram_bytes != right.vram_bytes) {
                return left.vram_bytes > right.vram_bytes;
            }
            const unsigned long long left_io =
                left.io_read_bytes_per_second + left.io_write_bytes_per_second;
            const unsigned long long right_io =
                right.io_read_bytes_per_second + right.io_write_bytes_per_second;
            if (left_io != right_io) {
                return left_io > right_io;
            }
            if (left.network_bytes_per_second != right.network_bytes_per_second) {
                return left.network_bytes_per_second > right.network_bytes_per_second;
            }
            return left.pid < right.pid;
        };

        const auto compare_with_tie_break =
            [&default_compare](auto metric_compare) {
                return [default_compare, metric_compare](const ProcessPopupItem& left,
                                                         const ProcessPopupItem& right) {
                    if (metric_compare(left, right)) {
                        return true;
                    }
                    if (metric_compare(right, left)) {
                        return false;
                    }
                    return default_compare(left, right);
                };
            };

        switch (hover_popup_sort_mode_) {
        case HoverPopupSortMode::kCpu:
            std::stable_sort(hover_popup_snapshot_.top_processes.begin(),
                             hover_popup_snapshot_.top_processes.end(),
                             compare_with_tie_break([](const ProcessPopupItem& left,
                                                       const ProcessPopupItem& right) {
                                 return std::abs(left.cpu_percent - right.cpu_percent) > 0.01 &&
                                        left.cpu_percent > right.cpu_percent;
                             }));
            break;
        case HoverPopupSortMode::kMemory:
            std::stable_sort(hover_popup_snapshot_.top_processes.begin(),
                             hover_popup_snapshot_.top_processes.end(),
                             compare_with_tie_break([](const ProcessPopupItem& left,
                                                       const ProcessPopupItem& right) {
                                 if (left.uss_bytes != right.uss_bytes) {
                                     return left.uss_bytes > right.uss_bytes;
                                 }
                                 if (left.rss_bytes != right.rss_bytes) {
                                     return left.rss_bytes > right.rss_bytes;
                                 }
                                 if (left.vms_bytes != right.vms_bytes) {
                                     return left.vms_bytes > right.vms_bytes;
                                 }
                                 return false;
                             }));
            break;
        case HoverPopupSortMode::kGpu:
            std::stable_sort(hover_popup_snapshot_.top_processes.begin(),
                             hover_popup_snapshot_.top_processes.end(),
                             compare_with_tie_break([](const ProcessPopupItem& left,
                                                       const ProcessPopupItem& right) {
                                 if (std::abs(left.gpu_percent - right.gpu_percent) > 0.01) {
                                     return left.gpu_percent > right.gpu_percent;
                                 }
                                 if (left.vram_bytes != right.vram_bytes) {
                                     return left.vram_bytes > right.vram_bytes;
                                 }
                                 return false;
                             }));
            break;
        case HoverPopupSortMode::kVram:
            std::stable_sort(hover_popup_snapshot_.top_processes.begin(),
                             hover_popup_snapshot_.top_processes.end(),
                             compare_with_tie_break([](const ProcessPopupItem& left,
                                                       const ProcessPopupItem& right) {
                                 return left.vram_bytes != right.vram_bytes &&
                                        left.vram_bytes > right.vram_bytes;
                             }));
            break;
        case HoverPopupSortMode::kIo:
            std::stable_sort(hover_popup_snapshot_.top_processes.begin(),
                             hover_popup_snapshot_.top_processes.end(),
                             compare_with_tie_break([](const ProcessPopupItem& left,
                                                       const ProcessPopupItem& right) {
                                 const unsigned long long left_io =
                                     left.io_read_bytes_per_second + left.io_write_bytes_per_second;
                                 const unsigned long long right_io =
                                     right.io_read_bytes_per_second + right.io_write_bytes_per_second;
                                 return left_io != right_io && left_io > right_io;
                             }));
            break;
        case HoverPopupSortMode::kNetwork:
            std::stable_sort(hover_popup_snapshot_.top_processes.begin(),
                             hover_popup_snapshot_.top_processes.end(),
                             compare_with_tie_break([](const ProcessPopupItem& left,
                                                       const ProcessPopupItem& right) {
                                 return left.network_bytes_per_second != right.network_bytes_per_second &&
                                        left.network_bytes_per_second >
                                            right.network_bytes_per_second;
                             }));
            break;
        case HoverPopupSortMode::kDefault:
        default:
            break;
        }

        ClampHoverPopupScrollOffset();
    }

    HoverPopupSortMode HitTestHoverPopupSortMode(const POINT& client_point) const {
        if (hover_popup_window_ == nullptr || !IsWindow(hover_popup_window_)) {
            return HoverPopupSortMode::kDefault;
        }

        RECT client_rect{};
        GetClientRect(hover_popup_window_, &client_rect);
        const HoverPopupTableLayout layout = ComputeHoverPopupTableLayout(client_rect);
        if (PtInRect(&layout.cpu_rect, client_point)) {
            return HoverPopupSortMode::kCpu;
        }
        if (PtInRect(&layout.mem_rect, client_point)) {
            return HoverPopupSortMode::kMemory;
        }
        if (PtInRect(&layout.gpu_rect, client_point)) {
            return HoverPopupSortMode::kGpu;
        }
        if (PtInRect(&layout.vram_rect, client_point)) {
            return HoverPopupSortMode::kVram;
        }
        if (PtInRect(&layout.io_rect, client_point)) {
            return HoverPopupSortMode::kIo;
        }
        if (PtInRect(&layout.net_rect, client_point)) {
            return HoverPopupSortMode::kNetwork;
        }
        return HoverPopupSortMode::kDefault;
    }

    bool IsHoverPopupSearchHit(const POINT& client_point) const {
        if (hover_popup_window_ == nullptr || !IsWindow(hover_popup_window_)) {
            return false;
        }

        RECT client_rect{};
        GetClientRect(hover_popup_window_, &client_rect);
        const HoverPopupTableLayout layout = ComputeHoverPopupTableLayout(client_rect);
        return PtInRect(&layout.search_rect, client_point) != FALSE;
    }

    void SetHoverPopupSearchActive(bool active) {
        if (hover_popup_search_active_ == active) {
            return;
        }

        hover_popup_search_active_ = active;
        RequestHoverPopupRedraw();
    }

    void SetHoverPopupSearchText(const std::wstring& text) {
        if (hover_popup_search_text_ == text) {
            return;
        }

        hover_popup_search_text_ = text;
        hover_popup_scroll_offset_ = 0;
        RefreshHoverPopupView();
    }

    void AppendHoverPopupSearchCharacter(wchar_t ch) {
        if (hover_popup_search_text_.size() >= 64) {
            return;
        }

        std::wstring updated_text = hover_popup_search_text_;
        updated_text.push_back(ch);
        SetHoverPopupSearchText(updated_text);
    }

    void RemoveHoverPopupSearchCharacter() {
        if (hover_popup_search_text_.empty()) {
            return;
        }

        std::wstring updated_text = hover_popup_search_text_;
        updated_text.pop_back();
        SetHoverPopupSearchText(updated_text);
    }

    bool HandleHoverPopupSearchKeyDown(WPARAM key) {
        switch (key) {
        case VK_UP:
            ScrollHoverPopupBy(-1);
            return true;
        case VK_DOWN:
            ScrollHoverPopupBy(1);
            return true;
        case VK_PRIOR:
            ScrollHoverPopupBy(-GetHoverPopupVisibleRowCount());
            return true;
        case VK_NEXT:
            ScrollHoverPopupBy(GetHoverPopupVisibleRowCount());
            return true;
        case VK_HOME:
            SetHoverPopupScrollOffset(0);
            return true;
        case VK_END:
            SetHoverPopupScrollOffset(GetHoverPopupMaxScrollOffset());
            return true;
        case VK_ESCAPE:
            if (!hover_popup_search_text_.empty()) {
                SetHoverPopupSearchText(L"");
            } else {
                SetHoverPopupSearchActive(false);
            }
            return true;
        default:
            return false;
        }
    }

    bool HandleHoverPopupSearchChar(WPARAM key) {
        if (key == VK_BACK) {
            RemoveHoverPopupSearchCharacter();
            return true;
        }

        if (key < 32 || key == 127) {
            return false;
        }

        SetHoverPopupSearchActive(true);
        AppendHoverPopupSearchCharacter(static_cast<wchar_t>(key));
        return true;
    }

    void ToggleHoverPopupSort(HoverPopupSortMode mode) {
        if (mode == HoverPopupSortMode::kDefault) {
            return;
        }

        hover_popup_sort_mode_ =
            (hover_popup_sort_mode_ == mode) ? HoverPopupSortMode::kDefault : mode;
        hover_popup_scroll_offset_ = 0;
        RefreshHoverPopupView();
    }

    void HandleHoverPopupClick(const POINT& client_point) {
        SetFocus(hover_popup_window_);
        if (IsHoverPopupSearchHit(client_point)) {
            SetHoverPopupSearchActive(true);
            return;
        }

        SetHoverPopupSearchActive(false);
        ToggleHoverPopupSort(HitTestHoverPopupSortMode(client_point));
    }

    void HandleHoverPopupVScroll(WPARAM scroll_code, int thumb_position) {
        switch (scroll_code) {
        case SB_LINEUP:
            ScrollHoverPopupBy(-1);
            break;
        case SB_LINEDOWN:
            ScrollHoverPopupBy(1);
            break;
        case SB_PAGEUP:
            ScrollHoverPopupBy(-GetHoverPopupVisibleRowCount());
            break;
        case SB_PAGEDOWN:
            ScrollHoverPopupBy(GetHoverPopupVisibleRowCount());
            break;
        case SB_TOP:
            SetHoverPopupScrollOffset(0);
            break;
        case SB_BOTTOM:
            SetHoverPopupScrollOffset(GetHoverPopupMaxScrollOffset());
            break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK:
            SetHoverPopupScrollOffset(thumb_position);
            break;
        default:
            break;
        }
    }

    void HandleHoverPopupMouseWheel(short delta) {
        const int notches = static_cast<int>(delta / WHEEL_DELTA);
        if (notches != 0) {
            ScrollHoverPopupBy(-notches * kHoverPopupWheelRows);
        }
    }

    void UpdateHoverPopupSize() {
        const int padding = ScaleByDpi(current_dpi_, 16);
        const int title_gap = ScaleByDpi(current_dpi_, 8);
        const int summary_gap = ScaleByDpi(current_dpi_, 4);
        const int section_gap = ScaleByDpi(current_dpi_, 10);
        const int search_height = GetHoverPopupSearchHeight();
        const int header_height = popup_text_line_height_ + ScaleByDpi(current_dpi_, 8);
        const int row_gap = ScaleByDpi(current_dpi_, 6);
        const int row_height = GetHoverPopupRowHeight();
        const int footer_height =
            popup_text_line_height_ * 2 + ScaleByDpi(current_dpi_, 10);
        const int row_count = GetHoverPopupVisibleRowCount();

        hover_popup_size_.cx = ScaleByDpi(current_dpi_, 980);
        hover_popup_size_.cy = padding + popup_title_line_height_ + title_gap +
                               (popup_text_line_height_ + summary_gap) * 3 + section_gap +
                               search_height + section_gap + header_height + row_gap +
                               row_count * row_height + section_gap + footer_height + padding;
    }

    void DrawHoverPopupContents(HDC dc, const RECT& client_rect) {
        const bool light_theme = IsLightTaskbarTheme();
        const HoverPopupPalette palette = GetHoverPopupPalette(light_theme);

        HBRUSH background_brush = CreateSolidBrush(palette.background);
        FillRect(dc, &client_rect, background_brush);
        DeleteObject(background_brush);

        HBRUSH border_brush = CreateSolidBrush(palette.border);
        FrameRect(dc, &client_rect, border_brush);
        DeleteObject(border_brush);

        RECT accent_rect{client_rect.left, client_rect.top, client_rect.right,
                         client_rect.top + ScaleByDpi(current_dpi_, 3)};
        HBRUSH accent_brush = CreateSolidBrush(palette.accent);
        FillRect(dc, &accent_rect, accent_brush);
        DeleteObject(accent_brush);

        const int padding_left = ScaleByDpi(current_dpi_, 16);
        const int padding_right = ScaleByDpi(current_dpi_, 30);
        const int gap = ScaleByDpi(current_dpi_, 8);
        const int line_gap = ScaleByDpi(current_dpi_, 4);
        const int row_gap = ScaleByDpi(current_dpi_, 6);
        const int scroll_bar_width = GetSystemMetrics(SM_CXVSCROLL);
        const int content_left = client_rect.left + padding_left;
        const int content_right = client_rect.right - padding_right - scroll_bar_width;

        SetBkMode(dc, TRANSPARENT);

        HFONT title_font =
            popup_title_font_ != nullptr ? popup_title_font_
                                         : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HFONT body_font =
            popup_font_ != nullptr ? popup_font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ old_font = SelectObject(dc, title_font);

        RECT title_rect{content_left,
                        client_rect.top + padding_left,
                        content_right,
                        client_rect.top + padding_left + popup_title_line_height_};
        SetTextColor(dc, palette.title_text);
        DrawTextW(dc,
                  L"Performance Snapshot",
                  -1,
                  &title_rect,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

        RECT subtitle_rect{content_left,
                           client_rect.top + padding_left,
                           content_right,
                           client_rect.top + padding_left + popup_title_line_height_};
        SetTextColor(dc, palette.secondary_text);
        DrawTextW(dc,
                  GetHoverPopupSubtitle().c_str(),
                  -1,
                  &subtitle_rect,
                  DT_SINGLELINE | DT_RIGHT | DT_NOPREFIX | DT_END_ELLIPSIS);

        SelectObject(dc, body_font);

        int content_top = title_rect.bottom + gap;
        RECT line_rect{content_left, content_top, content_right,
                       content_top + popup_text_line_height_};

        auto draw_inline_segment = [&](RECT& rect, const std::wstring& text, COLORREF color) {
            if (text.empty()) {
                return;
            }
            SetTextColor(dc, color);
            RECT draw_rect = rect;
            DrawTextW(dc,
                      text.c_str(),
                      -1,
                      &draw_rect,
                      DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
            rect.left += MeasureText(dc, text).cx;
        };

        RECT summary_line1_rect = line_rect;
        const int total_process_count =
            std::max(hover_popup_base_snapshot_.total_process_count,
                     hover_popup_snapshot_.total_process_count);
        const std::wstring proc_summary =
            hover_popup_search_text_.empty()
                ? std::to_wstring(std::max(total_process_count, 0))
                : (std::to_wstring(hover_popup_snapshot_.top_processes.size()) + L"/" +
                   std::to_wstring(std::max(total_process_count, 0)));
        const std::wstring gpu_summary =
            last_snapshot_.gpu_percent >= 0 ? FormatPercentValue(last_snapshot_.gpu_percent) : L"--";
        draw_inline_segment(summary_line1_rect, L"CPU ", palette.primary_text);
        draw_inline_segment(summary_line1_rect,
                            FormatPercentValue(std::max(last_snapshot_.cpu_percent, 0)),
                            ResolveAlertColor(palette,
                                              GetUsageAlertLevel(last_snapshot_.cpu_percent, 80.0, 95.0),
                                              palette.primary_text));
        draw_inline_segment(summary_line1_rect, L"   MEM ", palette.primary_text);
        draw_inline_segment(summary_line1_rect,
                            FormatPercentValue(std::max(last_snapshot_.memory_percent, 0)),
                            ResolveAlertColor(palette,
                                              GetUsageAlertLevel(last_snapshot_.memory_percent,
                                                                 85.0,
                                                                 95.0),
                                              palette.primary_text));
        draw_inline_segment(summary_line1_rect, L"   GPU ", palette.primary_text);
        draw_inline_segment(summary_line1_rect,
                            gpu_summary,
                            ResolveAlertColor(palette,
                                              GetUsageAlertLevel(last_snapshot_.gpu_percent, 90.0, 98.0),
                                              palette.primary_text));
        draw_inline_segment(summary_line1_rect, L"   PROC ", palette.primary_text);
        draw_inline_segment(summary_line1_rect, proc_summary, palette.secondary_text);

        const std::wstring summary_line2 =
            L"NET \u2191 " +
            FormatNetworkRate(last_snapshot_.upload_bytes_per_second,
                              app_config_.network_display_unit) +
            L"   \u2193 " +
            FormatNetworkRate(last_snapshot_.download_bytes_per_second,
                              app_config_.network_display_unit) +
            L"   DISK R " +
            FormatRate(last_snapshot_.disk_read_bytes_per_second) + L"   W " +
            FormatRate(last_snapshot_.disk_write_bytes_per_second);
        const std::wstring summary_line3 =
            L"Uptime " + FormatUptime(hover_popup_snapshot_.uptime_ms) +
            L"   MEM = RSS / USS / VMS(commit)   VRAM = dedicated GPU memory";

        OffsetRect(&line_rect, 0, popup_text_line_height_ + line_gap);
        SetTextColor(dc, palette.primary_text);
        DrawTextW(dc,
                  summary_line2.c_str(),
                  -1,
                  &line_rect,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
        OffsetRect(&line_rect, 0, popup_text_line_height_ + line_gap);
        SetTextColor(dc, palette.secondary_text);
        DrawTextW(dc,
                  summary_line3.c_str(),
                  -1,
                  &line_rect,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

        const HoverPopupTableLayout table_layout = ComputeHoverPopupTableLayout(client_rect);
        HBRUSH search_fill_brush = CreateSolidBrush(palette.search_fill);
        FillRect(dc, &table_layout.search_rect, search_fill_brush);
        DeleteObject(search_fill_brush);

        HBRUSH search_border_brush = CreateSolidBrush(hover_popup_search_active_
                                                          ? palette.search_active_border
                                                          : palette.search_border);
        FrameRect(dc, &table_layout.search_rect, search_border_brush);
        DeleteObject(search_border_brush);

        RECT search_text_rect = table_layout.search_rect;
        InflateRect(&search_text_rect, -ScaleByDpi(current_dpi_, 8), 0);
        std::wstring search_text;
        COLORREF search_text_color = palette.primary_text;
        if (!hover_popup_search_text_.empty()) {
            search_text = hover_popup_search_text_;
            if (hover_popup_search_active_) {
                search_text += L" |";
            }
        } else if (hover_popup_search_active_) {
            search_text = L"|";
        } else {
            search_text = L"Search processes...  (type to filter)";
            search_text_color = palette.search_placeholder;
        }
        SetTextColor(dc, search_text_color);
        DrawTextW(dc,
                  search_text.c_str(),
                  -1,
                  &search_text_rect,
                  DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

        const RECT& header_rect = table_layout.header_rect;
        HBRUSH header_brush = CreateSolidBrush(palette.header_fill);
        FillRect(dc, &header_rect, header_brush);
        DeleteObject(header_brush);

        auto draw_header = [&](const std::wstring& text,
                               const RECT& rect,
                               UINT format,
                               HoverPopupSortMode mode = HoverPopupSortMode::kDefault) {
            SetTextColor(dc,
                         hover_popup_sort_mode_ == mode && mode != HoverPopupSortMode::kDefault
                             ? palette.title_text
                             : palette.secondary_text);
            RECT draw_rect = rect;
            DrawTextW(dc,
                      text.c_str(),
                      -1,
                      &draw_rect,
                      format | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        };

        draw_header(L"#", table_layout.rank_rect, DT_LEFT);
        draw_header(L"Process", table_layout.name_rect, DT_LEFT);
        draw_header(GetHoverPopupHeaderText(L"CPU", HoverPopupSortMode::kCpu),
                    table_layout.cpu_rect,
                    DT_RIGHT,
                    HoverPopupSortMode::kCpu);
        draw_header(GetHoverPopupHeaderText(L"MEM (R/U/V)", HoverPopupSortMode::kMemory),
                    table_layout.mem_rect,
                    DT_RIGHT,
                    HoverPopupSortMode::kMemory);
        draw_header(GetHoverPopupHeaderText(L"GPU", HoverPopupSortMode::kGpu),
                    table_layout.gpu_rect,
                    DT_RIGHT,
                    HoverPopupSortMode::kGpu);
        draw_header(GetHoverPopupHeaderText(L"VRAM", HoverPopupSortMode::kVram),
                    table_layout.vram_rect,
                    DT_RIGHT,
                    HoverPopupSortMode::kVram);
        draw_header(GetHoverPopupHeaderText(L"IO", HoverPopupSortMode::kIo),
                    table_layout.io_rect,
                    DT_RIGHT,
                    HoverPopupSortMode::kIo);
        draw_header(GetHoverPopupHeaderText(hover_popup_snapshot_.network_is_estimated ? L"NET*" : L"NET",
                                            HoverPopupSortMode::kNetwork),
                    table_layout.net_rect,
                    DT_RIGHT,
                    HoverPopupSortMode::kNetwork);

        const int row_height = GetHoverPopupRowHeight();
        int row_top = table_layout.list_rect.top;
        const int start_index =
            std::min<int>(hover_popup_scroll_offset_,
                          std::max<int>(0,
                                        static_cast<int>(hover_popup_snapshot_.top_processes.size()) - 1));
        const int end_index =
            std::min<int>(start_index + GetHoverPopupVisibleRowCount(),
                          static_cast<int>(hover_popup_snapshot_.top_processes.size()));
        if (hover_popup_snapshot_.top_processes.empty()) {
            RECT empty_rect{content_left,
                            row_top,
                            content_right,
                            row_top + row_height};
            SetTextColor(dc, palette.secondary_text);
            DrawTextW(dc,
                      hover_popup_base_snapshot_.total_process_count > 0
                          ? L"No matching processes. Press Esc to clear search."
                          : L"Process data is warming up. Keep the popup open for a second.",
                      -1,
                      &empty_rect,
                      DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        } else {
            for (int index = start_index; index < end_index; ++index) {
                const ProcessPopupItem& item = hover_popup_snapshot_.top_processes[index];
                RECT row_rect{content_left, row_top, content_right,
                              row_top + row_height};

                if ((index % 2) == 0) {
                    HBRUSH row_brush = CreateSolidBrush(palette.row_highlight);
                    FillRect(dc, &row_rect, row_brush);
                    DeleteObject(row_brush);
                }
                if (index < 3) {
                    RECT marker_rect{row_rect.left, row_rect.top, row_rect.left + ScaleByDpi(current_dpi_, 3),
                                     row_rect.bottom};
                    HBRUSH marker_brush = CreateSolidBrush(palette.accent);
                    FillRect(dc, &marker_rect, marker_brush);
                    DeleteObject(marker_brush);
                }

                RECT current_rank_rect{table_layout.rank_rect.left,
                                       row_rect.top,
                                       table_layout.rank_rect.right,
                                       row_rect.bottom};
                RECT current_name_rect{table_layout.name_rect.left,
                                       row_rect.top,
                                       table_layout.name_rect.right,
                                       row_rect.bottom};
                RECT current_cpu_rect{table_layout.cpu_rect.left,
                                      row_rect.top,
                                      table_layout.cpu_rect.right,
                                      row_rect.bottom};
                RECT current_mem_rect{table_layout.mem_rect.left,
                                      row_rect.top,
                                      table_layout.mem_rect.right,
                                      row_rect.bottom};
                RECT current_gpu_rect{table_layout.gpu_rect.left,
                                      row_rect.top,
                                      table_layout.gpu_rect.right,
                                      row_rect.bottom};
                RECT current_vram_rect{table_layout.vram_rect.left,
                                       row_rect.top,
                                       table_layout.vram_rect.right,
                                       row_rect.bottom};
                RECT current_io_rect{table_layout.io_rect.left,
                                     row_rect.top,
                                     table_layout.io_rect.right,
                                     row_rect.bottom};
                RECT current_net_rect{table_layout.net_rect.left,
                                      row_rect.top,
                                      table_layout.net_rect.right,
                                      row_rect.bottom};

                const std::wstring rank_text = std::to_wstring(index + 1);
                const std::wstring cpu_text = FormatPercentValue(item.cpu_percent);
                const std::wstring mem_text =
                    FormatBytes(item.rss_bytes) + L" / " + FormatBytes(item.uss_bytes) + L" / " +
                    FormatBytes(item.vms_bytes);
                const std::wstring gpu_text =
                    last_snapshot_.gpu_percent >= 0 ? FormatPercentValue(item.gpu_percent) : L"--";
                const std::wstring vram_text = FormatBytes(item.vram_bytes);
                const std::wstring io_text =
                    FormatRate(item.io_read_bytes_per_second + item.io_write_bytes_per_second);
                const std::wstring net_text = hover_popup_snapshot_.network_metric_available
                                                  ? FormatNetworkRate(
                                                        item.network_bytes_per_second,
                                                        app_config_.network_display_unit)
                                                  : L"--";
                const COLORREF cpu_color =
                    ResolveAlertColor(palette,
                                      GetUsageAlertLevel(item.cpu_percent, 80.0, 95.0),
                                      palette.primary_text);
                const COLORREF gpu_color =
                    last_snapshot_.gpu_percent >= 0
                        ? ResolveAlertColor(palette,
                                            GetUsageAlertLevel(item.gpu_percent, 90.0, 98.0),
                                            palette.primary_text)
                        : palette.secondary_text;

                SetTextColor(dc, palette.secondary_text);
                DrawTextW(dc,
                          rank_text.c_str(),
                          -1,
                          &current_rank_rect,
                          DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                SetTextColor(dc, palette.primary_text);
                DrawTextW(dc,
                          item.name.c_str(),
                          -1,
                          &current_name_rect,
                          DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                SetTextColor(dc, cpu_color);
                DrawTextW(dc,
                          cpu_text.c_str(),
                          -1,
                          &current_cpu_rect,
                          DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                SetTextColor(dc, palette.primary_text);
                DrawTextW(dc,
                          mem_text.c_str(),
                          -1,
                          &current_mem_rect,
                          DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                SetTextColor(dc, gpu_color);
                DrawTextW(dc,
                          gpu_text.c_str(),
                          -1,
                          &current_gpu_rect,
                          DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                SetTextColor(dc, palette.primary_text);
                DrawTextW(dc,
                          vram_text.c_str(),
                          -1,
                          &current_vram_rect,
                          DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                DrawTextW(dc,
                          io_text.c_str(),
                          -1,
                          &current_io_rect,
                          DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                DrawTextW(dc,
                          net_text.c_str(),
                          -1,
                          &current_net_rect,
                          DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

                row_top += row_height;
            }
        }

        RECT footer_rect1{content_left,
                          table_layout.list_rect.bottom + gap,
                          content_right,
                          table_layout.list_rect.bottom + gap + popup_text_line_height_};
        RECT footer_rect2{content_left,
                          footer_rect1.bottom + line_gap,
                          content_right,
                          footer_rect1.bottom + line_gap + popup_text_line_height_};
        SetTextColor(dc, palette.secondary_text);
        DrawTextW(dc,
                  L"* MEM shows RSS / USS / VMS(commit, psutil/taskmgr-style). PSS is not exposed directly yet.",
                  -1,
                  &footer_rect1,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
        DrawTextW(dc,
                  GetNetworkUnitFooterText(app_config_.network_display_unit),
                  -1,
                  &footer_rect2,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

        SelectObject(dc, old_font);
    }

    void PaintHoverPopup() {
        PAINTSTRUCT paint_struct{};
        HDC dc = BeginPaint(hover_popup_window_, &paint_struct);

        RECT client_rect{};
        GetClientRect(hover_popup_window_, &client_rect);
        DrawHoverPopupContents(dc, client_rect);
        EndPaint(hover_popup_window_, &paint_struct);
    }

    void RequestHoverPopupRedraw() {
        if (hover_popup_window_ == nullptr || !IsWindow(hover_popup_window_) || !hover_popup_visible_) {
            return;
        }

        RedrawWindow(hover_popup_window_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }

    bool IsCursorOverWidgetOrPopup() const {
        POINT cursor_pos{};
        GetCursorPos(&cursor_pos);
        return IsPointInWindowRect(widget_window_, cursor_pos) ||
               (hover_popup_visible_ && IsPointInWindowRect(hover_popup_window_, cursor_pos));
    }

    void RefreshHoverPopupData() {
        hover_popup_base_snapshot_ = process_monitor_.Sample(last_snapshot_, 0);
        ApplyHoverPopupSort();
        UpdateHoverPopupSize();
        UpdateHoverPopupScrollBar();
    }

    void PositionHoverPopup() {
        if (hover_popup_window_ == nullptr || !IsWindow(hover_popup_window_) || widget_window_ == nullptr ||
            !IsWindow(widget_window_)) {
            return;
        }

        RECT widget_rect{};
        if (!GetWindowRect(widget_window_, &widget_rect)) {
            return;
        }

        HMONITOR monitor_handle = MonitorFromRect(&widget_rect, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitor_info{};
        monitor_info.cbSize = sizeof(monitor_info);
        if (!GetMonitorInfoW(monitor_handle, &monitor_info)) {
            return;
        }

        const RECT bounds = monitor_info.rcMonitor;
        const int gap = ScaleByDpi(current_dpi_, 8);

        const int min_x = static_cast<int>(bounds.left) + gap;
        const int max_x =
            std::max<int>(min_x, static_cast<int>(bounds.right) - hover_popup_size_.cx - gap);
        const int min_y = static_cast<int>(bounds.top) + gap;
        const int max_y =
            std::max<int>(min_y, static_cast<int>(bounds.bottom) - hover_popup_size_.cy - gap);

        const int space_above = static_cast<int>(widget_rect.top - bounds.top);
        const int space_below = static_cast<int>(bounds.bottom - widget_rect.bottom);
        const int space_left = static_cast<int>(widget_rect.left - bounds.left);
        const int space_right = static_cast<int>(bounds.right - widget_rect.right);

        int x = std::clamp<int>(static_cast<int>(widget_rect.right) - hover_popup_size_.cx, min_x, max_x);
        int y = 0;

        const bool prefer_vertical =
            std::max(space_above, space_below) >= std::max(space_left, space_right);
        if (prefer_vertical) {
            if (space_above >= hover_popup_size_.cy + gap || space_above >= space_below) {
                y = widget_rect.top - hover_popup_size_.cy - gap;
            } else {
                y = widget_rect.bottom + gap;
            }
            y = std::clamp<int>(y, min_y, max_y);
        } else {
            if (space_left >= hover_popup_size_.cx + gap || space_left >= space_right) {
                x = static_cast<int>(widget_rect.left) - hover_popup_size_.cx - gap;
            } else {
                x = static_cast<int>(widget_rect.right) + gap;
            }
            x = std::clamp<int>(x, min_x, max_x);
            y = std::clamp<int>(static_cast<int>(widget_rect.bottom) - hover_popup_size_.cy,
                                min_y,
                                max_y);
        }

        SetWindowPos(hover_popup_window_,
                     HWND_TOPMOST,
                     x,
                     y,
                     hover_popup_size_.cx,
                     hover_popup_size_.cy,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void ShowHoverPopup(bool activate) {
        if (!EnsureHoverPopupWindow()) {
            return;
        }

        KillTimer(controller_window_, kHoverHideTimerId);
        RefreshHoverPopupData();
        PositionHoverPopup();
        ShowWindow(hover_popup_window_, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
        hover_popup_visible_ = true;
        UpdateHoverPopupScrollBar();
        RequestHoverPopupRedraw();
        if (activate) {
            SetForegroundWindow(hover_popup_window_);
            SetActiveWindow(hover_popup_window_);
            SetFocus(hover_popup_window_);
        }
    }

    void HideHoverPopup() {
        KillTimer(controller_window_, kHoverHideTimerId);
        hover_popup_visible_ = false;
        hover_popup_search_active_ = false;
        if (hover_popup_window_ != nullptr && IsWindow(hover_popup_window_)) {
            ShowWindow(hover_popup_window_, SW_HIDE);
        }
    }

    void ArmHoverHideTimer() {
        if (app_config_.popup_activation_mode != PopupActivationMode::kHover) {
            return;
        }
        if (controller_window_ == nullptr || !IsWindow(controller_window_)) {
            return;
        }
        SetTimer(controller_window_, kHoverHideTimerId, kHoverHideDelayMs, nullptr);
    }

    void HandleHoverMove(HWND source_window) {
        if (app_config_.popup_activation_mode != PopupActivationMode::kHover) {
            return;
        }
        TRACKMOUSEEVENT track_event{};
        track_event.cbSize = sizeof(track_event);
        track_event.dwFlags = TME_LEAVE;
        track_event.hwndTrack = source_window;
        TrackMouseEvent(&track_event);

        KillTimer(controller_window_, kHoverHideTimerId);
        if (!hover_popup_visible_) {
            ShowHoverPopup(false);
        }
    }

    bool RenderLayeredWidget() {
        if (widget_window_ == nullptr || !IsWindow(widget_window_)) {
            return false;
        }

        RECT window_rect{};
        if (!GetWindowRect(widget_window_, &window_rect)) {
            return false;
        }

        const int width = window_rect.right - window_rect.left;
        const int height = window_rect.bottom - window_rect.top;
        if (width <= 0 || height <= 0) {
            return false;
        }

        HDC screen_dc = GetDC(nullptr);
        HDC memory_dc = CreateCompatibleDC(screen_dc);

        BITMAPINFO bitmap_info{};
        bitmap_info.bmiHeader.biSize = sizeof(bitmap_info.bmiHeader);
        bitmap_info.bmiHeader.biWidth = width;
        bitmap_info.bmiHeader.biHeight = -height;
        bitmap_info.bmiHeader.biPlanes = 1;
        bitmap_info.bmiHeader.biBitCount = 32;
        bitmap_info.bmiHeader.biCompression = BI_RGB;

        void* bitmap_bits = nullptr;
        HBITMAP bitmap =
            CreateDIBSection(screen_dc, &bitmap_info, DIB_RGB_COLORS, &bitmap_bits, nullptr, 0);
        if (bitmap == nullptr || bitmap_bits == nullptr) {
            if (bitmap != nullptr) {
                DeleteObject(bitmap);
            }
            DeleteDC(memory_dc);
            ReleaseDC(nullptr, screen_dc);
            return false;
        }

        HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);
        RECT client_rect{0, 0, width, height};
        DrawWidgetContents(memory_dc, client_rect);

        auto* pixels = static_cast<BgraPixel*>(bitmap_bits);
        const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
        for (size_t i = 0; i < pixel_count; ++i) {
            pixels[i].alpha = 255;
        }

        POINT source_point{0, 0};
        POINT destination_point{window_rect.left, window_rect.top};
        HWND parent_window = GetParent(widget_window_);
        if (parent_window != nullptr) {
            ScreenToClient(parent_window, &destination_point);
        }
        SIZE window_size{width, height};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        const BOOL update_ok =
            UpdateLayeredWindow(widget_window_, screen_dc, &destination_point, &window_size,
                                memory_dc, &source_point, 0, &blend, ULW_ALPHA);

        SelectObject(memory_dc, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        return update_ok != FALSE;
    }

    void RequestWidgetRedraw() {
        if (widget_window_ == nullptr || !IsWindow(widget_window_)) {
            return;
        }

        if (RenderLayeredWidget()) {
            return;
        }

        RedrawWindow(widget_window_,
                     nullptr,
                     nullptr,
                     RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }

    void SampleAndRefresh() {
        last_snapshot_ = metrics_.Sample();
        UpdateDisplayLines(last_snapshot_);
        RequestWidgetRedraw();
        if (hover_popup_visible_) {
            RefreshHoverPopupData();
            PositionHoverPopup();
            RequestHoverPopupRedraw();
        }
    }

    bool SaveConfig() const {
        return SaveAppConfig(app_config_);
    }

    void ApplyMetricVisibilityChange() {
        UpdateDisplayLines(last_snapshot_);
        RefreshFontAndSize();
        if (embedder_.IsAttached()) {
            if (!embedder_.RefreshLayout(widget_window_, widget_size_)) {
                ShowWindow(widget_window_, SW_HIDE);
                SetTimer(controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
                HideHoverPopup();
                return;
            }
        }
        RequestWidgetRedraw();
        if (hover_popup_visible_) {
            PositionHoverPopup();
            RequestHoverPopupRedraw();
        }
    }

    bool ToggleMetricVisibility(UINT command_id) {
        bool* target = nullptr;
        switch (command_id) {
        case kMetricCpuCommandId:
            target = &app_config_.visible_metrics.show_cpu;
            break;
        case kMetricMemoryCommandId:
            target = &app_config_.visible_metrics.show_memory;
            break;
        case kMetricUploadCommandId:
            target = &app_config_.visible_metrics.show_upload;
            break;
        case kMetricDownloadCommandId:
            target = &app_config_.visible_metrics.show_download;
            break;
        case kMetricGpuCommandId:
            target = &app_config_.visible_metrics.show_gpu;
            break;
        case kMetricDiskReadCommandId:
            target = &app_config_.visible_metrics.show_disk_read;
            break;
        case kMetricDiskWriteCommandId:
            target = &app_config_.visible_metrics.show_disk_write;
            break;
        default:
            return false;
        }

        if (*target && CountVisibleMetrics(app_config_.visible_metrics) <= 1) {
            MessageBoxW(controller_window_,
                        L"Keep at least one metric visible.",
                        L"Minimal Taskbar Monitor",
                        MB_OK | MB_ICONINFORMATION);
            return true;
        }

        *target = !*target;
        if (!SaveConfig()) {
            *target = !*target;
            MessageBoxW(controller_window_,
                        L"Unable to save the local config file.",
                        L"Minimal Taskbar Monitor",
                        MB_OK | MB_ICONERROR);
            return true;
        }

        ApplyMetricVisibilityChange();
        return true;
    }

    bool SetNetworkDisplayUnit(NetworkDisplayUnit network_display_unit) {
        if (app_config_.network_display_unit == network_display_unit) {
            return true;
        }

        const NetworkDisplayUnit previous_unit = app_config_.network_display_unit;
        app_config_.network_display_unit = network_display_unit;
        if (!SaveConfig()) {
            app_config_.network_display_unit = previous_unit;
            MessageBoxW(controller_window_,
                        L"Unable to save the local config file.",
                        L"Minimal Taskbar Monitor",
                        MB_OK | MB_ICONERROR);
            return true;
        }

        ApplyMetricVisibilityChange();
        return true;
    }

    bool SetPopupActivationMode(PopupActivationMode popup_activation_mode) {
        if (app_config_.popup_activation_mode == popup_activation_mode) {
            return true;
        }

        const PopupActivationMode previous_mode = app_config_.popup_activation_mode;
        app_config_.popup_activation_mode = popup_activation_mode;
        if (!SaveConfig()) {
            app_config_.popup_activation_mode = previous_mode;
            MessageBoxW(controller_window_,
                        L"Unable to save the local config file.",
                        L"Minimal Taskbar Monitor",
                        MB_OK | MB_ICONERROR);
            return true;
        }

        HideHoverPopup();
        return true;
    }

    bool SetSampleIntervalSeconds(unsigned int sample_interval_seconds) {
        if (!IsSupportedSampleIntervalSeconds(sample_interval_seconds)) {
            sample_interval_seconds = 1;
        }
        if (app_config_.sample_interval_seconds == sample_interval_seconds) {
            return true;
        }

        const unsigned int previous_interval = app_config_.sample_interval_seconds;
        app_config_.sample_interval_seconds = sample_interval_seconds;
        if (!SaveConfig()) {
            app_config_.sample_interval_seconds = previous_interval;
            MessageBoxW(controller_window_,
                        L"Unable to save the local config file.",
                        L"Minimal Taskbar Monitor",
                        MB_OK | MB_ICONERROR);
            return true;
        }

        SetTimer(controller_window_,
                 kSampleTimerId,
                 GetSampleTimerIntervalMs(app_config_.sample_interval_seconds),
                 nullptr);
        SampleAndRefresh();
        return true;
    }

    void HandleWidgetLeftButtonDown() {
        click_popup_started_from_widget_ =
            app_config_.popup_activation_mode == PopupActivationMode::kClick && hover_popup_visible_;
    }

    void HandleWidgetLeftButtonUp() {
        if (app_config_.popup_activation_mode != PopupActivationMode::kClick) {
            click_popup_started_from_widget_ = false;
            return;
        }

        if (click_popup_started_from_widget_) {
            click_popup_started_from_widget_ = false;
            return;
        }

        if (hover_popup_visible_) {
            HideHoverPopup();
            return;
        }

        ShowHoverPopup(true);
    }

    void UpdateLayout() {
        if (!EnsureWidgetWindow()) {
            return;
        }

        if (!embedder_.IsAttached()) {
            ReattachWidget();
            return;
        }

        const UINT previous_dpi = current_dpi_;
        RefreshFontAndSize();
        if (current_dpi_ != previous_dpi) {
            RequestWidgetRedraw();
            if (hover_popup_visible_) {
                PositionHoverPopup();
                RequestHoverPopupRedraw();
            }
        }
        if (!embedder_.RefreshLayout(widget_window_, widget_size_)) {
            ShowWindow(widget_window_, SW_HIDE);
            SetTimer(controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
            HideHoverPopup();
            return;
        }

        if (hover_popup_visible_) {
            PositionHoverPopup();
        }
    }

    void PaintWidget() {
        PAINTSTRUCT paint_struct{};
        HDC dc = BeginPaint(widget_window_, &paint_struct);

        RECT client_rect{};
        GetClientRect(widget_window_, &client_rect);
        DrawWidgetContents(dc, client_rect);
        EndPaint(widget_window_, &paint_struct);
    }

    void ToggleAutoStart() {
        const bool enabled = IsAutoStartEnabled();
        if (!SetAutoStartEnabled(!enabled)) {
            MessageBoxW(controller_window_,
                        L"Unable to update the startup setting.",
                        L"Minimal Taskbar Monitor",
                        MB_OK | MB_ICONERROR);
        }
    }

    void ShowContextMenu(POINT screen_point) {
        HideHoverPopup();
        HMENU menu = CreatePopupMenu();
        HMENU metrics_menu = CreatePopupMenu();
        HMENU network_units_menu = CreatePopupMenu();
        HMENU popup_mode_menu = CreatePopupMenu();
        HMENU refresh_interval_menu = CreatePopupMenu();
        AppendMenuW(metrics_menu,
                    MF_STRING | (app_config_.visible_metrics.show_cpu ? MF_CHECKED : MF_UNCHECKED),
                    kMetricCpuCommandId,
                    L"CPU Usage");
        AppendMenuW(metrics_menu,
                    MF_STRING |
                        (app_config_.visible_metrics.show_memory ? MF_CHECKED : MF_UNCHECKED),
                    kMetricMemoryCommandId,
                    L"Memory");
        AppendMenuW(metrics_menu,
                    MF_STRING |
                        (app_config_.visible_metrics.show_upload ? MF_CHECKED : MF_UNCHECKED),
                    kMetricUploadCommandId,
                    L"Upload Speed");
        AppendMenuW(metrics_menu,
                    MF_STRING |
                        (app_config_.visible_metrics.show_download ? MF_CHECKED : MF_UNCHECKED),
                    kMetricDownloadCommandId,
                    L"Download Speed");
        AppendMenuW(metrics_menu,
                    MF_STRING | (app_config_.visible_metrics.show_gpu ? MF_CHECKED : MF_UNCHECKED),
                    kMetricGpuCommandId,
                    L"GPU Usage");
        AppendMenuW(metrics_menu,
                    MF_STRING |
                        (app_config_.visible_metrics.show_disk_read ? MF_CHECKED : MF_UNCHECKED),
                    kMetricDiskReadCommandId,
                    L"Disk Read");
        AppendMenuW(metrics_menu,
                    MF_STRING |
                        (app_config_.visible_metrics.show_disk_write ? MF_CHECKED : MF_UNCHECKED),
                    kMetricDiskWriteCommandId,
                    L"Disk Write");
        AppendMenuW(network_units_menu,
                    MF_STRING |
                        (app_config_.network_display_unit == NetworkDisplayUnit::kBitsPerSecond
                             ? MF_CHECKED
                             : MF_UNCHECKED),
                    kNetworkUnitsBitsCommandId,
                    L"Bits/sec (Task Manager style)");
        AppendMenuW(network_units_menu,
                    MF_STRING |
                        (app_config_.network_display_unit == NetworkDisplayUnit::kBytesPerSecond
                             ? MF_CHECKED
                             : MF_UNCHECKED),
                    kNetworkUnitsBytesCommandId,
                    L"Bytes/sec (KB/s, MB/s)");
        AppendMenuW(popup_mode_menu,
                    MF_STRING |
                        (app_config_.popup_activation_mode == PopupActivationMode::kHover
                             ? MF_CHECKED
                             : MF_UNCHECKED),
                    kPopupModeHoverCommandId,
                    L"Hover popup");
        AppendMenuW(popup_mode_menu,
                    MF_STRING |
                        (app_config_.popup_activation_mode == PopupActivationMode::kClick
                             ? MF_CHECKED
                             : MF_UNCHECKED),
                    kPopupModeClickCommandId,
                    L"Click popup");
        AppendMenuW(refresh_interval_menu,
                    MF_STRING |
                        (app_config_.sample_interval_seconds == 1 ? MF_CHECKED : MF_UNCHECKED),
                    kSampleInterval1sCommandId,
                    L"1 second");
        AppendMenuW(refresh_interval_menu,
                    MF_STRING |
                        (app_config_.sample_interval_seconds == 2 ? MF_CHECKED : MF_UNCHECKED),
                    kSampleInterval2sCommandId,
                    L"2 seconds");
        AppendMenuW(refresh_interval_menu,
                    MF_STRING |
                        (app_config_.sample_interval_seconds == 5 ? MF_CHECKED : MF_UNCHECKED),
                    kSampleInterval5sCommandId,
                    L"5 seconds");
        AppendMenuW(refresh_interval_menu,
                    MF_STRING |
                        (app_config_.sample_interval_seconds == 10 ? MF_CHECKED : MF_UNCHECKED),
                    kSampleInterval10sCommandId,
                    L"10 seconds");

        AppendMenuW(menu,
                    MF_POPUP,
                    reinterpret_cast<UINT_PTR>(metrics_menu),
                    L"Visible Metrics");
        AppendMenuW(menu,
                    MF_POPUP,
                    reinterpret_cast<UINT_PTR>(network_units_menu),
                    L"Network Units");
        AppendMenuW(menu,
                    MF_POPUP,
                    reinterpret_cast<UINT_PTR>(popup_mode_menu),
                    L"Popup Mode");
        AppendMenuW(menu,
                    MF_POPUP,
                    reinterpret_cast<UINT_PTR>(refresh_interval_menu),
                    L"Refresh Interval");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        const UINT auto_start_flags =
            MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED);
        AppendMenuW(menu, auto_start_flags, kAutoStartCommandId, L"Launch at startup");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kExitCommandId, L"Exit");

        SetForegroundWindow(controller_window_ != nullptr ? controller_window_ : widget_window_);
        const UINT command =
            TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen_point.x, screen_point.y, 0,
                           controller_window_, nullptr);
        DestroyMenu(menu);
        PostMessageW(controller_window_, WM_NULL, 0, 0);

        if (ToggleMetricVisibility(command)) {
            return;
        }
        if (command == kNetworkUnitsBitsCommandId) {
            SetNetworkDisplayUnit(NetworkDisplayUnit::kBitsPerSecond);
            return;
        }
        if (command == kNetworkUnitsBytesCommandId) {
            SetNetworkDisplayUnit(NetworkDisplayUnit::kBytesPerSecond);
            return;
        }
        if (command == kPopupModeHoverCommandId) {
            SetPopupActivationMode(PopupActivationMode::kHover);
            return;
        }
        if (command == kPopupModeClickCommandId) {
            SetPopupActivationMode(PopupActivationMode::kClick);
            return;
        }
        if (command == kSampleInterval1sCommandId) {
            SetSampleIntervalSeconds(1);
            return;
        }
        if (command == kSampleInterval2sCommandId) {
            SetSampleIntervalSeconds(2);
            return;
        }
        if (command == kSampleInterval5sCommandId) {
            SetSampleIntervalSeconds(5);
            return;
        }
        if (command == kSampleInterval10sCommandId) {
            SetSampleIntervalSeconds(10);
            return;
        }
        if (command == kAutoStartCommandId) {
            ToggleAutoStart();
            return;
        }
        if (command == kExitCommandId) {
            PostMessageW(controller_window_, WM_CLOSE, 0, 0);
        }
    }

    bool EnsureGdiplus() {
        if (gdiplus_started_) {
            return true;
        }

        Gdiplus::GdiplusStartupInput startup_input;
        const Gdiplus::Status status =
            Gdiplus::GdiplusStartup(&gdiplus_token_, &startup_input, nullptr);
        gdiplus_started_ = status == Gdiplus::Ok;
        if (!gdiplus_started_) {
            gdiplus_token_ = 0;
        }
        return gdiplus_started_;
    }

    void ShutdownGdiplus() {
        if (!gdiplus_started_) {
            return;
        }

        Gdiplus::GdiplusShutdown(gdiplus_token_);
        gdiplus_started_ = false;
        gdiplus_token_ = 0;
    }

    std::unique_ptr<Gdiplus::Bitmap> LoadLogoBitmapFromResource() {
        if (!EnsureGdiplus()) {
            return nullptr;
        }

        HRSRC resource_info =
            FindResourceW(instance_handle_, MAKEINTRESOURCEW(IDR_APP_LOGO_PNG), RT_RCDATA);
        if (resource_info == nullptr) {
            return nullptr;
        }

        const DWORD resource_size = SizeofResource(instance_handle_, resource_info);
        if (resource_size == 0) {
            return nullptr;
        }

        HGLOBAL loaded_resource = LoadResource(instance_handle_, resource_info);
        if (loaded_resource == nullptr) {
            return nullptr;
        }

        const void* resource_data = LockResource(loaded_resource);
        if (resource_data == nullptr) {
            return nullptr;
        }

        HGLOBAL resource_copy = GlobalAlloc(GMEM_MOVEABLE, resource_size);
        if (resource_copy == nullptr) {
            return nullptr;
        }

        void* copy_data = GlobalLock(resource_copy);
        if (copy_data == nullptr) {
            GlobalFree(resource_copy);
            return nullptr;
        }

        memcpy(copy_data, resource_data, resource_size);
        GlobalUnlock(resource_copy);

        IStream* stream = nullptr;
        if (CreateStreamOnHGlobal(resource_copy, TRUE, &stream) != S_OK) {
            GlobalFree(resource_copy);
            return nullptr;
        }

        std::unique_ptr<Gdiplus::Bitmap> bitmap(Gdiplus::Bitmap::FromStream(stream, FALSE));
        stream->Release();
        if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok) {
            return nullptr;
        }

        return bitmap;
    }

    HICON CreateIconFromLogo() {
        std::unique_ptr<Gdiplus::Bitmap> source_bitmap = LoadLogoBitmapFromResource();
        if (!source_bitmap) {
            return nullptr;
        }

        Gdiplus::Rect content_bounds{};
        if (!FindContentBounds(*source_bitmap, content_bounds)) {
            content_bounds =
                Gdiplus::Rect(0, 0, static_cast<INT>(source_bitmap->GetWidth()),
                              static_cast<INT>(source_bitmap->GetHeight()));
        }

        const int icon_size = std::max({GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 32});
        const int padding = std::max(2, icon_size / 12);
        const int available_width = std::max(1, icon_size - padding * 2);
        const int available_height = std::max(1, icon_size - padding * 2);
        const double scale =
            std::min(static_cast<double>(available_width) / std::max(content_bounds.Width, 1),
                     static_cast<double>(available_height) / std::max(content_bounds.Height, 1));
        const int draw_width = std::max(1, static_cast<int>(content_bounds.Width * scale));
        const int draw_height = std::max(1, static_cast<int>(content_bounds.Height * scale));
        const int offset_x = (icon_size - draw_width) / 2;
        const int offset_y = (icon_size - draw_height) / 2;

        Gdiplus::Bitmap icon_bitmap(icon_size, icon_size, PixelFormat32bppARGB);
        Gdiplus::Graphics graphics(&icon_bitmap);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

        Gdiplus::ImageAttributes image_attributes;
        image_attributes.SetColorKey(Gdiplus::Color(245, 245, 245),
                                     Gdiplus::Color(255, 255, 255),
                                     Gdiplus::ColorAdjustTypeBitmap);

        const Gdiplus::Rect destination_rect(offset_x, offset_y, draw_width, draw_height);
        if (graphics.DrawImage(source_bitmap.get(),
                               destination_rect,
                               content_bounds.X,
                               content_bounds.Y,
                               content_bounds.Width,
                               content_bounds.Height,
                               Gdiplus::UnitPixel,
                               &image_attributes) != Gdiplus::Ok) {
            return nullptr;
        }

        HICON icon_handle = nullptr;
        if (icon_bitmap.GetHICON(&icon_handle) != Gdiplus::Ok) {
            return nullptr;
        }

        return icon_handle;
    }

    HICON LoadTrayIcon() {
        HICON icon_handle = CreateIconFromLogo();
        if (icon_handle != nullptr) {
            return icon_handle;
        }

        HICON fallback_icon = LoadIconW(nullptr, IDI_APPLICATION);
        return fallback_icon != nullptr ? CopyIcon(fallback_icon) : nullptr;
    }

    static MonitorApp* FromWindow(HWND window_handle) {
        return reinterpret_cast<MonitorApp*>(GetWindowLongPtrW(window_handle, GWLP_USERDATA));
    }

    static LRESULT CALLBACK ControllerWindowProc(HWND window_handle,
                                                 UINT message,
                                                 WPARAM w_param,
                                                 LPARAM l_param) {
        MonitorApp* app = nullptr;
        if (message == WM_NCCREATE) {
            auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(l_param);
            app = static_cast<MonitorApp*>(create_struct->lpCreateParams);
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->controller_window_ = window_handle;
        } else {
            app = FromWindow(window_handle);
        }

        if (app == nullptr) {
            return DefWindowProcW(window_handle, message, w_param, l_param);
        }

        if (message == app->taskbar_created_message_) {
            app->HideHoverPopup();
            app->embedder_.Detach(app->widget_window_);
            app->tray_icon_added_ = false;
            SetTimer(window_handle, kReattachTimerId, kReattachDelayMs, nullptr);
            return 0;
        }

        switch (message) {
        case WM_CREATE:
            return app->Initialize() ? 0 : -1;
        case WM_TIMER:
            if (w_param == kSampleTimerId) {
                app->SampleAndRefresh();
                return 0;
            }
            if (w_param == kLayoutTimerId) {
                app->UpdateLayout();
                return 0;
            }
            if (w_param == kReattachTimerId) {
                KillTimer(window_handle, kReattachTimerId);
                app->EnsureTrayIcon();
                app->ReattachWidget();
                return 0;
            }
            if (w_param == kHoverHideTimerId) {
                KillTimer(window_handle, kHoverHideTimerId);
                if (!app->IsCursorOverWidgetOrPopup()) {
                    app->HideHoverPopup();
                }
                return 0;
            }
            break;
        case kTrayIconCallbackMessage:
            if (l_param == WM_RBUTTONUP || l_param == WM_CONTEXTMENU) {
                POINT point{};
                GetCursorPos(&point);
                app->ShowContextMenu(point);
                return 0;
            }
            break;
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
            app->RefreshFontAndSize();
            if (app->widget_window_ != nullptr && IsWindow(app->widget_window_)) {
                app->RequestWidgetRedraw();
            }
            if (app->hover_popup_visible_) {
                app->PositionHoverPopup();
                app->RequestHoverPopupRedraw();
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(window_handle);
            return 0;
        case WM_DESTROY:
            app->Shutdown();
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }

        return DefWindowProcW(window_handle, message, w_param, l_param);
    }

    static LRESULT CALLBACK WidgetWindowProc(HWND window_handle,
                                             UINT message,
                                             WPARAM w_param,
                                             LPARAM l_param) {
        MonitorApp* app = nullptr;
        if (message == WM_NCCREATE) {
            auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(l_param);
            app = static_cast<MonitorApp*>(create_struct->lpCreateParams);
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->widget_window_ = window_handle;
        } else {
            app = FromWindow(window_handle);
        }

        if (app == nullptr) {
            return DefWindowProcW(window_handle, message, w_param, l_param);
        }

        switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            app->PaintWidget();
            return 0;
        case WM_MOUSEMOVE:
            app->HandleHoverMove(window_handle);
            return 0;
        case WM_MOUSELEAVE:
            app->ArmHoverHideTimer();
            return 0;
        case WM_LBUTTONDOWN:
            app->HandleWidgetLeftButtonDown();
            return 0;
        case WM_LBUTTONUP:
            app->HandleWidgetLeftButtonUp();
            return 0;
        case WM_RBUTTONUP: {
            POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            ClientToScreen(window_handle, &point);
            app->ShowContextMenu(point);
            return 0;
        }
        case WM_CONTEXTMENU: {
            POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            if (point.x == -1 && point.y == -1) {
                RECT rect{};
                GetWindowRect(window_handle, &rect);
                point.x = rect.left;
                point.y = rect.bottom;
            }
            app->ShowContextMenu(point);
            return 0;
        }
        case WM_NCDESTROY:
            if (app->widget_window_ == window_handle) {
                app->HideHoverPopup();
                app->widget_window_ = nullptr;
                if (!app->is_shutting_down_ && app->controller_window_ != nullptr &&
                    IsWindow(app->controller_window_)) {
                    SetTimer(app->controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
                }
            }
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, 0);
            break;
        default:
            break;
        }

        return DefWindowProcW(window_handle, message, w_param, l_param);
    }

    static LRESULT CALLBACK HoverPopupWindowProc(HWND window_handle,
                                                 UINT message,
                                                 WPARAM w_param,
                                                 LPARAM l_param) {
        MonitorApp* app = nullptr;
        if (message == WM_NCCREATE) {
            auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(l_param);
            app = static_cast<MonitorApp*>(create_struct->lpCreateParams);
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->hover_popup_window_ = window_handle;
        } else {
            app = FromWindow(window_handle);
        }

        if (app == nullptr) {
            return DefWindowProcW(window_handle, message, w_param, l_param);
        }

        switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            app->PaintHoverPopup();
            return 0;
        case WM_VSCROLL:
            app->HandleHoverPopupVScroll(LOWORD(w_param), HIWORD(w_param));
            return 0;
        case WM_MOUSEWHEEL:
            app->HandleHoverPopupMouseWheel(GET_WHEEL_DELTA_WPARAM(w_param));
            return 0;
        case WM_MOUSEMOVE:
            app->HandleHoverMove(window_handle);
            return 0;
        case WM_MOUSELEAVE:
            app->ArmHoverHideTimer();
            return 0;
        case WM_KEYDOWN:
            if (app->HandleHoverPopupSearchKeyDown(w_param)) {
                return 0;
            }
            break;
        case WM_CHAR:
            if (app->HandleHoverPopupSearchChar(w_param)) {
                return 0;
            }
            break;
        case WM_KILLFOCUS:
            app->SetHoverPopupSearchActive(false);
            if (app->app_config_.popup_activation_mode == PopupActivationMode::kClick) {
                app->HideHoverPopup();
            } else {
                app->ArmHoverHideTimer();
            }
            return 0;
        case WM_LBUTTONUP: {
            POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            app->HandleHoverPopupClick(point);
            return 0;
        }
        case WM_RBUTTONUP: {
            POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            ClientToScreen(window_handle, &point);
            app->ShowContextMenu(point);
            return 0;
        }
        case WM_CONTEXTMENU: {
            POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            if (point.x == -1 && point.y == -1) {
                RECT rect{};
                GetWindowRect(window_handle, &rect);
                point.x = rect.left;
                point.y = rect.bottom;
            }
            app->ShowContextMenu(point);
            return 0;
        }
        case WM_NCDESTROY:
            if (app->hover_popup_window_ == window_handle) {
                app->hover_popup_window_ = nullptr;
                app->hover_popup_visible_ = false;
            }
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, 0);
            break;
        default:
            break;
        }

        return DefWindowProcW(window_handle, message, w_param, l_param);
    }

    HINSTANCE instance_handle_{nullptr};
    HWND controller_window_{nullptr};
    HWND widget_window_{nullptr};
    HWND hover_popup_window_{nullptr};
    HFONT font_{nullptr};
    HFONT popup_font_{nullptr};
    HFONT popup_title_font_{nullptr};
    UINT current_dpi_{96};
    UINT taskbar_created_message_{0};
    SIZE widget_size_{};
    SIZE hover_popup_size_{};
    int text_line_height_{0};
    int popup_text_line_height_{0};
    int popup_title_line_height_{0};
    bool has_second_line_{true};
    bool is_shutting_down_{false};
    bool hover_popup_visible_{false};
    bool hover_popup_search_active_{false};
    bool click_popup_started_from_widget_{false};
    bool tray_icon_added_{false};
    HICON tray_icon_handle_{nullptr};
    ULONG_PTR gdiplus_token_{0};
    bool gdiplus_started_{false};
    AppConfig app_config_{};
    MetricsSnapshot last_snapshot_{};
    ProcessPopupSnapshot hover_popup_base_snapshot_{};
    ProcessPopupSnapshot hover_popup_snapshot_{};
    HoverPopupSortMode hover_popup_sort_mode_{HoverPopupSortMode::kDefault};
    int hover_popup_scroll_offset_{0};
    std::wstring hover_popup_search_text_{};
    std::wstring line1_text_{L"CPU 0%  MEM 0%"};
    std::wstring line2_text_{L"\u2191 0bps  \u2193 0bps  R 0B/s  W 0B/s"};
    std::vector<DisplayLines::Column> display_columns_{};
    std::vector<int> column_widths_{};
    ProcessMonitor process_monitor_{};
    SystemMetrics metrics_{};
    TaskbarEmbedder embedder_{};
};

}  // namespace minimal_taskbar_monitor

int APIENTRY wWinMain(HINSTANCE instance_handle,
                      HINSTANCE,
                      LPWSTR,
                      int show_command) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    minimal_taskbar_monitor::MonitorApp app;
    return app.Run(instance_handle, show_command);
}
