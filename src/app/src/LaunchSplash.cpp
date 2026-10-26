#include "LaunchSplash.h"

#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWindow>
#include <QRect>
#include <QScreen>
#include <QSettings>
#include <QSize>
#include <QSurfaceFormat>
#include <QVariantMap>

namespace wabcut::app {

namespace {

constexpr int kFirstFrameMs = 800; // a window that draws nothing yet still plays (it shows once it can)
constexpr int kSettleMs = 120;     // the frame with the complete mark, or this long
constexpr int kWatchdogMs = 6000;  // the intro takes 2.1 s: the main window never waits longer than this
constexpr int kMainFrameMs = 4000; // the main window's first frame, or this long

constexpr auto kQueuedOnce = static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::SingleShotConnection);

} // namespace

bool LaunchSplash::wanted() {
    // Preferences.qml keeps both through QtCore Settings, in the same place ("true" / "false").
    const QSettings settings;
    return settings.value(QStringLiteral("appearance/launchIntro"), true).toBool() &&
           !settings.value(QStringLiteral("appearance/reduceMotion"), false).toBool();
}

LaunchSplash::LaunchSplash(QPoint centre, QObject* parent) : QObject(parent), m_centre(centre) {
    for (QTimer* timer : {&m_firstFrame, &m_settle, &m_watchdog, &m_mainFrame}) {
        timer->setSingleShot(true);
    }
    m_firstFrame.setInterval(kFirstFrameMs);
    m_settle.setInterval(kSettleMs);
    m_watchdog.setInterval(kWatchdogMs);
    m_mainFrame.setInterval(kMainFrameMs);
    connect(&m_firstFrame, &QTimer::timeout, this, &LaunchSplash::play);
    connect(&m_settle, &QTimer::timeout, this, &LaunchSplash::sendAssembled);
    connect(&m_watchdog, &QTimer::timeout, this, &LaunchSplash::sendAssembled);
    connect(&m_mainFrame, &QTimer::timeout, this, &LaunchSplash::dismiss);
}

LaunchSplash::~LaunchSplash() {
    restoreQuitOnLastWindowClosed();
    m_view.reset();
}

QQuickWindow* LaunchSplash::window() const {
    return m_view.get();
}

bool LaunchSplash::show() {
    if (m_view) {
        return true;
    }
    m_view = std::make_unique<QQuickView>();
    m_view->setTitle(QStringLiteral("Wabcut"));
    // See-through: no frame, no shadow, an alpha channel and a transparent clear colour (before the window exists).
    m_view->setFlags(Qt::SplashScreen | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::NoDropShadowWindowHint);
    QSurfaceFormat format = m_view->requestedFormat();
    format.setAlphaBufferSize(8);
    m_view->setFormat(format);
    m_view->setColor(Qt::transparent);
    m_view->setResizeMode(QQuickView::SizeRootObjectToView);
    m_view->setInitialProperties({{QStringLiteral("backdrop"), false},
                                  {QStringLiteral("holdAtEnd"), true},
                                  {QStringLiteral("markSize"), kMarkSize}});
    m_view->loadFromModule("Wabcut", "LaunchIntro");
    m_intro = m_view->rootObject();
    if (m_view->status() != QQuickView::Ready || !m_intro) {
        qWarning("Wabcut: the launch intro did not load");
        m_view.reset();
        return false;
    }
    connect(m_intro, SIGNAL(assembled()), this, SLOT(onAssembled()));
    connect(m_intro, SIGNAL(finished()), this, SLOT(onFinished()));
    // Without this, Qt would end the process when the scene graph cannot start; the main window then loads anyway (and
    // its own fallback, RhiFallbackHandler, deals with the graphics API).
    connect(m_view.get(), &QQuickWindow::sceneGraphError, this,
            [this](QQuickWindow::SceneGraphError, const QString& message) { fail(message); });

    // In the middle of where the main window will open, else of the primary screen.
    QScreen* screen = m_centre.isNull() ? nullptr : QGuiApplication::screenAt(m_centre);
    QPoint centre = m_centre;
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
        centre = screen ? screen->availableGeometry().center() : QPoint(kWindowSize / 2, kWindowSize / 2);
    }
    if (screen) {
        m_view->setScreen(screen);
    }
    m_view->setGeometry(QRect(centre - QPoint(kWindowSize / 2, kWindowSize / 2), QSize(kWindowSize, kWindowSize)));

    // Until the main window shows, the splash is the only window: hiding it must not end Wabcut.
    m_quitOnLastWindowClosed = QGuiApplication::quitOnLastWindowClosed();
    QGuiApplication::setQuitOnLastWindowClosed(false);
    m_holdsQuit = true;

    // Played from its first frame, so the first part of the animation is not spent while the window starts up.
    connect(m_view.get(), &QQuickWindow::frameSwapped, this, &LaunchSplash::play, kQueuedOnce);
    m_firstFrame.start();
    m_watchdog.start();
    m_view->show();
    return true;
}

