#include "WindowPlacement.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QWindow>

#include <algorithm>
#include <utility>

namespace wabcut::app {

namespace {

/// The title bar: what has to be on a screen for the window to count as being there.
constexpr int kTitleStrip = 32;
constexpr int kMinVisibleWidth = 64;
constexpr int kMinVisibleHeight = 24;
/// Saves wait until the window stopped moving or resizing this long.
constexpr int kSettleMs = 400;

QString key(const QString& group, const char* name) {
    return group + QLatin1Char('/') + QLatin1String(name);
}

/// `value` within [lo, hi]; `lo` when the range is empty.
int clampInto(int value, int lo, int hi) {
    return hi < lo ? lo : std::clamp(value, lo, hi);
}

} // namespace

std::optional<QRect> placeOnScreens(const QRect& saved, const QList<QRect>& screens, const QSize& minimum) {
    if (screens.isEmpty() || saved.width() <= 0 || saved.height() <= 0)
        return std::nullopt;
    const QRect strip(saved.x(), saved.y(), saved.width(), std::min(kTitleStrip, saved.height()));
    int best = -1;
    qint64 bestArea = 0;
    for (int i = 0; i < screens.size(); ++i) {
        const QRect on = strip.intersected(screens[i]);
        if (on.width() < kMinVisibleWidth || on.height() < kMinVisibleHeight)
            continue;
        const qint64 area = static_cast<qint64>(on.width()) * on.height();
        if (area > bestArea) {
            bestArea = area;
            best = i;
        }
    }
    const QRect& screen = screens[best >= 0 ? best : 0];
    const QSize size = QSize(std::min(saved.width(), screen.width()), std::min(saved.height(), screen.height()))
                           .expandedTo(minimum);
    if (best < 0) // on no screen now (a monitor unplugged, another layout): centred on the primary one
        return QRect(QPoint(screen.x() + (screen.width() - size.width()) / 2,
                            screen.y() + (screen.height() - size.height()) / 2),
                     size);
    QPoint at = saved.topLeft();
    if (size.width() < saved.width())
        at.setX(clampInto(at.x(), screen.left(), screen.left() + screen.width() - size.width()));
    if (size.height() < saved.height())
        at.setY(clampInto(at.y(), screen.top(), screen.top() + screen.height() - size.height()));
    at.setY(std::max(at.y(), screen.top())); // the title bar never above the screen
    return QRect(at, size);
}

std::optional<WindowPlacement> loadWindowPlacement(const QString& group) {
    const QSettings settings;
    if (!settings.contains(key(group, "width")) || !settings.contains(key(group, "height")))
        return std::nullopt;
    bool okX = false, okY = false, okWidth = false, okHeight = false;
    const int x = settings.value(key(group, "x")).toInt(&okX);
    const int y = settings.value(key(group, "y")).toInt(&okY);
    const int width = settings.value(key(group, "width")).toInt(&okWidth);
    const int height = settings.value(key(group, "height")).toInt(&okHeight);
    if (!okX || !okY || !okWidth || !okHeight || width <= 0 || height <= 0)
        return std::nullopt;
    return WindowPlacement{QRect(x, y, width, height), settings.value(key(group, "maximized"), false).toBool()};
}

void saveWindowPlacement(const WindowPlacement& placement, const QString& group) {
    if (placement.normal.width() <= 0 || placement.normal.height() <= 0)
        return;
    QSettings settings;
    settings.setValue(key(group, "x"), placement.normal.x());
    settings.setValue(key(group, "y"), placement.normal.y());
    settings.setValue(key(group, "width"), placement.normal.width());
    settings.setValue(key(group, "height"), placement.normal.height());
    settings.setValue(key(group, "maximized"), placement.maximized);
}

QVariantMap restoredWindowProperties(const QString& group, const QSize& minimum) {
    const std::optional<WindowPlacement> saved = loadWindowPlacement(group);
    if (!saved)
        return {};
    QList<QRect> screens;
    QScreen* primary = QGuiApplication::primaryScreen();
    if (primary)
        screens.append(primary->availableGeometry());
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen != primary)
            screens.append(screen->availableGeometry());
    }
    const std::optional<QRect> rect = placeOnScreens(saved->normal, screens, minimum);
    if (!rect)
        return {};
    QVariantMap properties{
        {QStringLiteral("x"), rect->x()},
        {QStringLiteral("y"), rect->y()},
        {QStringLiteral("width"), rect->width()},
        {QStringLiteral("height"), rect->height()},
    };
    if (saved->maximized)
        properties.insert(QStringLiteral("visibility"), QVariant::fromValue(QWindow::Maximized));
    return properties;
}

WindowPlacementKeeper::WindowPlacementKeeper(QWindow* window, QString group)
    : QObject(window), m_window(window), m_group(std::move(group)) {
    if (const std::optional<WindowPlacement> saved = loadWindowPlacement(m_group)) {
        m_placement = *saved;
        m_written = *saved;
    }
    m_settle.setSingleShot(true);
    m_settle.setInterval(kSettleMs);
    connect(&m_settle, &QTimer::timeout, this, &WindowPlacementKeeper::save);
    if (window) {
        // Read once things settled: while maximizing, the geometry can change before the state does.
        const auto changed = [this] { m_settle.start(); };
        connect(window, &QWindow::xChanged, this, changed);
        connect(window, &QWindow::yChanged, this, changed);
        connect(window, &QWindow::widthChanged, this, changed);
        connect(window, &QWindow::heightChanged, this, changed);
        connect(window, &QWindow::windowStateChanged, this, changed);
        capture();
    }
    if (QCoreApplication* app = QCoreApplication::instance())
        connect(app, &QCoreApplication::aboutToQuit, this, &WindowPlacementKeeper::save);
}

WindowPlacementKeeper::~WindowPlacementKeeper() {
    // As a child of the window it goes after the window is gone (the pointer is null then): what was last read is
    // written. Destroyed on its own, it reads the window once more.
    save();
}

void WindowPlacementKeeper::capture() {
    if (!m_window)
        return;
    const Qt::WindowStates states = m_window->windowStates();
    if (states & Qt::WindowMinimized)
        return; // keeps what it was before
    m_placement.maximized = (states & (Qt::WindowMaximized | Qt::WindowFullScreen)) != 0;
    if (!m_placement.maximized && m_window->width() > 0 && m_window->height() > 0)
        m_placement.normal = m_window->geometry();
}

void WindowPlacementKeeper::save() {
    capture();
    write();
}

void WindowPlacementKeeper::write() {
    if (m_placement.normal.width() <= 0 || m_placement.normal.height() <= 0)
        return;
    if (m_written && *m_written == m_placement)
        return;
    saveWindowPlacement(m_placement, m_group);
    m_written = m_placement;
}

} // namespace wabcut::app
