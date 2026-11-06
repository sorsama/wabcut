#pragma once

// WindowChromeFilter (0.7, card 017): the custom frame's two missing Windows 11 window features.
//
// The default window is frameless (Qt.FramelessWindowHint; Settings > General > "Use native window frame" brings the
// OS title bar back). Dragging to a screen edge (Aero Snap) and double-click to maximize already work through
// startSystemMove. Two things the OS only gives a window with a system caption:
//
//   * Snap Layouts: hovering the maximize button shows Windows 11's flyout of tiling layouts. Windows asks the window
//     what is under the pointer (WM_NCHITTEST); answering HTMAXBUTTON over the maximize button is what shows the flyout
//     and what makes the OS treat that spot as the button. The window's maximize button (WindowControls) registers its
//     item (addMaximizeButton), the filter answers the hit test from the item's current place - so it follows layout
//     changes and DPI changes without any bookkeeping - and, because Windows then sends non-client mouse messages
//     instead of client ones, it turns them back into what the QML button needs: `nativeHovered` / `nativePressed` on
//     the item (the hover and pressed look) and its clicked() signal (a release over it maximizes or restores).
//     Windows 11 only (build 22000 and later); elsewhere nothing is installed and the button works as before.
//   * The system menu (Restore, Move, Size, Minimize, Maximize, Close) on a right-click of the title bar and Alt+Space:
//     showSystemMenu() pops the window's own system menu, its rows enabled to match the window's state, and posts the
//     chosen command (WM_SYSCOMMAND) back. Windows only; systemMenuAvailable() is false elsewhere and the QML does not
//     offer it.
//
// Everything here runs on the GUI thread. The logic that does not need Windows (which button is under a point, which
// system menu rows are enabled) is public and tested on every platform.

#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QQuickItem>

#include <vector>

class QQuickWindow;
class QWindow;
#ifdef _WIN32
struct tagMSG; // windows.h's MSG
#endif

namespace wabcut::app::chrome {

/// Which system menu rows a window in this state offers (Windows' own rules: a maximized window cannot be moved or
/// sized, a normal one cannot be restored).
struct SystemMenuState {
    bool restore = false;
    bool move = false;
    bool size = false;
    bool minimize = false;
    bool maximize = false;
    bool close = true;
    bool operator==(const SystemMenuState&) const = default;
};
SystemMenuState systemMenuState(bool maximized, bool minimized, bool resizable);

/// The Windows window style bit WS_MAXIMIZEBOX. Windows 11 shows the Snap Layouts flyout only for a window that has it
/// (a frameless Qt window is created as WS_POPUP and gets the box only through Qt::WindowMaximizeButtonHint).
inline constexpr unsigned long kMaximizeBoxStyle = 0x00010000UL;
/// `style` (GWL_STYLE) with the maximize box set.
constexpr unsigned long withMaximizeBox(unsigned long style) {
    return style | kMaximizeBoxStyle;
}
/// Makes sure the native window of `window` has WS_MAXIMIZEBOX, adding it when a window flag change or another Qt
/// version took it away; true when it has it afterwards. False for a null window, one that has no native window yet,
/// a window of one size (it cannot be maximized) and off Windows. The filter calls it for every window whose maximize
/// button it answers for.
bool ensureMaximizeBox(QWindow* window);

/// Whether this system can pop a window's system menu (Windows).
bool systemMenuAvailable();
/// Whether the maximize button gets Windows 11's Snap Layouts flyout (Windows 11, build 22000 or later).
bool snapLayoutsAvailable();

/// Pops the system menu of `window` at `globalPos` (global logical pixels, as QML's eventPoint.globalPosition and
/// Item.mapToGlobal give them) and carries out the chosen command. It returns at once; the menu opens when the pointer
/// event that asked for it has been handled. False where there is no system menu (the window is null or the platform
/// has none).
bool showSystemMenu(QWindow* window, const QPointF& globalPos);

class WindowChromeFilter : public QAbstractNativeEventFilter {
public:
    WindowChromeFilter();
    ~WindowChromeFilter() override;

    /// The process-wide filter. The first registration on Windows 11 installs it into the application.
    static WindowChromeFilter& instance();

    /// A maximize button (a QtQuick Button with `nativeHovered` and `nativePressed` properties): the filter reports it
    /// to the system as the window's maximize button while it is visible and its `snapEnabled` property (when it has
    /// one) is true. Several may be registered (the editor's title bar and Home's); only the ones that count are found.
    /// Registering the same item twice does nothing.
    void addMaximizeButton(QQuickItem* button);
    void removeMaximizeButton(QQuickItem* button);
    int buttonCount() const { return static_cast<int>(m_buttons.size()); }

    /// The registered, visible button of `window` at `clientPixel` (physical pixels from the window's client origin, as
    /// the system reports them), or null.
    QQuickItem* buttonAt(const QQuickWindow* window, QPoint clientPixel) const;

    /// The registered button of `window` that is nativeHovered / nativePressed now (tests).
    QQuickItem* hoveredButton() const { return m_hovered; }

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

#ifdef _WIN32
    /// One window message for `window` (whose window handle is msg.hwnd): true when it was answered here (`*result` set).
    /// nativeEventFilter() finds the window and calls this; tests call it with a synthetic message.
    bool handleMessage(QQuickWindow* window, const tagMSG& msg, qintptr* result);
#endif

private:
    void setHovered(QQuickItem* button);
    void setPressed(QQuickItem* button);

    std::vector<QPointer<QQuickItem>> m_buttons;
    QPointer<QQuickItem> m_hovered;
    QPointer<QQuickItem> m_pressed;
    bool m_installed = false; ///< installed into the application (Windows 11 only)
};

} // namespace wabcut::app::chrome
