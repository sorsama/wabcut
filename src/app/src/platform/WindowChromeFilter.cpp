#include "WindowChromeFilter.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QOperatingSystemVersion>
#include <QQuickItem>
#include <QQuickWindow>
#include <QWindow>

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#endif

namespace wabcut::app::chrome {

namespace {

WindowChromeFilter* g_instance = nullptr;

#ifdef _WIN32
/// Pops `window`'s system menu (the part of showSystemMenu that runs once the pointer event is done: the menu is modal).
/// https://learn.microsoft.com/windows/win32/menurc/using-menus (system menu: GetSystemMenu, TrackPopupMenu with
/// TPM_RETURNCMD, the command posted back as WM_SYSCOMMAND).
void popSystemMenu(QWindow* window, const QPointF& globalPos) {
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!hwnd || !IsWindow(hwnd))
        return;
    HMENU menu = GetSystemMenu(hwnd, FALSE);
    if (!menu) {
        // A frameless window made without WS_SYSMENU has no system menu to get: give it one (nothing is drawn for it, the
        // window has no caption) and ask again.
        SetWindowLongPtrW(hwnd, GWL_STYLE, GetWindowLongPtrW(hwnd, GWL_STYLE) | WS_SYSMENU);
        menu = GetSystemMenu(hwnd, FALSE);
        if (!menu)
            return;
    }
    const SystemMenuState state =
        systemMenuState(IsZoomed(hwnd) != 0, IsIconic(hwnd) != 0, window->minimumSize() != window->maximumSize());
    const auto enable = [menu](UINT command, bool on) {
        EnableMenuItem(menu, command, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED));
    };
    enable(SC_RESTORE, state.restore);
    enable(SC_MOVE, state.move);
    enable(SC_SIZE, state.size);
    enable(SC_MINIMIZE, state.minimize);
    enable(SC_MAXIMIZE, state.maximize);
    enable(SC_CLOSE, state.close);

    // Global logical pixels -> this window's physical client pixels -> screen pixels.
    const qreal dpr = window->devicePixelRatio();
    const QPoint local = window->mapFromGlobal(globalPos.toPoint());
    POINT point{static_cast<LONG>(std::lround(local.x() * dpr)), static_cast<LONG>(std::lround(local.y() * dpr))};
    ClientToScreen(hwnd, &point);

    SetForegroundWindow(hwnd); // a popup menu of a background window would not close when clicking elsewhere
    const UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    const int command = TrackPopupMenu(menu, align | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD, point.x, point.y, 0,
                                       hwnd, nullptr);
    if (command != 0)
        PostMessageW(hwnd, WM_SYSCOMMAND, static_cast<WPARAM>(command), MAKELPARAM(point.x, point.y));
    PostMessageW(hwnd, WM_NULL, 0, 0); // the documented companion of TrackPopupMenu
}
#endif

} // namespace

SystemMenuState systemMenuState(bool maximized, bool minimized, bool resizable) {
    SystemMenuState state;
    state.restore = maximized || minimized;
    state.move = !maximized && !minimized;
    state.size = !maximized && !minimized && resizable;
    state.minimize = !minimized;
    state.maximize = !maximized && resizable;
    state.close = true;
    return state;
}

bool ensureMaximizeBox(QWindow* window) {
#ifdef _WIN32
    if (!window || window->minimumSize() == window->maximumSize())
        return false;
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!hwnd || !IsWindow(hwnd))
        return false;
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_MAXIMIZEBOX) == 0)
        SetWindowLongPtrW(hwnd, GWL_STYLE, static_cast<LONG_PTR>(withMaximizeBox(static_cast<unsigned long>(style))));
    return true;
#else
    Q_UNUSED(window);
    return false;
#endif
}

bool systemMenuAvailable() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

bool snapLayoutsAvailable() {
#ifdef _WIN32
    // Windows 11 is Windows 10's version number with a build of 22000 or later.
    const QOperatingSystemVersion version = QOperatingSystemVersion::current();
    return version.majorVersion() == 10 && version.microVersion() >= 22000;
#else
    return false;
#endif
}

bool showSystemMenu(QWindow* window, const QPointF& globalPos) {
#ifdef _WIN32
    if (!window || !QCoreApplication::instance())
        return false;
    // The menu is modal (its own message loop): open it once the pointer event that asked for it has been handled.
    QPointer<QWindow> guard(window);
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [guard, globalPos] {
            if (guard)
                popSystemMenu(guard, globalPos);
        },
        Qt::QueuedConnection);
    return true;
#else
    Q_UNUSED(window);
    Q_UNUSED(globalPos);
    return false;
#endif
}

WindowChromeFilter::WindowChromeFilter() = default;

WindowChromeFilter::~WindowChromeFilter() {
    if (m_installed && QCoreApplication::instance())
        QCoreApplication::instance()->removeNativeEventFilter(this);
}

WindowChromeFilter& WindowChromeFilter::instance() {
    if (!g_instance) {
        g_instance = new WindowChromeFilter;
        // Gone with the application, before its event dispatcher.
        qAddPostRoutine([] {
            delete g_instance;
            g_instance = nullptr;
        });
    }
    return *g_instance;
}

