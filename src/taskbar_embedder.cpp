#include "taskbar_embedder.h"

#include <shellapi.h>
#include <winternl.h>

#include <algorithm>

namespace minimal_taskbar_monitor {

namespace {

bool IsWindows11OrLater() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return false;
    }

    auto rtl_get_version =
        reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (rtl_get_version == nullptr) {
        return false;
    }

    RTL_OSVERSIONINFOW version_info{};
    version_info.dwOSVersionInfoSize = sizeof(version_info);
    if (rtl_get_version(&version_info) != 0) {
        return false;
    }

    return version_info.dwMajorVersion >= 10 && version_info.dwBuildNumber >= 22000;
}

int RectWidth(const RECT& rect) {
    return rect.right - rect.left;
}

int RectHeight(const RECT& rect) {
    return rect.bottom - rect.top;
}

int ScaleByDpi(UINT dpi, int value) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

}  // namespace

bool TaskbarEmbedder::Attach(HWND widget_window) {
    if (widget_window == nullptr) {
        return false;
    }

    Detach(widget_window);

    if (!ResolveHandles()) {
        return false;
    }

    LONG_PTR style = GetWindowLongPtrW(widget_window, GWL_STYLE);
    style &= ~static_cast<LONG_PTR>(WS_POPUP);
    style |= WS_CHILD | WS_VISIBLE;
    SetWindowLongPtrW(widget_window, GWL_STYLE, style);

    SetLastError(ERROR_SUCCESS);
    if (SetParent(widget_window, parent_window_) == nullptr && GetLastError() != ERROR_SUCCESS) {
        mode_ = Mode::kNone;
        parent_window_ = nullptr;
        return false;
    }

    SetWindowPos(widget_window,
                 HWND_TOP,
                 0,
                 0,
                 0,
                 0,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    return true;
}

void TaskbarEmbedder::Detach(HWND widget_window) {
    RestoreClassicReservation();

    if (widget_window != nullptr && IsWindow(widget_window)) {
        SetParent(widget_window, nullptr);
        LONG_PTR style = GetWindowLongPtrW(widget_window, GWL_STYLE);
        style &= ~static_cast<LONG_PTR>(WS_CHILD);
        style |= WS_POPUP;
        SetWindowLongPtrW(widget_window, GWL_STYLE, style);
        SetWindowPos(widget_window,
                     nullptr,
                     0,
                     0,
                     0,
                     0,
                     SWP_FRAMECHANGED | SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOMOVE |
                         SWP_NOSIZE | SWP_NOZORDER);
    }

    mode_ = Mode::kNone;
    taskbar_window_ = nullptr;
    parent_window_ = nullptr;
    task_list_window_ = nullptr;
    tray_notify_window_ = nullptr;
    start_button_window_ = nullptr;
}

bool TaskbarEmbedder::RefreshLayout(HWND widget_window, const SIZE& desired_size) {
    if (widget_window == nullptr || desired_size.cx <= 0 || desired_size.cy <= 0) {
        return false;
    }

    if (!IsAttached() || !IsHandleAlive(taskbar_window_) || !IsHandleAlive(parent_window_)) {
        if (!Attach(widget_window)) {
            return false;
        }
    }

    if (mode_ == Mode::kWin10Classic) {
        return LayoutClassic(widget_window, desired_size, QueryTaskbarEdge());
    }
    return LayoutNearTray(widget_window, desired_size);
}

UINT TaskbarEmbedder::CurrentDpi() const {
    if (!IsHandleAlive(parent_window_)) {
        return 96;
    }
    return GetDpiForWindow(parent_window_);
}

bool TaskbarEmbedder::IsAttached() const {
    return mode_ != Mode::kNone && IsHandleAlive(taskbar_window_) && IsHandleAlive(parent_window_);
}

bool TaskbarEmbedder::ResolveHandles() {
    taskbar_window_ = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!IsHandleAlive(taskbar_window_)) {
        return false;
    }

    if (IsWindows11OrLater()) {
        return ResolveWin11Handles();
    }
    return ResolveClassicHandles();
}

bool TaskbarEmbedder::ResolveClassicHandles() {
    parent_window_ = FindWindowExW(taskbar_window_, nullptr, L"ReBarWindow32", nullptr);
    if (!IsHandleAlive(parent_window_)) {
        parent_window_ = FindWindowExW(taskbar_window_, nullptr, L"WorkerW", nullptr);
    }
    if (!IsHandleAlive(parent_window_)) {
        parent_window_ = taskbar_window_;
    }

    task_list_window_ = FindWindowExW(parent_window_, nullptr, L"MSTaskSwWClass", nullptr);
    if (!IsHandleAlive(task_list_window_)) {
        task_list_window_ = FindWindowExW(parent_window_, nullptr, L"MSTaskListWClass", nullptr);
    }

    tray_notify_window_ = FindWindowExW(taskbar_window_, nullptr, L"TrayNotifyWnd", nullptr);
    mode_ = Mode::kWin10Classic;
    return IsHandleAlive(parent_window_);
}

bool TaskbarEmbedder::ResolveWin11Handles() {
    parent_window_ = taskbar_window_;
    tray_notify_window_ = FindWindowExW(taskbar_window_, nullptr, L"TrayNotifyWnd", nullptr);
    start_button_window_ = FindWindowExW(taskbar_window_, nullptr, L"Start", nullptr);
    mode_ = Mode::kWin11;
    return true;
}

