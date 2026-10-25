#pragma once

// TaskbarProgress (0.4): the window's taskbar button shows progress (Windows: ITaskbarList3 through CoCreateInstance,
// shobjidl.h; no new dependency) and flashes when work ends while the window is not active (QWindow::alert, every OS).
// Other platforms keep the state (and the test sink) but make no taskbar calls. The taskbar is told only when the mode
// or the progress in permille changed, and again when the window becomes visible (its button may not exist before).
//
// QML: `TaskbarProgress { window: window; mode: ...; progress: ... }` and `flash()`. GUI thread only.

#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <functional>

namespace wabcut::app {

class TaskbarProgress : public QObject {
    Q_OBJECT
    QML_ELEMENT
    /// The window whose taskbar button shows the progress.
    Q_PROPERTY(QQuickWindow* window READ window WRITE setWindow NOTIFY windowChanged FINAL)
    /// "none" | "normal" | "indeterminate" | "paused" | "error" (anything else reads "none")
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged FINAL)
    /// 0..1 (normal, paused, error; clamped)
    Q_PROPERTY(double progress READ progress WRITE setProgress NOTIFY progressChanged FINAL)

public:
    explicit TaskbarProgress(QObject* parent = nullptr);
    ~TaskbarProgress() override; ///< back to "none"

    /// Flashes the button until the window is activated; false (nothing) while the window is active or without one.
    Q_INVOKABLE bool flash();

    struct Sink {
        std::function<void(const QString& mode, double progress)> show;
        std::function<void()> flash;
    };
    /// Tests: replaces the taskbar calls and alert(); {} restores them.
    static void setSinkForTests(Sink sink);

    QQuickWindow* window() const { return m_window; }
    void setWindow(QQuickWindow* window);
    QString mode() const { return m_mode; }
    void setMode(const QString& mode);
    double progress() const { return m_progress; }
    void setProgress(double progress);

signals:
    void windowChanged();
    void modeChanged();
    void progressChanged();

private:
    QPointer<QQuickWindow> m_window;
    QMetaObject::Connection m_visibleConnection;
    QString m_mode = QStringLiteral("none");
    double m_progress = 0.0;
    int m_shownPermille = -1; ///< what the taskbar shows now (apply only on change); -1: no value
    QString m_shownMode = QStringLiteral("none"); ///< a new button shows no progress
    void apply();
};

} // namespace wabcut::app