void WindowChromeFilter::addMaximizeButton(QQuickItem* button) {
    if (!button)
        return;
    std::erase_if(m_buttons, [](const QPointer<QQuickItem>& b) { return b.isNull(); });
    if (std::ranges::any_of(m_buttons, [button](const QPointer<QQuickItem>& b) { return b == button; }))
        return;
    m_buttons.emplace_back(button);
    if (!m_installed && snapLayoutsAvailable() && QCoreApplication::instance()) {
        QCoreApplication::instance()->installNativeEventFilter(this);
        m_installed = true;
    }
}

void WindowChromeFilter::removeMaximizeButton(QQuickItem* button) {
    std::erase_if(m_buttons, [button](const QPointer<QQuickItem>& b) { return b.isNull() || b == button; });
    if (m_hovered == button)
        m_hovered = nullptr;
    if (m_pressed == button)
        m_pressed = nullptr;
}

QQuickItem* WindowChromeFilter::buttonAt(const QQuickWindow* window, QPoint clientPixel) const {
    if (!window)
        return nullptr;
    const qreal dpr = window->devicePixelRatio();
    const QPointF point(clientPixel);
    for (const QPointer<QQuickItem>& button : m_buttons) {
        if (!button || button->window() != window || !button->isVisible() || button->width() <= 0 || button->height() <= 0)
            continue;
        // A button the QML turned off (its bar is under another screen) is not the window's maximize button now.
        if (const QVariant enabled = button->property("snapEnabled"); enabled.isValid() && !enabled.toBool())
            continue;
        const QRectF scene = button->mapRectToScene(QRectF(0, 0, button->width(), button->height()));
        const QRectF pixels(scene.x() * dpr, scene.y() * dpr, scene.width() * dpr, scene.height() * dpr);
        if (pixels.contains(point))
            return button;
    }
    return nullptr;
}

void WindowChromeFilter::setHovered(QQuickItem* button) {
    if (m_hovered == button)
        return;
    if (m_hovered)
        m_hovered->setProperty("nativeHovered", false);
    m_hovered = button;
    if (button)
        button->setProperty("nativeHovered", true);
}

void WindowChromeFilter::setPressed(QQuickItem* button) {
    if (m_pressed == button)
        return;
    if (m_pressed)
        m_pressed->setProperty("nativePressed", false);
    m_pressed = button;
    if (button)
        button->setProperty("nativePressed", true);
}

bool WindowChromeFilter::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) {
#ifdef _WIN32
    if (m_buttons.empty() || eventType != "windows_generic_MSG" || !message)
        return false;
    const MSG& msg = *static_cast<const MSG*>(message);
    switch (msg.message) {
    case WM_NCHITTEST:
    case WM_NCMOUSEMOVE:
    case WM_NCMOUSELEAVE:
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
    case WM_NCLBUTTONUP:
    case WM_MOUSEMOVE:
        break;
    default:
        return false;
    }
    // Only a window that is shown has a handle to compare (and a hidden one - the tests' - must not be made to have one).
    for (const QPointer<QQuickItem>& button : m_buttons) {
        QQuickWindow* window = button ? button->window() : nullptr;
        if (window && window->isVisible() && reinterpret_cast<HWND>(window->winId()) == msg.hwnd)
            return handleMessage(window, msg, result);
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif
    return false;
}

#ifdef _WIN32
bool WindowChromeFilter::handleMessage(QQuickWindow* window, const MSG& msg, qintptr* result) {
    // Review (card 017): the flyout is only offered to a window with WS_MAXIMIZEBOX; one pointer message in is enough to
    // be sure it has it (a style read when it does).
    ensureMaximizeBox(window);
    // The pointer, for the non-client messages (screen pixels), in the window's client pixels.
    const auto clientPoint = [&msg] {
        POINT point{GET_X_LPARAM(msg.lParam), GET_Y_LPARAM(msg.lParam)};
        ScreenToClient(msg.hwnd, &point);
        return QPoint(point.x, point.y);
    };
    switch (msg.message) {
    case WM_NCHITTEST:
        // The answer that shows the Snap Layouts flyout.
        if (buttonAt(window, clientPoint())) {
            *result = HTMAXBUTTON;
            return true;
        }
        return false;
    case WM_NCMOUSEMOVE:
        if (msg.wParam == HTMAXBUTTON) {
            if (QQuickItem* button = buttonAt(window, clientPoint())) {
                setHovered(button);
                // WM_NCMOUSELEAVE only comes when asked for.
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE | TME_NONCLIENT, msg.hwnd, 0};
                TrackMouseEvent(&track);
                *result = 0;
                return true;
            }
        }
        setHovered(nullptr);
        return false;
    case WM_NCMOUSELEAVE:
    case WM_MOUSEMOVE: // back in the client area (Qt gets this one too)
        setHovered(nullptr);
        setPressed(nullptr);
        return false;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (msg.wParam == HTMAXBUTTON) {
            // Windows would track the press itself for a real caption button; here it is swallowed and the release
            // decides (the QML button draws the pressed look from nativePressed).
            setPressed(buttonAt(window, clientPoint()));
            *result = 0;
            return true;
        }
        return false;
    case WM_NCLBUTTONUP:
        if (msg.wParam == HTMAXBUTTON) {
            QQuickItem* pressed = m_pressed;
            setPressed(nullptr);
            // A click: the press began on this button and the release is on it. The QML button maximizes or restores.
            if (pressed && pressed == buttonAt(window, clientPoint()))
                QMetaObject::invokeMethod(pressed, "clicked", Qt::QueuedConnection);
            *result = 0;
            return true;
        }
        return false;
    default:
        return false;
    }
}
#endif

} // namespace wabcut::app::chrome
