#pragma once

// UiProfile: startup and panel-creation timings of the UI (singleton `UiProfile`, M5n). Always recorded (cheap);
// printed only when WABCUT_UI_PROFILE is set ("ui-profile: <what> <ms>" on the wabcut.ui.profile category). The app's
// own start-up probe (WABCUT_PERF_STARTUP, P.1) is separate and lives in main.cpp.
//
//   * mark(what): a milestone, in ms since the process started (since this library's static initialisation, which is
//     the process start for practical purposes): "Main.qml completed", "first frame", "panels ready".
//   * span(what, ms): a duration, e.g. how long a lazily created panel took to load ("panel essential 12.4").
//   * entries: [{ what, ms, kind: "mark" | "span" }] in the order they came, for tests and the report.
//   * backgroundLoading(): the engine creates objects in the background (it has an incubation controller: the app's
//     QQmlApplicationEngine window). Main.qml defers its inactive panels only then; a plain QQmlEngine (the tests'
//     fixture, the e2e suites) gets every panel up front, as before; so does WABCUT_EAGER_PANELS (for comparisons).

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class UiProfile : public QObject {
    Q_OBJECT
    /// WABCUT_UI_PROFILE is set: the timings are printed as they come.
    Q_PROPERTY(bool enabled READ enabled CONSTANT FINAL)
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged FINAL)

public:
    explicit UiProfile(QObject* parent = nullptr);
    static UiProfile* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    bool enabled() const { return m_enabled; }
    QVariantList entries() const { return m_entries; }

    /// Milliseconds since the process started.
    Q_INVOKABLE double now() const;
    /// A milestone at now().
    Q_INVOKABLE void mark(const QString& what);
    /// A duration in ms.
    Q_INVOKABLE void span(const QString& what, double ms);
    Q_INVOKABLE void clear();
    Q_INVOKABLE bool backgroundLoading() const;

signals:
    void entriesChanged();

private:
    void add(const QString& what, double ms, const QString& kind);

    bool m_enabled = false;
    QVariantList m_entries;
    QPointer<QObject> m_engine; ///< the QQmlEngine
};

struct UiProfileSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::UiProfile)
    QML_NAMED_ELEMENT(UiProfile)
    QML_SINGLETON
public:
    static UiProfile* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return UiProfile::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
