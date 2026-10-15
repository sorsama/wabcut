#include "UiProfile.h"

#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QQmlEngine>
#include <QQmlIncubationController>
#include <QVariantMap>

namespace wabcut::app {

namespace {

Q_LOGGING_CATEGORY(lcUiProfile, "wabcut.ui.profile")

/// Started during static initialisation: the process start, for practical purposes.
const QElapsedTimer& processClock() {
    static const QElapsedTimer clock = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return clock;
}

[[maybe_unused]] const bool kClockStarted = (processClock(), true);

} // namespace

UiProfile::UiProfile(QObject* parent) : QObject(parent), m_enabled(!qEnvironmentVariableIsEmpty("WABCUT_UI_PROFILE")) {}

UiProfile* UiProfile::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* profile = new UiProfile;
    profile->m_engine = qmlEngine;
    return profile;
}

bool UiProfile::backgroundLoading() const {
    // WABCUT_EAGER_PANELS: every panel up front, as before M5n (the startup comparison runs the same build both ways).
    if (qEnvironmentVariableIsSet("WABCUT_EAGER_PANELS"))
        return false;
    const auto* engine = qobject_cast<const QQmlEngine*>(m_engine.data());
    return engine && engine->incubationController() != nullptr;
}

double UiProfile::now() const {
    return static_cast<double>(processClock().nsecsElapsed()) / 1.0e6;
}

void UiProfile::mark(const QString& what) {
    add(what, now(), QStringLiteral("mark"));
}

void UiProfile::span(const QString& what, double ms) {
    add(what, ms, QStringLiteral("span"));
}

void UiProfile::clear() {
    if (m_entries.isEmpty())
        return;
    m_entries.clear();
    emit entriesChanged();
}

void UiProfile::add(const QString& what, double ms, const QString& kind) {
    m_entries.append(QVariantMap{{QStringLiteral("what"), what}, {QStringLiteral("ms"), ms}, {QStringLiteral("kind"), kind}});
    if (m_enabled)
        qCInfo(lcUiProfile).noquote() << "ui-profile:" << kind << what << QString::number(ms, 'f', 1);
    emit entriesChanged();
}

} // namespace wabcut::app
