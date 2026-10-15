#include "UiScale.h"

#include <QByteArray>
#include <QSettings>
#include <QtGlobal>

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
    return scale == 125 || scale == 150 ? scale : 100;
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

} // namespace wabcut::app
