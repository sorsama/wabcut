#include "Editor.h"

#include <wabcut/plughost/Ipc.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace wabcut::plughost {

namespace {

constexpr wchar_t kClassName[] = L"WabcutPluginEditor";

LRESULT CALLBACK editorProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<EditorWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        break;
    }
    case WM_CLOSE:
        if (self != nullptr) {
            self->closeRequested();
            return 0;
        }
        break;
    case WM_SIZE:
        if (self != nullptr && wparam != SIZE_MINIMIZED) {
            self->userResized(LOWORD(lparam), HIWORD(lparam));
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool registerClass() {
    static const bool registered = [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = editorProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kClassName;
        return RegisterClassExW(&wc) != 0;
    }();
    return registered;
}

DWORD styleFor(bool resizable) {
    return resizable ? WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX : (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
}

} // namespace

std::unique_ptr<EditorWindow> EditorWindow::create(const std::string& title, int width, int height, bool resizable,
                                                   Callbacks callbacks, std::string& error) {
    if (!registerClass()) {
        error = "cannot register the editor window class";
        return nullptr;
    }
    std::unique_ptr<EditorWindow> w(new EditorWindow());
    w->m_callbacks = std::move(callbacks);
    const DWORD style = styleFor(resizable);
    RECT r{0, 0, std::max(width, 64), std::max(height, 32)};
    AdjustWindowRectEx(&r, style, FALSE, 0);
    HWND hwnd = CreateWindowExW(0, kClassName, widen(title).c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                                r.bottom - r.top, nullptr, nullptr, GetModuleHandleW(nullptr), w.get());
    if (hwnd == nullptr) {
        error = "cannot create the editor window";
        return nullptr;
    }
    w->m_hwnd = hwnd;
    return w;
}

EditorWindow::~EditorWindow() {
    if (m_hwnd != nullptr) {
        SetWindowLongPtrW(static_cast<HWND>(m_hwnd), GWLP_USERDATA, 0);
        DestroyWindow(static_cast<HWND>(m_hwnd));
    }
}

void EditorWindow::setClientSize(int width, int height) {
    auto* hwnd = static_cast<HWND>(m_hwnd);
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    RECT r{0, 0, std::max(width, 64), std::max(height, 32)};
    AdjustWindowRectEx(&r, style, FALSE, 0);
    m_settingSize = true;
    SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    m_settingSize = false;
}

void EditorWindow::show() {
    ShowWindow(static_cast<HWND>(m_hwnd), SW_SHOW);
    SetForegroundWindow(static_cast<HWND>(m_hwnd));
}

void EditorWindow::closeRequested() {
    if (m_callbacks.closed) {
        m_callbacks.closed();
    }
}

void EditorWindow::userResized(int width, int height) {
    if (!m_settingSize && m_callbacks.resized) {
        m_callbacks.resized(width, height);
    }
}

} // namespace wabcut::plughost
