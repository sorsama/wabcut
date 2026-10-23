#pragma once

// WindowPlacement (UX rework, stream S): the main window comes back where it was. Its windowed geometry and whether it
// was maximized are kept between runs (QSettings "window/x", "window/y", "window/width", "window/height" - the
// windowed geometry, also while maximized - and "window/maximized"). Restoring checks the screens there are now: a
// window whose title strip is on none of them (a monitor unplugged) is centred on the primary screen, and one larger
// than its screen is cut to it. Only main() uses it: before the window loads (QML initial properties, so the window
// opens in place, with no jump) and after (a keeper follows the window and saves). The tests' windows are never restored
// or saved.

#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include <optional>

class QWindow;

namespace wabcut::app {

struct WindowPlacement {
    QRect normal;           ///< the windowed geometry (device-independent pixels, without the frame)
    bool maximized = false; ///< maximized (or full screen) when it was last seen

    friend bool operator==(const WindowPlacement&, const WindowPlacement&) = default;
};

/// Where a window saved at `saved` goes on `screens` (their available geometries; the first is the primary screen):
///   * at least 64 x 24 px of its top 32 px (the title bar) lie on a screen: it stays where it was, only cut to that
///     screen's size when larger (then moved inside it along the cut side) and never above the screen's top edge;
///   * otherwise it is centred on the primary screen (cut to it).
/// Never smaller than `minimum`. nullopt without screens or for an empty `saved`.
std::optional<QRect> placeOnScreens(const QRect& saved, const QList<QRect>& screens, const QSize& minimum = {});

/// The placement saved in QSettings group `group`, or nullopt when none was (or it is unusable).
std::optional<WindowPlacement> loadWindowPlacement(const QString& group = QStringLiteral("window"));
void saveWindowPlacement(const WindowPlacement& placement, const QString& group = QStringLiteral("window"));

/// The QML initial properties that open the main window where it was: { x, y, width, height } fitted to the screens
/// there are now (QGuiApplication::screens(), placeOnScreens), plus visibility: Window.Maximized (4) when it was
/// maximized. {} when nothing was saved or there is no screen.
QVariantMap restoredWindowProperties(const QString& group = QStringLiteral("window"), const QSize& minimum = {});

/// Follows `window`'s windowed geometry and maximized state (a minimized window keeps what it had) and saves them:
/// 400 ms after they last changed, when the application is about to quit, and when the keeper goes (it is a child of
/// the window). Seeded with the saved placement, so a window that starts maximized keeps its windowed geometry.
class WindowPlacementKeeper : public QObject {
public:
    explicit WindowPlacementKeeper(QWindow* window, QString group = QStringLiteral("window"));
    ~WindowPlacementKeeper() override;

    WindowPlacement placement() const { return m_placement; }
    /// Reads the window's state now (what the timer does after a change).
    void capture();
    /// capture(), then writes the placement when it changed since the last write.
    void save();

private:
    void write();

    QPointer<QWindow> m_window;
    QString m_group;
    WindowPlacement m_placement;
    std::optional<WindowPlacement> m_written;
    QTimer m_settle;
};

} // namespace wabcut::app
