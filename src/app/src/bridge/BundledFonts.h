#pragma once

// BundledFonts: the Wabcut type system (design language v2), for QML (singleton `BundledFonts`, read by Theme).
//
//   UI text        IBM Plex Sans (+ IBM Plex Sans Thai for Thai)        Theme.fontSans
//   Display/titles Bai Jamjuree (Thai + Latin)                          Theme.fontDisplay
//   Numbers        IBM Plex Mono (+ IBM Plex Sans Thai)                 Theme.fontMono
//
// The fonts are bundled in the QML module's resources (resources/fonts, SIL OFL 1.1; THIRD-PARTY-NOTICES.md) and
// registered once per process with QFontDatabase, as FontLoader would. Thai text set in Plex Sans or Plex Mono falls
// back to IBM Plex Sans Thai (QFont substitutions) before any system font, so vowels and tone marks stack the same
// way everywhere. If a family fails to load, its property names a system font instead (Segoe UI / SF Pro / Inter).

#include <QObject>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class BundledFonts : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString sans READ sans CONSTANT FINAL)
    Q_PROPERTY(QString thai READ thai CONSTANT FINAL)
    Q_PROPERTY(QString mono READ mono CONSTANT FINAL)
    Q_PROPERTY(QString display READ display CONSTANT FINAL)
    /// True when every bundled family loaded (false: system fallbacks in use).
    Q_PROPERTY(bool bundled READ bundled CONSTANT FINAL)

public:
    explicit BundledFonts(QObject* parent = nullptr);
    static BundledFonts* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    /// Registers the bundled fonts and the Thai substitutions (idempotent). main() and the tests call it before the
    /// QML loads; the singleton calls it too.
    static void registerAll();
    /// The families that loaded, for diagnostics and tests.
    static QStringList loadedFamilies();

    static QString sans();
    static QString thai();
    static QString mono();
    static QString display();
    static bool bundled();
};

/// QML: the singleton `BundledFonts`.
struct BundledFontsSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::BundledFonts)
    QML_NAMED_ELEMENT(BundledFonts)
    QML_SINGLETON
public:
    static BundledFonts* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return BundledFonts::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
