#include "TaskbarProgress.h"

#include <QGuiApplication>

#include <algorithm>
#include <cmath>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#endif

namespace wabcut::app {

namespace {

TaskbarProgress::Sink& testSink() {
    static TaskbarProgress::Sink sink;
    return sink;
}

bool knownMode(const QString& mode) {
    return mode == QLatin1String("none") || mode == QLatin1String("normal") || mode == QLatin1String("indeterminate") ||
           mode == QLatin1String("paused") || mode == QLatin1String("error");
}

/// Whether the mode shows a value (normal, paused, error); none and indeterminate do not.
bool showsValue(const QString& mode) {
    return mode == QLatin1String("normal") || mode == QLatin1String("paused") || mode == QLatin1String("error");
}

#ifdef _WIN32
/// The shell's taskbar list, made once on the GUI thread (Qt's GUI thread is an OLE STA on Windows). Null while it
/// cannot be made (no COM yet, no shell): the next change tries again.
ITaskbarList3* taskbarList() {
    static ITaskbarList3* list = nullptr;
    if (list)
        return list;
    ITaskbarList3* made = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&made))) || !made)
        return nullptr;
    if (FAILED(made->HrInit())) {
        made->Release();
        return nullptr;
    }
    list = made;
    return list;
}

/// Shows `mode` / `permille` on the button of `window`; false when the taskbar could not be told (try again later).
bool showOnTaskbar(QQuickWindow* window, const QString& mode, int permille) {
    if (!window || !window->handle() || QGuiApplication::platformName() != QLatin1String("windows"))
        return false; // no native window (yet), or not the Windows platform plugin (offscreen tests)
    ITaskbarList3* list = taskbarList();
    if (!list)
        return false;
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    TBPFLAG flag = TBPF_NOPROGRESS;
    if (mode == QLatin1String("normal"))
        flag = TBPF_NORMAL;
    else if (mode == QLatin1String("indeterminate"))
        flag = TBPF_INDETERMINATE;
    else if (mode == QLatin1String("paused"))
        flag = TBPF_PAUSED;
    else if (mode == QLatin1String("error"))
        flag = TBPF_ERROR;
    // The value first: setting it on a button without progress turns it to "normal", so the state follows it.
    if (showsValue(mode) && FAILED(list->SetProgressValue(hwnd, static_cast<ULONGLONG>(permille), 1000)))
        return false;
    return SUCCEEDED(list->SetProgressState(hwnd, flag));
}
#endif

} // namespace

TaskbarProgress::TaskbarProgress(QObject* parent) : QObject(parent) {}

TaskbarProgress::~TaskbarProgress() {
    if (m_shownMode == QLatin1String("none"))
        return;
    if (testSink().show) {
        testSink().show(QStringLiteral("none"), 0.0);
        return;
    }
#ifdef _WIN32
    if (m_window)
        showOnTaskbar(m_window, QStringLiteral("none"), 0);
#endif
}

void TaskbarProgress::setWindow(QQuickWindow* window) {
    if (m_window == window)
        return;
    if (m_visibleConnection)
        disconnect(m_visibleConnection);
    m_window = window;
    m_shownMode = QStringLiteral("none"); // another button: it shows no progress yet
    m_shownPermille = -1;
    if (window) {
        m_visibleConnection = connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
            if (!visible)
                return;
            m_shownMode = QStringLiteral("none"); // the button may only exist now, without progress
            m_shownPermille = -1;
            apply();
        });
    }
    emit windowChanged();
    apply();
}

void TaskbarProgress::setMode(const QString& mode) {
    const QString known = knownMode(mode) ? mode : QStringLiteral("none");
    if (known == m_mode)
        return;
    m_mode = known;
    emit modeChanged();
    apply();
}

void TaskbarProgress::setProgress(double progress) {
    const double clamped = std::isfinite(progress) ? std::clamp(progress, 0.0, 1.0) : 0.0;
    if (clamped == m_progress)
        return;
    m_progress = clamped;
    emit progressChanged();
    apply();
}

void TaskbarProgress::apply() {
    const int permille = showsValue(m_mode) ? static_cast<int>(std::lround(m_progress * 1000.0)) : -1;
    if (m_mode == m_shownMode && permille == m_shownPermille)
        return;
    if (testSink().show) {
        testSink().show(m_mode, m_progress);
    } else {
#ifdef _WIN32
        if (!showOnTaskbar(m_window, m_mode, std::max(0, permille)))
            return; // not told: the next change or the window's showing tries again
#else
        // No taskbar progress here: keep the state only.
#endif
    }
    m_shownMode = m_mode;
    m_shownPermille = permille;
}

bool TaskbarProgress::flash() {
    if (!m_window || m_window->isActive())
        return false;
    if (testSink().flash)
        testSink().flash();
    else
        m_window->alert(0);
    return true;
}

void TaskbarProgress::setSinkForTests(Sink sink) {
    testSink() = std::move(sink);
}

} // namespace wabcut::app
