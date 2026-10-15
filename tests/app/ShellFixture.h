#pragma once

// Shared fixture for the UI shell tests: one QGuiApplication per process (Catch2 listener), Qt message
// capture, and `Shell`, the real Main.qml created hidden.
//
// Offscreen approach: the Main window is created with visible=false and never shown, so nothing appears on
// the desktop. QQuickWindow::grabWindow() on a window that is not exposed renders one frame offscreen with the
// platform's real QRhi backend (D3D11 on Windows, Metal on macOS, Vulkan/OpenGL on Linux) and reads it back
// (QSGRhiSupport::grabOffscreen). That keeps MultiEffect icon tinting and shadows identical to the app.
// On a Linux machine without a display the tests fall back to QT_QPA_PLATFORM=offscreen, where Qt Quick
// uses its software renderer (no shader effects); run them under a virtual display there for real screenshots.

#include <QColor>
#include <QImage>
#include <QObject>
#include <QPointF>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <string>

namespace wabcut::test {

inline constexpr int kShotWidth = 1600;
inline constexpr int kShotHeight = 960;

// Scale factor the process was started with (tags [hidpi125] / [hidpi150]).
double appScaleFactor();

// RHI backend name the policy requests on this platform ("D3D11", "Metal", "Vulkan"/"OpenGL").
QString requestedRhiBackend();

// ---- Qt message capture ----
void clearMessages();
QStringList takeMessages();
std::string join(const QStringList& lines);

// ---- Helpers ----
void spin(int milliseconds);
// Polls `condition` (every 10 ms) until it holds. The bound is generous on purpose: under machine load (parallel
// builds, other suites) animations and timers run late, and a wait that is too tight makes a test flaky.
bool waitFor(const std::function<bool()>& condition, int timeoutMs = 10000);
bool sameColor(const QColor& a, const QColor& b, int tolerance = 2);
std::string describe(const QColor& c);
QString outputDir();                       // <build>/test-output/ui (created)
void writeOutputFile(const QString& name, const QString& text);
void resetSettings();                      // clears the per-process INI settings
QColor pixelAt(const QImage& image, const QQuickItem* item, QPointF local, double dpr);

// The real Main.qml, created hidden. Members are declared so the window dies before its engine.
struct Shell {
    QQmlEngine engine;
    std::unique_ptr<QQuickWindow> window;
    QObject* theme = nullptr;
    QObject* preferences = nullptr;
    QStringList errors;

    Shell();

    // A window that is never shown receives no resize events, so also size the content item the way
    // QQuickWindow::resizeEvent() would; otherwise the scene keeps its 0x0 layout.
    void resize(int width, int height);

    // Items declared in Main.qml are QObject children of the window (their visual parent is contentItem).
    QQuickItem* item(const char* objectName) const;
    QObject* object(const char* objectName) const;

    QColor themeColor(const char* token) const;
    void setAppearance(bool dark, const char* accent);

    // Waits until the animated palette has settled on the current dark/accent targets, then lets the remaining
    // state transitions (durState / durCollapse) finish.
    bool settle(bool dark, const QColor& accentFill);

    // Renders one frame offscreen (QSGRhiSupport::grabOffscreen: a temporary QRhi on the real backend) and
    // reads it back. That path tears the scene graph down after every grab, and QtQuick.Shapes items are not
    // rebuilt by the next offscreen grab of the same window, so screenshot tests grab each window once.
    QImage grab() const;
};

} // namespace wabcut::test