void LaunchSplash::play() {
    if (m_playing || !m_intro) {
        return;
    }
    m_playing = true;
    m_firstFrame.stop();
    QMetaObject::invokeMethod(m_intro, "play");
}

void LaunchSplash::skip() {
    if (m_intro && m_intro->property("playing").toBool()) {
        QMetaObject::invokeMethod(m_intro, "skip");
    }
}

void LaunchSplash::onAssembled() {
    if (m_assembledSent || m_settle.isActive()) {
        return;
    }
    // The main window loads on this thread right after: first let the complete mark reach the screen.
    if (m_view) {
        connect(m_view.get(), &QQuickWindow::frameSwapped, this, &LaunchSplash::sendAssembled, kQueuedOnce);
        m_view->update();
    }
    m_settle.start();
}

void LaunchSplash::sendAssembled() {
    if (m_assembledSent) {
        return;
    }
    m_assembledSent = true;
    m_settle.stop();
    m_watchdog.stop();
    emit assembled();
}

void LaunchSplash::dismiss() {
    if (m_dismissed || m_finishedSent) {
        return;
    }
    m_dismissed = true;
    m_mainFrame.stop();
    if (m_intro && m_intro->property("playing").toBool()) {
        QMetaObject::invokeMethod(m_intro, "dismiss"); // fades out, then finished()
    } else {
        onFinished();
    }
}

void LaunchSplash::dismissOnFirstFrame(QQuickWindow* window) {
    if (m_dismissed || m_finishedSent) {
        return;
    }
    if (!window) {
        dismiss();
        return;
    }
    connect(window, &QQuickWindow::frameSwapped, this, &LaunchSplash::dismiss, kQueuedOnce);
    m_mainFrame.start();
}

void LaunchSplash::fail(const QString& why) {
    qWarning("Wabcut: the launch intro stops: %s", qPrintable(why));
    m_firstFrame.stop();
    if (m_view) {
        m_view->hide();
    }
    onFinished();
}

void LaunchSplash::onFinished() {
    if (m_finishedSent) {
        return;
    }
    m_finishedSent = true;
    m_firstFrame.stop();
    m_mainFrame.stop();
    if (m_view) {
        m_view->hide();
    }
    // Out of the intro's own call (its animation or signal): sendAssembled() may load the main window, and the main
    // window is up before quitting on its close is allowed again.
    QMetaObject::invokeMethod(
        this,
        [this] {
            sendAssembled();
            restoreQuitOnLastWindowClosed();
            emit finished();
        },
        Qt::QueuedConnection);
}

void LaunchSplash::restoreQuitOnLastWindowClosed() {
    if (!m_holdsQuit) {
        return;
    }
    m_holdsQuit = false;
    QGuiApplication::setQuitOnLastWindowClosed(m_quitOnLastWindowClosed);
}

} // namespace wabcut::app
