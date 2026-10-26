#pragma once

// LaunchSplash (design language v3, "Launch intro · Assemble"): when Wabcut starts, the echo mark assembles on the
// desktop in a frameless, see-through window of its own - only the mark shows, the way a game launcher's logo does -
// before the main window opens. main() shows it first and loads the main window once the mark is complete (assembled():
// loading keeps the GUI thread busy for a moment, and a finished mark has nothing left to animate), under it; when the
// main window has drawn its first frame the mark fades out above it (dismissOnFirstFrame()). A click or Esc completes
// the mark at once. The intro preference (Settings > Appearance, "appearance/launchIntro") and reduce motion
// ("appearance/reduceMotion") leave it out (wanted()); so does main() for the startup benchmark and the
// software-renderer restart. Its QML (onboarding/LaunchIntro.qml, `backdrop: false`, `holdAtEnd: true`) runs in an
// engine of its own: the main window's can be replaced while it shows (RhiFallbackHandler). Whatever goes wrong with it
// (no window, a scene graph that cannot start), assembled() still comes, so the main window always loads.

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QQuickItem>
#include <QTimer>

#include <memory>

class QQuickView;
class QQuickWindow;

namespace wabcut::app {

class LaunchSplash : public QObject {
    Q_OBJECT

public:
    /// The window's size and the mark's, in device-independent pixels.
    static constexpr int kWindowSize = 340;
    static constexpr int kMarkSize = 220;

    /// The saved preferences ask for the intro: it is on and motion is not reduced.
    static bool wanted();

    /// `centre`: where the main window will open (its middle); null = the middle of the primary screen.
    explicit LaunchSplash(QPoint centre = {}, QObject* parent = nullptr);
    ~LaunchSplash() override;

    /// Shows the window and plays the intro once it has drawn its first frame. False when the intro does not load: then
    /// nothing shows and no signal comes (load the main window straight away).
    bool show();
    /// Completes the mark now (a click or Esc does the same).
    void skip();
    /// Fades the mark out; finished() when it has gone.
    void dismiss();
    /// dismiss() once `window` has drawn its first frame (at once for none; at the latest after a few seconds).
    void dismissOnFirstFrame(QQuickWindow* window);

    QQuickWindow* window() const;
    /// The intro (null before show() or when it could not load).
    QQuickItem* intro() const { return m_intro; }
    bool isAssembled() const { return m_assembledSent; }
    bool isFinished() const { return m_finishedSent; }

signals:
    /// The complete mark is on screen (or there is no splash after all): load the main window now. Once.
    void assembled();
    /// The splash has gone (faded out, or failed). Once.
    void finished();

private slots:
    // The intro's signals (connected by name: it is QML).
    void onAssembled();
    void onFinished();

private:
    void play();
    void sendAssembled();
    void fail(const QString& why);
    void restoreQuitOnLastWindowClosed();

    QPoint m_centre;
    std::unique_ptr<QQuickView> m_view;
    QPointer<QQuickItem> m_intro;
    QTimer m_firstFrame; ///< plays anyway when no first frame comes
    QTimer m_settle;     ///< after assembled: the frame with the complete mark, or this
    QTimer m_watchdog;   ///< the main window loads at the latest after this, whatever the splash does
    QTimer m_mainFrame;  ///< dismissOnFirstFrame()'s limit
    bool m_playing = false;
    bool m_assembledSent = false;
    bool m_finishedSent = false;
    bool m_dismissed = false;
    bool m_quitOnLastWindowClosed = true;
    bool m_holdsQuit = false;
};

} // namespace wabcut::app
