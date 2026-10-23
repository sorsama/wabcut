// M5n startup performance (the lazy panels), offscreen; no window is shown. The start-up itself is measured by P.1's
// StartupProbe (WABCUT_PERF_STARTUP); these cover what it does not isolate:
//   * Each panel's own creation cost, created alone in the shell's engine (startup-panels.json), [perf].
//   * The lazy panels: an inactive browser panel or workspace panel is created when it is first shown (or preloaded
//     after the first frame), kept afterwards, and works once created.

#include "ShellFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQmlIncubationController>
#include <QQuickItem>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <vector>

using namespace wabcut::test;

namespace {

/// Creates objects in the background like the app's window does (QQmlApplicationEngine installs the window's
/// controller; the tests' plain engine has none, so Main.qml creates every panel up front there): a zero-interval timer
/// incubating a few ms per slice.
class TimerIncubationController : public QObject, public QQmlIncubationController {
public:
    TimerIncubationController() {
        m_timer.setInterval(0);
        QObject::connect(&m_timer, &QTimer::timeout, this, [this] {
            incubateFor(5);
            if (incubatingObjectCount() == 0)
                m_timer.stop();
        });
    }

protected:
    void incubatingObjectCountChanged(int count) override {
        if (count > 0 && !m_timer.isActive())
            m_timer.start();
    }

private:
    QTimer m_timer;
};

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

QQuickItem* namedIn(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = namedIn(child, name))
            return found;
    }
    return nullptr;
}

} // namespace

TEST_CASE("Startup: each panel's creation cost, created alone in the shell's engine; report", "[app][startup][perf]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    const char* panels[] = {"LibraryPanel",    "EffectsPanel",       "TranscriptPanel", "AudioToolsPanel",
                            "MarkersPanel",    "GeneratePanel",      "ShortsPanel",     "TakesPanel",
                            "EssentialSoundPanel", "ReviewPanel",    "ScopesPanel",     "InspectorPanel",
                            "CaptionStylePanel", "FrameInspectorPanel", "AudioMixerPanel", "ExportPanel",
                            "ColorPanel",      "TimelinePanel",      "MonitorPanel",    "SourceMonitorPanel",
                            "TopBar",          "AppMenuBar",         "HomeScreen",      "Onboarding",
                            "PaletteOverlay",  "AssistantPanel",     "KeyboardShortcutsDialog", "SpeedDurationDialog",
                            "FitClipDialog",   "UnsavedChangesDialog", "ImportReportDialog", "RelinkDialog",
                            "RecoveryDialog",  "CrashNoticeDialog",  "FirstRunCheckDialog", "WindowFrame"};
    QJsonObject costs;
    QStringList lines;
    for (const char* name : panels) {
        std::vector<double> samples;
        QString error;
        for (int i = 0; i < 3 && error.isEmpty(); ++i) {
            QQmlComponent component(&shell.engine);
            component.setData("import Wabcut\n" + QByteArray(name) + " { width: 600; height: 500 }\n", QUrl());
            QElapsedTimer clock;
            clock.start();
            std::unique_ptr<QObject> object(component.create());
            const double ms = static_cast<double>(clock.nsecsElapsed()) / 1e6;
            if (!object) { // a required property only Main.qml sets
                error = component.errorString().trimmed();
                break;
            }
            if (auto* item = qobject_cast<QQuickItem*>(object.get()))
                item->setParentItem(shell.window->contentItem());
            samples.push_back(ms);
            object.reset();
            spin(10);
        }
        if (!error.isEmpty()) {
            lines << QStringLiteral("%1 not created alone").arg(QString::fromLatin1(name));
            continue;
        }
        costs.insert(QString::fromLatin1(name), median(samples));
        lines << QStringLiteral("%1 %2 ms").arg(QString::fromLatin1(name)).arg(median(samples), 0, 'f', 1);
    }
    writeOutputFile(QStringLiteral("startup-panels.json"), QString::fromUtf8(QJsonDocument(costs).toJson()));
    WARN("panel creation (median of 3): " << lines.join(QStringLiteral("; ")).toStdString());
    takeMessages(); // panels made outside the window miss its context (no project): their warnings are not the point
}

