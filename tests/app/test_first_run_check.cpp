// I.7 first-run performance check (docs/release.md "First-run performance check"): PerformanceCheck runs the probe in
// the background, keeps the report in the settings, applies its recommendation only when asked (through MediaReadiness)
// and is offered once; then the dialog in the real window (FirstRunCheckDialog, Main.qml): offered after the onboarding,
// run, summarised, applied.

#include "ShellFixture.h"

#include "PerformanceCheck.h"

#include <wabcut/media/GpuInfo.h>

#include <catch2/catch_test_macros.hpp>

#include <QQuickItem>
#include <QSettings>

#include <filesystem>
#include <functional>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace fs = std::filesystem;

namespace {

/// A quick check: small samples, no audio device (the CI machines have none), a generous budget.
editor::ProbeOptions quickOptions(const char* name) {
    editor::ProbeOptions options;
    options.workDir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "performance" / name;
    std::error_code ec;
    fs::remove_all(options.workDir, ec);
    options.width = 320;
    options.height = 180;
    options.frames = 6;
    options.budgetSeconds = 60.0;
    options.audio = false;
    return options;
}

/// A control of a dialog (a Popup): its QObject children, else the visual tree under its content item (Repeater
/// delegates are only there).
QObject* inDialog(QObject* dialog, const char* name) {
    const QString wanted = QString::fromLatin1(name);
    if (QObject* found = dialog->findChild<QObject*>(wanted)) {
        return found;
    }
    const std::function<QObject*(QQuickItem*)> walk = [&](QQuickItem* item) -> QObject* {
        if (!item) {
            return nullptr;
        }
        if (item->objectName() == wanted) {
            return item;
        }
        for (QQuickItem* child : item->childItems()) {
            if (QObject* found = walk(child)) {
                return found;
            }
        }
        return nullptr;
    };
    return walk(dialog->property("contentItem").value<QQuickItem*>());
}

QString recommended(const PerformanceCheck& check, const char* key) {
    return check.recommendation().value(QString::fromLatin1(key)).toString();
}

} // namespace

