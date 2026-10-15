#pragma once

// The interface size preference (M5j, accessibility): 100, 125 or 150 % (Preferences.uiScale, saved as
// appearance/uiScale). Qt Quick scales the whole window by QT_SCALE_FACTOR, which must be set before the
// QGuiApplication exists, so main() applies the saved size at launch and a change takes effect on the next start.

#include <QObject>
#include <QtQml/qqmlregistration.h>

class QSettings;

namespace wabcut::app {

/// The saved size in percent; anything other than 125 or 150 is 100.
int savedUiScale(const QSettings& settings);

/// Before QGuiApplication: sets QT_SCALE_FACTOR from the saved size unless the environment already sets it. The
/// organization and application names must be set first (QSettings finds the store with them). Returns the percent
/// applied (100: nothing changed).
int applySavedUiScale();

/// The interface size this process runs at, in percent: what applySavedUiScale() applied, or the QT_SCALE_FACTOR the
/// environment set (rounded), or 100.
int appliedUiScale();

/// QML singleton `UiScaleInfo`: the applied size, so the Appearance popover shows what is in effect, not a raw stored
/// value (review P2-47).
class UiScaleInfo : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(int applied READ applied CONSTANT FINAL)

public:
    explicit UiScaleInfo(QObject* parent = nullptr) : QObject(parent) {}
    int applied() const { return appliedUiScale(); }
};

} // namespace wabcut::app
