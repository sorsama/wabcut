#include "UiScale.h"

#include <QByteArray>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QWindow>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace wabcut::app {

namespace {
int g_applied = 0; // 0 = applySavedUiScale() has not run

int environmentScale() {
    bool ok = false;
    const double factor = qEnvironmentVariable("QT_SCALE_FACTOR").toDouble(&ok);
    return ok && factor > 0 ? static_cast<int>(std::lround(factor * 100.0)) : 100;
}
} // namespace

int savedUiScale(const QSettings& settings) {
    const int scale = settings.value(QStringLiteral("appearance/uiScale"), 100).toInt();
    return std::ranges::find(kUiScales, scale) != kUiScales.end() ? scale : 100;
}

int applySavedUiScale() {
    if (qEnvironmentVariableIsSet("QT_SCALE_FACTOR")) {
        g_applied = environmentScale();
        return 100; // the user (or a test) chose a factor already
    }
    const QSettings settings;
    const int scale = savedUiScale(settings);
    if (scale != 100)
        qputenv("QT_SCALE_FACTOR", QByteArray::number(scale / 100.0));
    g_applied = scale;
    return scale;
}

int appliedUiScale() {
    if (g_applied > 0)
        return g_applied;
    return qEnvironmentVariableIsSet("QT_SCALE_FACTOR") ? environmentScale() : 100;
}

bool uiScaleFits(QSize available, int applied, int candidate, QSize minimum) {
    if (available.isEmpty() || applied <= 0 || candidate <= 0)
        return true; // nothing known: offer it
    // At `candidate` % the same screen holds available x applied / candidate logical pixels.
    const auto room = [&](int length) { return static_cast<qint64>(length) * applied / candidate; };
    return room(available.width()) >= minimum.width() && room(available.height()) >= minimum.height();
}

QSize availableScreenSize(const QWindow* window) {
    const QScreen* screen = window && window->screen() ? window->screen() : QGuiApplication::primaryScreen();
    return screen ? screen->availableGeometry().size() : QSize();
}

UiScaleInfo::UiScaleInfo(QObject* parent) : QObject(parent) {
    watchScreens();
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        const auto changed = [this] {
            watchScreens();
            bump();
        };
        connect(app, &QGuiApplication::screenAdded, this, changed);
        connect(app, &QGuiApplication::screenRemoved, this, changed);
    }
}

void UiScaleInfo::watchScreens() {
    for (QScreen* screen : QGuiApplication::screens()) {
        connect(screen, &QScreen::availableGeometryChanged, this, &UiScaleInfo::bump, Qt::UniqueConnection);
    }
}

void UiScaleInfo::bump() {
    ++m_revision;
    emit screensChanged();
}

QSize UiScaleInfo::availableSize(QWindow* window) const {
    return availableScreenSize(window);
}

bool UiScaleInfo::fits(int percent, QWindow* window) const {
    return uiScaleFits(availableSize(window), applied(), percent);
}

} // namespace wabcut::app