bool TaskbarEmbedder::LayoutClassic(HWND widget_window,
                                    const SIZE& desired_size,
                                    UINT taskbar_edge) {
    if (!IsHandleAlive(parent_window_)) {
        return false;
    }

    if (!IsHandleAlive(task_list_window_)) {
        return LayoutNearTray(widget_window, desired_size);
    }

    RECT parent_rect{};
    RECT task_list_rect{};
    if (!GetWindowRect(parent_window_, &parent_rect) ||
        !GetWindowRect(task_list_window_, &task_list_rect)) {
        return false;
    }

    OffsetRect(&task_list_rect, -parent_rect.left, -parent_rect.top);

    const bool horizontal = (taskbar_edge == ABE_BOTTOM || taskbar_edge == ABE_TOP);
    if (horizontal) {
        const int base_width = RectWidth(task_list_rect) + reserved_extent_;
        const int desired_width = static_cast<int>(desired_size.cx);
        const int desired_height = static_cast<int>(desired_size.cy);
        const int new_task_list_width = std::max<int>(0, base_width - desired_width);
        MoveWindow(task_list_window_,
                   task_list_rect.left,
                   task_list_rect.top,
                   new_task_list_width,
                   RectHeight(task_list_rect),
                   TRUE);

        const int x = task_list_rect.left + new_task_list_width;
        const int y = std::max<int>(0, (RectHeight(parent_rect) - desired_height) / 2);
        SetWindowPos(widget_window,
                     HWND_TOP,
                     x,
                     y,
                     desired_width,
                     desired_height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        reserved_extent_ = desired_width;
        return true;
    }

    const int base_height = RectHeight(task_list_rect) + reserved_extent_;
    const int desired_width = static_cast<int>(desired_size.cx);
    const int desired_height = static_cast<int>(desired_size.cy);
    const int new_task_list_height = std::max<int>(0, base_height - desired_height);
    MoveWindow(task_list_window_,
               task_list_rect.left,
               task_list_rect.top,
               RectWidth(task_list_rect),
               new_task_list_height,
               TRUE);

    const int x = std::max<int>(0, (RectWidth(parent_rect) - desired_width) / 2);
    const int y = task_list_rect.top + new_task_list_height;
    SetWindowPos(widget_window,
                 HWND_TOP,
                 x,
                 y,
                 desired_width,
                 desired_height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    reserved_extent_ = desired_height;
    return true;
}

bool TaskbarEmbedder::LayoutNearTray(HWND widget_window, const SIZE& desired_size) {
    if (!IsHandleAlive(taskbar_window_) || !IsHandleAlive(parent_window_)) {
        return false;
    }

    RECT taskbar_rect{};
    if (!GetWindowRect(taskbar_window_, &taskbar_rect)) {
        return false;
    }

    const int taskbar_width = RectWidth(taskbar_rect);
    const int taskbar_height = RectHeight(taskbar_rect);
    const int desired_width = static_cast<int>(desired_size.cx);
    const int desired_height = static_cast<int>(desired_size.cy);
    const int margin = std::max(ScaleByDpi(CurrentDpi(), 4), 2);

    int x = taskbar_width - desired_width - margin - static_cast<int>(CurrentDpi() * 0.9);
    if (IsHandleAlive(tray_notify_window_)) {
        RECT tray_rect{};
        if (GetWindowRect(tray_notify_window_, &tray_rect)) {
            x = tray_rect.left - taskbar_rect.left - desired_width - margin;
        }
    }

    x = std::max<int>(0, x);
    const int y = std::max<int>(0, (taskbar_height - desired_height) / 2);
    SetWindowPos(widget_window,
                 HWND_TOP,
                 x,
                 y,
                 desired_width,
                 desired_height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

void TaskbarEmbedder::RestoreClassicReservation() {
    if (mode_ != Mode::kWin10Classic || reserved_extent_ <= 0 || !IsHandleAlive(task_list_window_)) {
        reserved_extent_ = 0;
        return;
    }

    RECT taskbar_rect{};
    RECT parent_rect{};
    RECT task_list_rect{};
    if (!GetWindowRect(taskbar_window_, &taskbar_rect) ||
        !GetWindowRect(parent_window_, &parent_rect) ||
        !GetWindowRect(task_list_window_, &task_list_rect)) {
        reserved_extent_ = 0;
        return;
    }

    OffsetRect(&task_list_rect, -parent_rect.left, -parent_rect.top);

    const bool horizontal = RectWidth(taskbar_rect) >= RectHeight(taskbar_rect);
    if (horizontal) {
        MoveWindow(task_list_window_,
                   task_list_rect.left,
                   task_list_rect.top,
                   RectWidth(task_list_rect) + reserved_extent_,
                   RectHeight(task_list_rect),
                   TRUE);
    } else {
        MoveWindow(task_list_window_,
                   task_list_rect.left,
                   task_list_rect.top,
                   RectWidth(task_list_rect),
                   RectHeight(task_list_rect) + reserved_extent_,
                   TRUE);
    }

    reserved_extent_ = 0;
}

bool TaskbarEmbedder::IsHandleAlive(HWND window_handle) const {
    return window_handle != nullptr && IsWindow(window_handle) != FALSE;
}

UINT TaskbarEmbedder::QueryTaskbarEdge() const {
    APPBARDATA appbar_data{};
    appbar_data.cbSize = sizeof(appbar_data);
    appbar_data.hWnd = taskbar_window_;
    if (SHAppBarMessage(ABM_GETTASKBARPOS, &appbar_data)) {
        return appbar_data.uEdge;
    }
    return ABE_BOTTOM;
}

}  // namespace minimal_taskbar_monitor