TEST_CASE("First-run check: runs in the background, keeps the report, applies only when asked", "[app][performance]") {
    resetSettings();
    PerformanceCheck check(nullptr);
    check.setOptions(quickOptions("bridge"));
    CHECK(check.offerAtStartup());
    CHECK_FALSE(check.hasReport());

    check.start();
    CHECK(check.running());
    CHECK_FALSE(check.offerAtStartup()); // running it counts as offered
    REQUIRE(waitFor([&] { return !check.running(); }, 120'000));
    REQUIRE(check.hasReport());
    CHECK_FALSE(check.partial());
    CHECK(check.progress() == 1.0);

    QStringList keys;
    for (const QVariant& row : check.rows()) {
        keys << row.toMap().value(QStringLiteral("key")).toString();
    }
    INFO(keys.join(QLatin1Char(',')).toStdString());
    for (const char* key : {"H.264", "HEVC", "ProRes", "encoders", "graphics", "audio", "cpu"}) {
        CHECK(keys.contains(QString::fromLatin1(key)));
    }
    CHECK_FALSE(check.reasons().isEmpty());
    CHECK_FALSE(recommended(check, "text").isEmpty());
    // Issue #10: which GPU and why (with a GPU in the computer).
    if (const media::GpuSelection gpu = media::selectedGpu(); gpu.adapter) {
        CHECK(keys.contains(QStringLiteral("gpu")));
        CHECK(check.report()->gpuName == gpu.adapter->name);
        CHECK(check.report()->gpuReason == std::string(media::gpuReasonKey(gpu.reason)));
        CHECK_FALSE(check.report()->gpuNames.empty());
    }

    // Not applied until asked; then the media-readiness settings are the recommendation.
    QSettings settings;
    CHECK(settings.value(QStringLiteral("performance/report")).toString().contains(QStringLiteral("recommendation")));
    check.apply();
    CHECK(check.applied());
    CHECK(settings.value(QStringLiteral("proxies/policy")).toString() == recommended(check, "proxyPolicy"));
    CHECK(settings.value(QStringLiteral("proxies/format")).toString() == recommended(check, "proxyFormat"));
    CHECK(settings.value(QStringLiteral("proxies/divisor")).toInt() == check.recommendation().value(QStringLiteral("proxyDivisor")).toInt());
    CHECK(settings.value(QStringLiteral("preview/quality")).toString() == recommended(check, "previewQuality"));

    // The next start shows the same report and does not offer the check again.
    PerformanceCheck next(nullptr);
    REQUIRE(next.hasReport());
    CHECK(next.report() == check.report());
    CHECK_FALSE(next.reportTime().isEmpty());
    CHECK_FALSE(next.offerAtStartup());
    CHECK(next.applied());
}

TEST_CASE("First-run check: Not now is remembered, and Stop keeps what was measured", "[app][performance]") {
    resetSettings();
    {
        PerformanceCheck check(nullptr);
        check.skip();
        CHECK_FALSE(check.offerAtStartup());
    }
    resetSettings();
    PerformanceCheck check(nullptr);
    check.setOptions(quickOptions("stop"));
    check.start();
    check.cancel();
    REQUIRE(waitFor([&] { return !check.running(); }, 60'000));
    REQUIRE(check.hasReport());
    CHECK(check.report()->cancelled);
    CHECK(check.partial());
}

TEST_CASE("First-run check: offered after the onboarding in the window, run, summarised and applied",
          "[app][performance][qml]") {
    resetSettings();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    auto* check = shell.engine.singletonInstance<PerformanceCheck*>("Wabcut", "PerformanceCheck");
    REQUIRE(check);
    check->setOptions(quickOptions("dialog"));
    QObject* dialog = shell.object("firstRunCheckDialog");
    REQUIRE(dialog);

    // Not offered by the tests' start (no launch Home screen); offered once when due and the onboarding closes.
    spin(800);
    CHECK_FALSE(dialog->property("opened").toBool());
    shell.window->setProperty("performanceCheckDue", true);
    QObject* onboarding = shell.object("onboarding");
    REQUIRE(onboarding);
    QMetaObject::invokeMethod(onboarding, "dialogClosed");
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
    CHECK(dialog->property("stage").toString() == QStringLiteral("offer"));
    CHECK_FALSE(shell.window->property("performanceCheckDue").toBool());
    for (const char* button : {"performanceSkip", "performanceStart"}) {
        INFO(button);
        auto* item = qobject_cast<QQuickItem*>(inDialog(dialog, button));
        REQUIRE(item);
        CHECK(item->isVisible());
    }

    QMetaObject::invokeMethod(inDialog(dialog, "performanceStart"), "clicked");
    CHECK(dialog->property("stage").toString() == QStringLiteral("running"));
    REQUIRE(waitFor([&] { return dialog->property("stage").toString() == QStringLiteral("result"); }, 120'000));
    CHECK(inDialog(dialog, "performanceRow_H.264") != nullptr);
    CHECK(inDialog(dialog, "performanceRow_encoders") != nullptr);
    QObject* recommendation = inDialog(dialog, "performanceRecommendation");
    REQUIRE(recommendation);
    CHECK_FALSE(recommendation->property("text").toString().isEmpty());

    // "Use these settings": MediaReadiness (Settings > Media & cache) now has them.
    QObject* readiness = shell.engine.singletonInstance<QObject*>("Wabcut", "MediaReadiness");
    REQUIRE(readiness);
    auto* apply = qobject_cast<QQuickItem*>(inDialog(dialog, "performanceApply"));
    REQUIRE(apply);
    if (apply->isVisible()) { // already the current settings otherwise
        QMetaObject::invokeMethod(apply, "clicked");
    } else {
        QMetaObject::invokeMethod(inDialog(dialog, "performanceKeep"), "clicked");
    }
    REQUIRE(waitFor([&] { return !dialog->property("opened").toBool(); }));
    CHECK(check->applied());
    CHECK(readiness->property("policy").toString() == recommended(*check, "proxyPolicy"));
    CHECK(readiness->property("previewQuality").toString() == recommended(*check, "previewQuality"));
    CHECK(readiness->property("proxyDivisor").toInt() == check->recommendation().value(QStringLiteral("proxyDivisor")).toInt());

    // Settings > Media & cache opens it again, on the last report.
    QMetaObject::invokeMethod(check, "openCheck");
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
    CHECK(dialog->property("stage").toString() == QStringLiteral("result"));
    QMetaObject::invokeMethod(dialog, "close");
    REQUIRE(waitFor([&] { return !dialog->property("opened").toBool(); }));
    resetSettings();
}
