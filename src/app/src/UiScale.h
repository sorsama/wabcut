#pragma once

// The interface size preference (M5j, accessibility): 90, 100, 125 or 150 % (Preferences.uiScale, saved as
// appearance/uiScale; 90 since P1 #47, for small laptop screens). Qt Quick scales the whole window by QT_SCALE_FACTOR,
// which must be set before the QGuiApplication exists, so main() applies the saved size at launch and a change takes
// effect on the next start (Settings > Appearance offers Restart now: AppShell.requestRestart()).

#include <QObject>
#include <QSize>
#include <QtQml/qqmlregistration.h>

#include <array>

class QSettings;
class QWindow;

namespace wabcut::app {

/// The sizes Settings offers, in percent, smallest first.
inline constexpr std::array<int, 4> kUiScales{90, 100, 125, 150};

/// The main window's smallest useful size in logical pixels at 100 % (Theme.windowMinWidth / windowMinHeight): the
/// window's minimum is this, cut to the screen's work area (P1 #47), and a size whose screen is smaller is not offered.
inline constexpr QSize kWindowMinimum{1100, 700};
/// The main window's first-run size (Theme.windowDefaultWidth / windowDefaultHeight); main() cuts it to the work area.
inline constexpr QSize kWindowDefaultSize{1600, 960};

/// The saved size in percent; anything other than 90, 125 or 150 is 100.
int savedUiScale(const QSettings& settings);

/// Before QGuiApplication: sets QT_SCALE_FACTOR from the saved size unless the environment already sets it. The
/// organization and application names must be set first (QSettings finds the store with them). Returns the percent
/// applied (100: nothing changed).
int applySavedUiScale();

/// The interface size this process runs at, in percent: what applySavedUiScale() applied, or the QT_SCALE_FACTOR the
/// environment set (rounded), or 100.
int appliedUiScale();

/// Whether the interface size `candidate` % leaves room for `minimum` on a work area of `available` logical pixels
/// measured while running at `applied` % (P1 #47): available x applied / candidate must hold the minimum both ways.
bool uiScaleFits(QSize available, int applied, int candidate, QSize minimum = kWindowMinimum);

/// The work area (QScreen::availableGeometry) of `window`'s screen in this process's logical pixels; the primary
/// screen's without a window; empty without a screen.
QSize availableScreenSize(const QWindow* window);

/// QML singleton `UiScaleInfo`: the applied size, so the Appearance popover shows what is in effect, not a raw stored
/// value (review P2-47), and (P1 #47) the screen room the main window and the size choices are fitted to.
class UiScaleInfo : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(int applied READ applied CONSTANT FINAL)
    /// Bumped when a screen comes or goes or its work area changes (the taskbar moved): bindings over
    /// availableSize() and fits() read it to update.
    Q_PROPERTY(int screenRevision READ screenRevision NOTIFY screensChanged FINAL)

public:
    explicit UiScaleInfo(QObject* parent = nullptr);
    int applied() const { return appliedUiScale(); }
    int screenRevision() const { return m_revision; }

    /// availableScreenSize(window).
    Q_INVOKABLE QSize availableSize(QWindow* window) const;
    /// uiScaleFits(availableSize(window), applied(), percent): whether that size leaves room for the window here.
    Q_INVOKABLE bool fits(int percent, QWindow* window) const;

signals:
    void screensChanged();

private:
    void watchScreens();
    void bump();
    int m_revision = 0;
};

} // namespace wabcut::app
