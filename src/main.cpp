#include "app_config.h"
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
#include <cstring>
#include <memory>
#include <string>

namespace minimal_taskbar_monitor {

namespace {

constexpr wchar_t kControllerClassName[] = L"MinimalTaskbarMonitorControllerWindow";
constexpr wchar_t kWidgetClassName[] = L"MinimalTaskbarMonitorWidgetWindow";
constexpr UINT_PTR kSampleTimerId = 1;
constexpr UINT_PTR kLayoutTimerId = 2;
constexpr UINT_PTR kReattachTimerId = 3;
constexpr UINT kSampleIntervalMs = 1000;
constexpr UINT kLayoutIntervalMs = 1000;
constexpr UINT kReattachDelayMs = 600;
constexpr UINT kTrayIconCallbackMessage = WM_APP + 1;
constexpr UINT kTrayIconId = 1;
constexpr UINT kExitCommandId = 1001;
constexpr UINT kAutoStartCommandId = 1002;
constexpr UINT kMetricCpuCommandId = 1101;
constexpr UINT kMetricMemoryCommandId = 1102;
constexpr UINT kMetricUploadCommandId = 1103;
constexpr UINT kMetricDownloadCommandId = 1104;
constexpr UINT kMetricGpuCommandId = 1105;
constexpr UINT kMetricDiskReadCommandId = 1106;
constexpr UINT kMetricDiskWriteCommandId = 1107;
constexpr wchar_t kRunRegistryPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"MinimalTaskbarMonitor";

struct WidgetPalette {
    COLORREF background;
    COLORREF border;
    COLORREF primary_text;
    COLORREF secondary_text;
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

HFONT CreatePreferredUiFont(UINT dpi) {
    const LOGFONTW font_template{
        .lfHeight = -ScaleByDpi(dpi, 12),
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
    }

    bool Initialize() {
        last_snapshot_ = metrics_.Sample();
        UpdateDisplayLines(last_snapshot_);
        if (!EnsureWidgetWindow()) {
            return false;
        }

        ReattachWidget();
        EnsureTrayIcon();

        SetTimer(controller_window_, kSampleTimerId, kSampleIntervalMs, nullptr);
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

        if (widget_window_ != nullptr && IsWindow(widget_window_)) {
            DestroyWindow(widget_window_);
            widget_window_ = nullptr;
        }
    }

    void ReattachWidget() {
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
        if (dpi != current_dpi_ || font_ == nullptr) {
            current_dpi_ = dpi;
            if (font_ != nullptr) {
                DeleteObject(font_);
                font_ = nullptr;
            }

            font_ = CreatePreferredUiFont(current_dpi_);
        }

        HDC screen_dc = GetDC(nullptr);
        HFONT active_font =
            font_ != nullptr ? font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ old_font = SelectObject(screen_dc, active_font);
        SIZE line1_size{};
        SIZE line2_size{};
        const DisplayLines sample_lines = GetMetricsSampleLines(app_config_.visible_metrics);
        GetTextExtentPoint32W(screen_dc,
                              sample_lines.line1.c_str(),
                              static_cast<int>(sample_lines.line1.size()),
                              &line1_size);
        GetTextExtentPoint32W(screen_dc,
                              sample_lines.line2.c_str(),
                              static_cast<int>(sample_lines.line2.size()),
                              &line2_size);
        TEXTMETRICW text_metrics{};
        GetTextMetricsW(screen_dc, &text_metrics);
        SelectObject(screen_dc, old_font);
        ReleaseDC(nullptr, screen_dc);

        text_line_height_ = text_metrics.tmHeight;
        const int horizontal_padding = ScaleByDpi(current_dpi_, 8);
        const int vertical_padding = ScaleByDpi(current_dpi_, 5);
        const int line_gap = ScaleByDpi(current_dpi_, 2);
        const bool has_second_line = !sample_lines.line2.empty();

        widget_size_.cx = std::max(line1_size.cx, line2_size.cx) + horizontal_padding * 2;
        widget_size_.cy =
            std::max<int>((has_second_line ? text_line_height_ * 2 + line_gap : text_line_height_) +
                              vertical_padding * 2,
                          ScaleByDpi(current_dpi_, has_second_line ? 32 : 24));
    }

    void UpdateDisplayLines(const MetricsSnapshot& snapshot) {
        const DisplayLines lines = FormatMetricsLines(snapshot, app_config_.visible_metrics);
        line1_text_ = lines.line1;
        line2_text_ = lines.line2;
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

        SelectObject(dc, old_font);
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
                return;
            }
        }
        RequestWidgetRedraw();
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
        }
        if (!embedder_.RefreshLayout(widget_window_, widget_size_)) {
            ShowWindow(widget_window_, SW_HIDE);
            SetTimer(controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
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
        HMENU menu = CreatePopupMenu();
        HMENU metrics_menu = CreatePopupMenu();
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

        AppendMenuW(menu,
                    MF_POPUP,
                    reinterpret_cast<UINT_PTR>(metrics_menu),
                    L"Visible Metrics");
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

    HINSTANCE instance_handle_{nullptr};
    HWND controller_window_{nullptr};
    HWND widget_window_{nullptr};
    HFONT font_{nullptr};
    UINT current_dpi_{96};
    UINT taskbar_created_message_{0};
    SIZE widget_size_{};
    int text_line_height_{0};
    bool has_second_line_{true};
    bool is_shutting_down_{false};
    bool tray_icon_added_{false};
    HICON tray_icon_handle_{nullptr};
    ULONG_PTR gdiplus_token_{0};
    bool gdiplus_started_{false};
    AppConfig app_config_{};
    MetricsSnapshot last_snapshot_{};
    std::wstring line1_text_{L"CPU 0%  MEM 0%"};
    std::wstring line2_text_{L"\u2191 0B/s  \u2193 0B/s  R 0B/s  W 0B/s"};
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