TEST_CASE("Lazy panels: created when first shown or after the first frame, kept afterwards, and working",
          "[app][startup][qml]") {
    resetSettings();
    clearMessages();
    TimerIncubationController incubation;
    QQmlEngine engine;
    engine.setIncubationController(&incubation); // as in the app: the panels are deferred
    QQmlComponent component(&engine, "Wabcut", "Main");
    REQUIRE_FALSE(component.isError());
    std::unique_ptr<QQuickWindow> window(
        qobject_cast<QQuickWindow*>(component.createWithInitialProperties({{QStringLiteral("visible"), false}})));
    REQUIRE(window);
    window->create();
    window->resize(kShotWidth, kShotHeight);
    window->contentItem()->setSize(QSizeF(kShotWidth, kShotHeight));
    REQUIRE(window->property("deferPanels").toBool());
    CHECK_FALSE(window->property("preloadPanels").toBool());
    CHECK_FALSE(window->property("panelsReady").toBool());

    const auto loader = [&](const char* name) {
        QObject* l = window->findChild<QObject*>(QStringLiteral("lazyPanel_") + QString::fromLatin1(name));
        INFO(name);
        REQUIRE(l);
        return l;
    };
    const auto itemOf = [](QObject* l) { return l->property("item").value<QQuickItem*>(); };
    const char* lazy[] = {"effects", "captions", "audio", "markers", "generate", "shorts", "takes", "essential",
                          "review", "scopes", "captionStyle", "frameInspector", "mixer", "export", "color"};
    // The window starts with the Edit workspace and the Library: nothing else is created yet.
    for (const char* name : lazy) {
        INFO(name);
        CHECK(itemOf(loader(name)) == nullptr);
    }
    CHECK(window->findChild<QObject*>(QStringLiteral("libraryPanel")) != nullptr);

    // A browser tab: created when first shown (in the background), kept when another is shown.
    REQUIRE(QMetaObject::invokeMethod(window.get(), "showBrowserPanel", Q_ARG(QVariant, QVariant(QStringLiteral("essential")))));
    QObject* essential = loader("essential");
    REQUIRE(waitFor([&] { return itemOf(essential) != nullptr; }, 10000));
    QQuickItem* essentialPanel = itemOf(essential);
    CHECK(essentialPanel->objectName() == QStringLiteral("essentialSoundPanel"));
    CHECK(essentialPanel->width() > 0);
    REQUIRE(waitFor([&] { return essentialPanel->isVisible(); }, 3000));
    CHECK(namedIn(essentialPanel, QStringLiteral("essentialEmpty")) != nullptr); // it works: nothing selected
    REQUIRE(QMetaObject::invokeMethod(window.get(), "showBrowserPanel", Q_ARG(QVariant, QVariant(QStringLiteral("library")))));
    REQUIRE(waitFor([&] { return !essentialPanel->isVisible(); }, 3000));
    CHECK(itemOf(essential) == essentialPanel);
    CHECK(itemOf(loader("effects")) == nullptr); // only what was shown

    // A workspace's panel: the Audio workspace's mixer, with its strips.
    window->setProperty("workspace", 1);
    QObject* mixer = loader("mixer");
    REQUIRE(waitFor([&] { return itemOf(mixer) != nullptr; }, 10000));
    CHECK(namedIn(itemOf(mixer), QStringLiteral("mixerStrips")) != nullptr);
    window->setProperty("workspace", 0);

    // Generative Extend from the clip menu: the Generate tab is created, then given the clip.
    QObject* generate = engine.singletonInstance<QObject*>("Wabcut", "GenerateController");
    REQUIRE(generate);
    REQUIRE(QMetaObject::invokeMethod(generate, "showRequested", Q_ARG(QString, QStringLiteral("c7"))));
    CHECK(window->property("browserPanel").toString() == QStringLiteral("generate"));
    QObject* generateLoader = loader("generate");
    REQUIRE(waitFor([&] { return itemOf(generateLoader) != nullptr; }, 10000));
    REQUIRE(waitFor([&] { return itemOf(generateLoader)->property("extendClip").toString() == QStringLiteral("c7"); },
                    3000));
    REQUIRE(QMetaObject::invokeMethod(window.get(), "showBrowserPanel", Q_ARG(QVariant, QVariant(QStringLiteral("library")))));

    // After the first frame (what frameSwapped does in the app): the rest, in the background.
    window->setProperty("preloadPanels", true);
    REQUIRE(waitFor([&] { return window->property("panelsReady").toBool(); }, 30000));
    for (const char* name : lazy) {
        INFO(name);
        CHECK(itemOf(loader(name)) != nullptr);
    }
    CHECK(itemOf(essential) == essentialPanel); // kept, not made again

    // Explain this frame: a tab next to Effect Controls, shown and closed.
    REQUIRE(QMetaObject::invokeMethod(window.get(), "showFrameInspector"));
    QQuickItem* inspector = itemOf(loader("frameInspector"));
    REQUIRE(waitFor([&] { return inspector->isVisible(); }, 3000));
    QQuickItem* close = namedIn(inspector, QStringLiteral("frameInspectorClose"));
    REQUIRE(close);
    REQUIRE(QMetaObject::invokeMethod(close, "clicked"));
    CHECK_FALSE(QQmlExpression(qmlContext(window.get()), window.get(), QStringLiteral("docking.isOpen('frameInspector')"))
                    .evaluate()
                    .toBool());

    // The timings: one span per panel created.
    QObject* profile = engine.singletonInstance<QObject*>("Wabcut", "UiProfile");
    REQUIRE(profile);
    QStringList spans;
    for (const QVariant& e : profile->property("entries").toList()) {
        const QVariantMap m = e.toMap();
        if (m.value(QStringLiteral("kind")).toString() == QStringLiteral("span"))
            spans << m.value(QStringLiteral("what")).toString();
    }
    for (const char* name : lazy) {
        INFO(name);
        CHECK(spans.contains(QStringLiteral("panel ") + QString::fromLatin1(name)));
    }
    window.reset();
    spin(50);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
