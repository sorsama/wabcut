// Theme switching at runtime (animated), reduce motion, persistence of every preference, and the v1 → v2 accent
// migration (design language v2 "Ultraviolet Cut").

#include "ShellFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <QElapsedTimer>
#include <QSettings>

using namespace wabcut::test;

namespace {

QVariant stored(const char* group, const char* key) {
    QSettings settings;
    settings.beginGroup(QLatin1String(group));
    return settings.value(QLatin1String(key));
}

// A grab can fail transiently while the GPU is busy with other work (the suite runs beside builds): try again once.
QImage grabTwice(const Shell& shell) {
    QImage image = shell.grab();
    if (image.isNull()) {
        spin(100);
        image = shell.grab();
    }
    return image;
}

} // namespace

TEST_CASE("Theme switches dark/light and accent at runtime with animated colors", "[app][theme]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    REQUIRE(shell.theme != nullptr);

    const QColor darkCanvas(0x00, 0x00, 0x00); // matte black (the user's choice)
    const QColor lightCanvas(0xED, 0xED, 0xF0);
    REQUIRE(shell.theme->property("dark").toBool());
    REQUIRE(sameColor(shell.themeColor("canvas"), darkCanvas, 0));
    REQUIRE(sameColor(shell.themeColor("accentFill"), QColor(0x9B, 0x6C, 0xFF), 0)); // design language v3 violet
    REQUIRE(sameColor(shell.themeColor("accentOnFill"), QColor(0x00, 0x00, 0x00), 0)); // black on the accent
    REQUIRE(shell.theme->property("accentName").toString() == QStringLiteral("ultraviolet"));

    SECTION("dark → light cross-fades") {
        // What is checked does not depend on how fast the machine is: the switch does not jump (the value right
        // after the change is still the old one, before any animation frame), and it ends on the light palette. The
        // fade's length is the token (durState), not a measured time.
        CHECK(shell.theme->property("durState").toInt() > 0);
        shell.theme->setProperty("dark", false);
        CHECK(sameColor(shell.themeColor("canvas"), darkCanvas, 0));
        REQUIRE(waitFor([&] { return sameColor(shell.themeColor("canvas"), lightCanvas, 0); }));
        CHECK(sameColor(shell.themeColor("textPrimary"), QColor(0x0B, 0x0B, 0x0C), 0));
        CHECK(sameColor(shell.themeColor("accentStroke"), QColor(0x6A, 0x3D, 0xE8), 0)); // a deeper violet on light
    }

    SECTION("the monitor surround stays dark neutral in both themes") {
        const QColor surround = shell.themeColor("monitorSurround");
        shell.theme->setProperty("dark", false);
        REQUIRE(waitFor([&] { return sameColor(shell.themeColor("canvas"), lightCanvas, 0); }));
        CHECK(sameColor(shell.themeColor("monitorSurround"), surround, 0));
        CHECK(sameColor(surround, QColor(0x00, 0x00, 0x00), 0));
    }

    SECTION("accent switches to phosphor, the lime (ink on its fill)") {
        shell.theme->setProperty("accentName", QStringLiteral("phosphor"));
        REQUIRE(waitFor([&] { return sameColor(shell.themeColor("accentFill"), QColor(0xD6, 0xF5, 0x4C), 0); }));
        REQUIRE(waitFor([&] { return sameColor(shell.themeColor("accentOnFill"), QColor(0x00, 0x00, 0x00), 0); }));
        CHECK(sameColor(shell.themeColor("accentStroke"), QColor(0xD6, 0xF5, 0x4C), 0));
        CHECK(sameColor(shell.themeColor("beamEnd"), QColor(0xD6, 0xF5, 0x4C), 0)); // v3: no gradient
    }

    SECTION("v1 accent names still resolve") {
        shell.theme->setProperty("accentName", QStringLiteral("cyan"));
        REQUIRE(waitFor([&] { return sameColor(shell.themeColor("accentFill"), QColor(0x4D, 0x8D, 0xFF), 0); }));
    }

    SECTION("reduce motion zeroes durations but state changes stay visible") {
        shell.theme->setProperty("reduceMotion", true);
        CHECK(shell.theme->property("durState").toInt() == 0);
        CHECK(shell.theme->property("durPress").toInt() == 0);
        CHECK(shell.theme->property("durCollapse").toInt() == 0);
        CHECK(shell.theme->property("durSpark").toInt() == 0);
        CHECK(shell.theme->property("durSlice").toInt() == 120); // the slice wipe becomes a short cross-fade
        CHECK(shell.theme->property("pressScale").toDouble() == 1.0);
        CHECK(shell.theme->property("liftOffset").toInt() == 0);
        shell.theme->setProperty("dark", false);
        CHECK(sameColor(shell.themeColor("canvas"), lightCanvas, 0)); // immediate, no cross-fade
    }

    SECTION("preferences persist through QtCore Settings") {
        shell.theme->setProperty("dark", false);
        shell.theme->setProperty("accentName", QStringLiteral("ice"));
        shell.theme->setProperty("reduceMotion", true);
        shell.preferences->setProperty("nativeFrame", true);
        // QtCore Settings batches writes on a short timer.
        CHECK(waitFor([&] { return stored("appearance", "accent").toString() == QStringLiteral("ice"); }));
        CHECK(waitFor([&] { return stored("window", "nativeFrame").toBool(); }));
        CHECK(stored("appearance", "dark").isValid());
        CHECK_FALSE(stored("appearance", "dark").toBool());
        CHECK(stored("appearance", "reduceMotion").toBool());
    }

    // The window still renders after the switch.
    CHECK_FALSE(grabTwice(shell).isNull());

    const QStringList messages = takeMessages();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Saved preferences are applied on start-up without animating; v1 accents are migrated", "[app][theme]") {
    resetSettings();
    {
        QSettings settings;
        settings.setValue(QStringLiteral("appearance/dark"), false);
        settings.setValue(QStringLiteral("appearance/accent"), QStringLiteral("orange")); // a v1 name
        settings.setValue(QStringLiteral("window/nativeFrame"), true);
        settings.sync();
    }
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    // Applied before color transitions were enabled: the light palette is there immediately.
    CHECK(sameColor(shell.themeColor("canvas"), QColor(0xED, 0xED, 0xF0), 0));
    // orange → sodium, and saved under the new name.
    CHECK(shell.theme->property("accentName").toString() == QStringLiteral("sodium"));
    CHECK(sameColor(shell.themeColor("accentFill"), QColor(0xFF, 0x7A, 0x2F), 0));

    CHECK(waitFor([&] { return stored("appearance", "accent").toString() == QStringLiteral("sodium"); }));
    CHECK(shell.preferences->property("nativeFrame").toBool());
    CHECK_FALSE((shell.window->flags() & Qt::FramelessWindowHint));

    const QStringList messages = takeMessages();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
}
