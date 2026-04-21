#pragma once

#include <windows.h>

namespace minimal_taskbar_monitor {

class TaskbarEmbedder {
public:
    TaskbarEmbedder() = default;
    ~TaskbarEmbedder() = default;

    bool Attach(HWND widget_window);
    void Detach(HWND widget_window);
    bool RefreshLayout(HWND widget_window, const SIZE& desired_size);
    UINT CurrentDpi() const;
    bool IsAttached() const;

private:
    enum class Mode {
        kNone,
        kWin10Classic,
        kWin11
    };

    bool ResolveHandles();
    bool ResolveClassicHandles();
    bool ResolveWin11Handles();
    bool LayoutClassic(HWND widget_window, const SIZE& desired_size, UINT taskbar_edge);
    bool LayoutNearTray(HWND widget_window, const SIZE& desired_size);
    void RestoreClassicReservation();
    bool IsHandleAlive(HWND window_handle) const;
    UINT QueryTaskbarEdge() const;

    Mode mode_{Mode::kNone};
    HWND taskbar_window_{nullptr};
    HWND parent_window_{nullptr};
    HWND task_list_window_{nullptr};
    HWND tray_notify_window_{nullptr};
    HWND start_button_window_{nullptr};
    int reserved_extent_{0};
};

}  // namespace minimal_taskbar_monitor
