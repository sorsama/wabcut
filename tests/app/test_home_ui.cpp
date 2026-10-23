// M5f: the Home screen — sequence presets and New Project, recent projects (persisted, missing files flagged), the
// import checks (detectImport), importing a timeline as a new project whose sequence opens (no empty placeholder, one
// undo step, the old default unchanged), the experimental .prproj reader only on request, and the screen in the
// shell: File > Home, a real drop of an FCP7 XML, the import report, 1100x700. Design language v3: the search narrows
// the projects, a star persists and the Starred filter shows it, the list view.
// UX rework (Home as a start window, C++): openDemo, the New project page's Advanced settings (applied before the
// project exists, bad values refused), free names and the summary line, the all-in-one Import's router
// (classifyImport, folders), the recent list's limit with stars kept, remove / clear unstarred with Undo, Locate,
// Show in Explorer, Copy Path, the website links and the import dialog filters.
// UX rework (Home's pages, QML): the Home page (Continue, Start, Recent) fits without scrolling; the nav and Home's
// keys switch Home / Projects / Learn / New project, each page fits at 1100x700 and 1600x960 and passes the
// accessibility audit; the all-in-one Import's routes on screen (the .prproj prompt, guidance for a Resolve project, a
// timeline dropped on the drop well); the Projects page's search, star, Starred, sort and view (remembered), the
// project menu, Delete with Undo, Locate and Clear Unstarred; New project creates with the Advanced values and
// remembers them; Learn's sample project; the start-up choice "Reopen the last project". Screenshots: home,
// home-prproj, home-import, home-import-report, home-1100x700, home-projects, home-projects-list, home-learn,
// home-new, home-new-advanced, home-new-1100x700, home-light (home-new-media: test_gaming_start).
// Offscreen; no window is shown.

#include "A11yAudit.h"
#include "ShellFixture.h"

#include "HomeBridge.h"
#include "LibraryBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/model/AudioChannels.h>
#include <wabcut/model/ColorManagement.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>
#include <wabcut/model/Timecode.h>

#include <catch2/catch_test_macros.hpp>

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QGuiApplication>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QQuickItem>
#include <QSettings>
#include <QTemporaryDir>

#include <functional>
#include <optional>
#include <utility>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

QString fixture(const char* name) {
    return QDir(QStringLiteral(WABCUT_INTERCHANGE_FIXTURES)).filePath(QString::fromLatin1(name));
}

QVariantMap presetNamed(const HomeBridge& home, const QString& id) {
    for (const QVariant& p : home.presets()) {
        if (p.toMap().value(QStringLiteral("id")).toString() == id)
            return p.toMap();
    }
    return {};
}

QVariantMap rateLabelled(const HomeBridge& home, const QString& label) {
    for (const QVariant& r : home.rates()) {
        if (r.toMap().value(QStringLiteral("label")).toString() == label)
            return r.toMap();
    }
    return {};
}

/// Home over a real project file service (no window).
struct Fixture {
    ProjectController controller;
    ProjectLifecycle lifecycle{&controller};
    HomeBridge home;
    QTemporaryDir dir;

    Fixture() {
        resetSettings();
        home.setController(&controller);
        home.setLifecycle(&lifecycle);
        home.refreshRecent();
    }
    QUrl folder(const QString& sub = {}) const {
        return QUrl::fromLocalFile(sub.isEmpty() ? dir.path() : dir.filePath(sub));
    }
};

QQuickItem* findItem(QQuickItem* root, const std::function<bool(QQuickItem*)>& match) {
    if (!root)
        return nullptr;
    if (match(root))
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findItem(child, match))
            return found;
    }
    return nullptr;
}

/// Lays out `root` and everything under it now, parents first: a window that is never shown only polishes when a frame
/// is rendered, so layouts would keep stale sizes and lists would not make their cards (twice: new cards lay out too).
void polishAll(QQuickItem* root) {
    if (!root)
        return;
    root->ensurePolished();
    for (QQuickItem* child : root->childItems())
        polishAll(child);
}

void polishWindow(Shell& shell) {
    for (int pass = 0; pass < 2; ++pass)
        polishAll(shell.window->contentItem());
}

/// The shown item named `objectName`, after laying the window out.
QQuickItem* visibleItem(Shell& shell, const QString& objectName) {
    polishWindow(shell);
    return findItem(shell.window->contentItem(), [&](QQuickItem* i) {
        return i->objectName() == objectName && i->isVisible();
    });
}

/// A QML `var` list (a JS array) as a QVariantList.
QVariantList variantList(const QVariant& value) {
    if (value.canConvert<QJSValue>() && value.metaType() == QMetaType::fromType<QJSValue>())
        return value.value<QJSValue>().toVariant().toList();
    return value.toList();
}

/// The demo project is still what the editor shows (nothing was imported).

bool demoShown(Shell& shell) {
    auto* lifecycle = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    return lifecycle && lifecycle->demo();
}

} // namespace

// ---- New project ----------------------------------------------------------------------------------------------------

TEST_CASE("Home: a new project from a preset gets that sequence and is saved where asked", "[app][home]") {
    Fixture fx;
    const QVariantList presets = fx.home.presets();
    REQUIRE(presets.size() == 12);
    QStringList ids;
    QStringList groups;
    for (const QVariant& p : presets) {
        ids << p.toMap().value(QStringLiteral("id")).toString();
        groups << p.toMap().value(QStringLiteral("group")).toString();
    }
    // The first nine keep their ids (homePreset_<id>); the UX rework adds three after them.
    CHECK(ids == QStringList{"1080p23976", "1080p25", "1080p2997", "1080p30", "1080p60", "uhd", "vertical", "square",
                             "custom", "1440p60", "uhd60", "vertical60"});
    CHECK(groups == QStringList{"landscape", "landscape", "landscape", "landscape", "landscape", "landscape", "vertical",
                                "vertical", "custom", "landscape", "landscape", "vertical"});
    CHECK(presetNamed(fx.home, "1080p2997").value("rateLabel").toString() == QStringLiteral("29.97"));
    CHECK(presetNamed(fx.home, "1440p60").value("width").toInt() == 2560);
    CHECK(presetNamed(fx.home, "vertical60").value("rateNum").toInt() == 60);
    CHECK(fx.lifecycle.demo());

    SECTION("1080p 29.97") {
        const QVariantMap preset = presetNamed(fx.home, "1080p2997");
        REQUIRE(fx.home.createProject(QStringLiteral("Brand Film"), fx.folder(), preset).isEmpty());
        const SequencePtr seq = fx.controller.currentSequence();
        REQUIRE(seq);
        CHECK(seq->frameRate.num == kRate29_97.num);
        CHECK(seq->frameRate.den == kRate29_97.den);
        CHECK(fx.controller.frameWidth() == 1920);
        CHECK(fx.controller.frameHeight() == 1080);
        CHECK(fx.controller.sequenceNames() == QStringList{QStringLiteral("Sequence 01")});
        CHECK_FALSE(fx.lifecycle.demo());
        CHECK_FALSE(fx.lifecycle.untitled());
        CHECK(QDir::fromNativeSeparators(fx.lifecycle.filePath()) == fx.dir.filePath(QStringLiteral("Brand Film.wabcut")));
        CHECK(QFile::exists(fx.dir.filePath(QStringLiteral("Brand Film.wabcut"))));
        // A second project with that name in that folder is refused; nothing changes.
        CHECK_FALSE(fx.home.createProject(QStringLiteral("Brand Film"), fx.folder(), preset).isEmpty());
    }
    SECTION("vertical 9:16 at another rate, in a folder that does not exist yet") {
        QVariantMap preset = presetNamed(fx.home, "vertical");
        CHECK(preset.value("rateChoice").toBool());
        const QVariantMap rate = rateLabelled(fx.home, "25");
        preset.insert(QStringLiteral("rateNum"), rate.value("rateNum"));
        preset.insert(QStringLiteral("rateDen"), rate.value("rateDen"));
        REQUIRE(fx.home.createProject(QStringLiteral("Social"), fx.folder(QStringLiteral("Clips/Social")), preset).isEmpty());
        CHECK(fx.controller.frameWidth() == 1080);
        CHECK(fx.controller.frameHeight() == 1920);
        CHECK(fx.controller.currentSequence()->frameRate.num == 25);
        CHECK(QFile::exists(fx.dir.filePath(QStringLiteral("Clips/Social/Social.wabcut"))));
    }
    SECTION("custom size") {
        QVariantMap preset = presetNamed(fx.home, "custom");
        preset.insert(QStringLiteral("width"), 1280);
        preset.insert(QStringLiteral("height"), 720);
        REQUIRE(fx.home.createProject(QStringLiteral("Custom"), fx.folder(), preset).isEmpty());
        CHECK(fx.controller.formatText() == QStringLiteral("720p30"));
    }
    SECTION("names and sizes that cannot be used") {
        CHECK_FALSE(fx.home.validateProject(QStringLiteral("  "), fx.folder()).isEmpty());
        CHECK_FALSE(fx.home.validateProject(QStringLiteral("a/b"), fx.folder()).isEmpty());
        CHECK_FALSE(fx.home.validateProject(QStringLiteral("Film"), QUrl()).isEmpty());
        CHECK(fx.home.validateProject(QStringLiteral("Film"), fx.folder()).isEmpty());
        CHECK(fx.home.projectPath(QStringLiteral("Film"), fx.folder()) ==
              QDir::toNativeSeparators(fx.dir.filePath(QStringLiteral("Film.wabcut"))));
        QVariantMap bad = presetNamed(fx.home, "custom");
        bad.insert(QStringLiteral("width"), 0);
        CHECK_FALSE(fx.home.createProject(QStringLiteral("Bad"), fx.folder(), bad).isEmpty());
        CHECK(fx.lifecycle.demo()); // nothing changed
        CHECK_FALSE(fx.lifecycle.newProjectWith({{QStringLiteral("width"), 1920}, {QStringLiteral("height"), 1080},
                                                 {QStringLiteral("rateNum"), 30}, {QStringLiteral("rateDen"), 0}}));
        CHECK_FALSE(fx.lifecycle.lastError().isEmpty());
    }
}

// ---- Recent projects ------------------------------------------------------------------------------------------------

TEST_CASE("Home: recent projects are remembered newest first, persist, flag missing files and can be removed", "[app][home]") {
    Fixture fx;
    CHECK(fx.home.recent().isEmpty());
    const QVariantMap hd = presetNamed(fx.home, "1080p25");
    REQUIRE(fx.home.createProject(QStringLiteral("First"), fx.folder(), hd).isEmpty());
    REQUIRE(fx.home.createProject(QStringLiteral("Second"), fx.folder(), presetNamed(fx.home, "uhd")).isEmpty());
    QVariantList recent = fx.home.recent();
    REQUIRE(recent.size() == 2);
    CHECK(recent[0].toMap().value("name").toString() == QStringLiteral("Second"));
    CHECK(recent[0].toMap().value("format").toString() == QStringLiteral("2160p23.976"));
    CHECK(recent[0].toMap().value("openedText").toString().startsWith(QStringLiteral("Today")));
    CHECK(recent[1].toMap().value("name").toString() == QStringLiteral("First"));
    CHECK_FALSE(recent[1].toMap().value("missing").toBool());
    CHECK(QDir::fromNativeSeparators(fx.home.defaultFolder().toLocalFile()) == fx.dir.path());

    // Opening one moves it to the front.
    REQUIRE(fx.lifecycle.open(QUrl::fromLocalFile(fx.dir.filePath(QStringLiteral("First.wabcut")))));
    CHECK(fx.home.recent()[0].toMap().value("name").toString() == QStringLiteral("First"));

    // Persisted: a new Home reads the same list.
    {
        HomeBridge again;
        REQUIRE(again.recent().size() == 2);
        CHECK(again.recent()[0].toMap().value("name").toString() == QStringLiteral("First"));
    }

    // A file that is gone is flagged, then removed from the list.
    REQUIRE(QFile::remove(fx.dir.filePath(QStringLiteral("Second.wabcut"))));
    fx.home.refreshRecent();
    recent = fx.home.recent();
    REQUIRE(recent.size() == 2);
    CHECK(recent[1].toMap().value("missing").toBool());
    fx.home.removeRecent(recent[1].toMap().value("url").toUrl());
    CHECK(fx.home.recent().size() == 1);
    fx.home.clearRecent();
    CHECK(fx.home.recent().isEmpty());
    CHECK(QSettings().value(QStringLiteral("home/recentProjects")).toList().isEmpty());
}

// ---- Import ---------------------------------------------------------------------------------------------------------

TEST_CASE("Home: detectImport says what a file is before importing it", "[app][home]") {
    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);
    const QVariantMap xml = lifecycle.detectImport(QUrl::fromLocalFile(fixture("fcp7_23976_mac.xml")));
    CHECK(xml.value("kind").toString() == QStringLiteral("timeline"));
    CHECK(xml.value("format").toString() == QStringLiteral("fcp7-xml"));
    CHECK(xml.value("formatName").toString() == QStringLiteral("Final Cut Pro 7 XML"));
    CHECK(xml.value("supported").toBool());
    CHECK_FALSE(xml.value("experimental").toBool());

    const QVariantMap prproj = lifecycle.detectImport(QUrl::fromLocalFile(fixture("prproj/07_transitions.prproj")));
    CHECK(prproj.value("kind").toString() == QStringLiteral("timeline"));
    CHECK(prproj.value("experimental").toBool());
    CHECK_FALSE(prproj.value("guidance").toString().isEmpty()); // export XML from Premiere instead

    QTemporaryDir dir;
    const QString drp = dir.filePath(QStringLiteral("Grade.drp"));
    {
        QFile f(drp);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("not a timeline");
    }
    const QVariantMap resolve = lifecycle.detectImport(QUrl::fromLocalFile(drp));
    CHECK(resolve.value("kind").toString() == QStringLiteral("timeline"));
    CHECK_FALSE(resolve.value("supported").toBool());
    CHECK_FALSE(resolve.value("guidance").toString().isEmpty());

    CHECK(lifecycle.detectImport(QUrl::fromLocalFile(dir.filePath(QStringLiteral("a.wabcut")))).value("kind") == QStringLiteral("project"));
    CHECK(lifecycle.detectImport(QUrl::fromLocalFile(dir.filePath(QStringLiteral("clip.mov")))).value("kind") == QStringLiteral("media"));
}

TEST_CASE("Home: a timeline imports as a new project whose sequence opens, in one undo step", "[app][home]") {
    const QUrl xml = QUrl::fromLocalFile(fixture("fcp7_23976_mac.xml"));
    SECTION("newProject + openImported: just the imported sequence, current, reported") {
        ProjectController controller;
        ProjectLifecycle lifecycle(&controller);
        QList<QVariantMap> reports;
        QObject::connect(&lifecycle, &ProjectLifecycle::importReport, [&](const QVariantMap& r) { reports << r; });
        const QVariantMap result = lifecycle.importFiles(
            {xml}, {{QStringLiteral("newProject"), true}, {QStringLiteral("openImported"), true}});
        CHECK(result.value("timelines").toInt() == 1);
        CHECK(result.value("firstSequence").toInt() == 0);
        CHECK(controller.sequenceNames() == QStringList{QStringLiteral("Reel 2 Assembly")});
        CHECK(controller.sequenceName() == QStringLiteral("Reel 2 Assembly"));
        CHECK(controller.formatText() == QStringLiteral("1080p23.976"));
        REQUIRE(reports.size() == 1);
        CHECK(reports[0].value("format").toString() == QStringLiteral("Final Cut Pro 7 XML"));
        CHECK_FALSE(lifecycle.demo());
        CHECK(lifecycle.dirty());
        // One undo step: back to the new project's empty sequence.
        REQUIRE(controller.undo());
        CHECK(controller.sequenceNames() == QStringList{QStringLiteral("Sequence 01")});
    }
    SECTION("from an open project, newProject starts another one") {
        ProjectController controller;
        ProjectLifecycle lifecycle(&controller);
        lifecycle.newProject();
        controller.newSequence();
        REQUIRE(controller.sequenceNames().size() == 2);
        lifecycle.importFiles({xml}, {{QStringLiteral("newProject"), true}, {QStringLiteral("openImported"), true}});
        CHECK(controller.sequenceNames() == QStringList{QStringLiteral("Reel 2 Assembly")});
    }
    SECTION("without options the old behaviour stays: the demo's import keeps its first sequence current") {
        ProjectController controller;
        ProjectLifecycle lifecycle(&controller);
        const QVariantMap result = lifecycle.importFiles({xml});
        CHECK(result.value("timelines").toInt() == 1);
        CHECK_FALSE(result.contains(QStringLiteral("firstSequence")));
        CHECK(controller.sequenceNames() == QStringList{QStringLiteral("Sequence 01"), QStringLiteral("Reel 2 Assembly")});
        CHECK(controller.currentSequenceIndex() == 0);
    }
}

TEST_CASE("Home: a Premiere .prproj imports only with the experimental reader", "[app][home]") {
    const QUrl prproj = QUrl::fromLocalFile(fixture("prproj/07_transitions.prproj"));
    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);
    const QVariantMap refused = lifecycle.importFiles({prproj}, {{QStringLiteral("newProject"), true}});
    CHECK(refused.value("timelines").toInt() == 0);
    CHECK(refused.value("failed").toList().size() == 1);
    const QVariantMap read = lifecycle.importFiles(
        {prproj}, {{QStringLiteral("newProject"), true},
                   {QStringLiteral("openImported"), true},
                   {QStringLiteral("experimentalPremiereProject"), true}});
    CHECK(read.value("timelines").toInt() == 1);
    CHECK(read.value("firstSequence").toInt() == 0);
    CHECK_FALSE(controller.sequenceNames().contains(QStringLiteral("Sequence 01")));
}

// ---- UX rework (Home as a start window): the C++ behind its pages ---------------------------------------------------

namespace {

void writeFile(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}

QStringList namesOf(const QVariantList& entries) {
    QStringList names;
    for (const QVariant& e : entries)
        names << e.toMap().value(QStringLiteral("name")).toString();
    return names;
}

QVariantMap classify(const HomeBridge& home, const QStringList& paths) {
    QList<QUrl> urls;
    for (const QString& path : paths)
        urls << QUrl::fromLocalFile(path);
    return home.classifyImport(urls);
}

/// Restores the platform's file manager after a test replaced it.
struct RevealerGuard {
    ~RevealerGuard() { LibraryBridge::setRevealer({}); }
};

} // namespace

TEST_CASE("Home: openDemo brings back the sample project as at launch", "[app][home]") {
    Fixture fx;
    REQUIRE(fx.lifecycle.demo());
    const QStringList demoSequences = fx.controller.sequenceNames();
    REQUIRE(demoSequences.size() == 3);
    const QString demoSequence = fx.controller.sequenceName();
    const Ticks demoPlayhead = fx.controller.playhead();

    SECTION("after a saved project with unsaved changes") {
        REQUIRE(fx.home.createProject(QStringLiteral("Film"), fx.folder(), presetNamed(fx.home, "1080p25")).isEmpty());
        REQUIRE_FALSE(fx.lifecycle.demo());
        fx.controller.newSequence();
        REQUIRE(fx.lifecycle.dirty());
        REQUIRE(fx.lifecycle.openDemo());
        CHECK(fx.lifecycle.demo());
        CHECK(fx.lifecycle.untitled());
        CHECK_FALSE(fx.lifecycle.dirty());
        CHECK_FALSE(fx.controller.canUndo());
        CHECK(fx.lifecycle.name() == QStringLiteral("Demo project"));
        CHECK(fx.controller.sequenceNames() == demoSequences);
        CHECK(fx.controller.sequenceName() == demoSequence);
        CHECK(fx.controller.playhead() == demoPlayhead);
        CHECK(fx.lifecycle.openProjects().size() == 1);
        // The project stays in the recent list; the demo is never remembered.
        REQUIRE(fx.home.recent().size() == 1);
        CHECK(fx.home.recent()[0].toMap().value("name").toString() == QStringLiteral("Film"));
    }
    SECTION("edits to the demo itself are dropped") {
        fx.controller.newSequence();
        REQUIRE(fx.controller.sequenceNames().size() == 4);
        REQUIRE(fx.lifecycle.openDemo());
        CHECK(fx.controller.sequenceNames() == demoSequences);
        CHECK_FALSE(fx.controller.canUndo());
    }
    SECTION("with two projects open only the active one makes way") {
        REQUIRE(fx.home.createProject(QStringLiteral("A"), fx.folder(), presetNamed(fx.home, "1080p25")).isEmpty());
        REQUIRE(fx.home.createProject(QStringLiteral("B"), fx.folder(), presetNamed(fx.home, "1080p30")).isEmpty());
        REQUIRE(fx.lifecycle.openAlongside(QUrl::fromLocalFile(fx.dir.filePath(QStringLiteral("A.wabcut")))));
        REQUIRE(fx.lifecycle.openProjects().size() == 2);
        REQUIRE(fx.lifecycle.openDemo());
        const QVariantList open = fx.lifecycle.openProjects();
        REQUIRE(open.size() == 2);
        CHECK(open[0].toMap().value("name").toString() == QStringLiteral("B"));
        CHECK_FALSE(open[0].toMap().value("demo").toBool());
        CHECK(open[1].toMap().value("demo").toBool());
        CHECK(open[1].toMap().value("active").toBool());
        CHECK(fx.lifecycle.demo());
    }
}

TEST_CASE("Home: Advanced settings are applied when the project is created", "[app][home]") {
    Fixture fx;
    SECTION("every key") {
        QVariantMap settings = presetNamed(fx.home, "1080p2997");
        settings.insert(QStringLiteral("audioRate"), 44100);
        settings.insert(QStringLiteral("dropFrame"), true);
        settings.insert(QStringLiteral("videoTracks"), 2);
        settings.insert(QStringLiteral("audioTracks"), 5);
        settings.insert(QStringLiteral("trackFormat"), QStringLiteral("mono"));
        settings.insert(QStringLiteral("masterFormat"), QStringLiteral("5.1"));
        settings.insert(QStringLiteral("pixelAspect"), QVariantMap{{QStringLiteral("num"), 10}, {QStringLiteral("den"), 11}});
        settings.insert(QStringLiteral("fields"), QStringLiteral("upperFirst"));
        settings.insert(QStringLiteral("startTimecode"), QStringLiteral("01:00:00;00"));
        settings.insert(QStringLiteral("previewFormat"), QStringLiteral("prores"));
        settings.insert(QStringLiteral("colorManagement"),
                        QVariantMap{{QStringLiteral("mode"), QStringLiteral("hdr-pq")}, {QStringLiteral("peakNits"), 1000}});
        settings.insert(QStringLiteral("sequenceName"), QStringLiteral("Main Edit"));
        settings.insert(QStringLiteral("somethingElse"), 42); // unknown keys are ignored
        CHECK(fx.home.validateSettings(settings).isEmpty());
        REQUIRE(fx.home.createProject(QStringLiteral("Advanced"), fx.folder(), settings).isEmpty());
        const SequencePtr seq = fx.controller.currentSequence();
        REQUIRE(seq);
        CHECK(seq->name == "Main Edit");
        CHECK(seq->frameRate == kRate29_97);
        CHECK(seq->audioRate == Rate{44100, 1});
        CHECK(seq->dropFrame);
        CHECK(seq->videoTracks.size() == 2);
        REQUIRE(seq->audioTracks.size() == 5);
        for (const auto& track : seq->audioTracks)
            CHECK(track->format == ChannelFormat::Mono);
        CHECK(seq->masterFormat == ChannelFormat::Surround51);
        CHECK(seq->pixelAspect == Rate{10, 11});
        CHECK(seq->fields == FieldOrder::UpperFirst);
        const auto hour = parseTimecode("01:00:00;00", kRate29_97, true);
        REQUIRE(hour);
        CHECK(seq->startTimecode == *hour);
        CHECK(seq->previewFormat == "prores");
        CHECK(fx.controller.snapshot()->colorManagement.mode == ColorManagementMode::HdrPq);
        // Part of the new project: no undo step, nothing unsaved.
        CHECK_FALSE(fx.lifecycle.dirty());
        CHECK_FALSE(fx.controller.canUndo());
    }
    SECTION("a model preset and drop-frame only where it exists") {
        REQUIRE(fx.home.createProject(QStringLiteral("Broadcast"), fx.folder(),
                                      {{QStringLiteral("preset"), QStringLiteral("1080p29.97df")}})
                    .isEmpty());
        CHECK(fx.controller.currentSequence()->dropFrame);
        CHECK(fx.controller.currentSequence()->frameRate == kRate29_97);
        QVariantMap pal = presetNamed(fx.home, "1080p25");
        pal.insert(QStringLiteral("dropFrame"), true);
        REQUIRE(fx.home.createProject(QStringLiteral("Pal"), fx.folder(), pal).isEmpty());
        CHECK_FALSE(fx.controller.currentSequence()->dropFrame);
        CHECK(fx.controller.currentSequence()->name == "Sequence 01");
    }
    SECTION("the defaults and every pill choice are accepted") {
        QVariantMap settings = presetNamed(fx.home, "1080p25");
        const QVariantMap defaults = fx.home.advancedDefaults();
        CHECK(defaults.value("audioRate").toInt() == 48000);
        CHECK(defaults.value("videoTracks").toInt() == 3);
        CHECK(defaults.value("audioTracks").toInt() == 3);
        CHECK(defaults.value("colorManagement").toMap().value("mode").toString() == QStringLiteral("rec709"));
        for (auto it = defaults.begin(); it != defaults.end(); ++it)
            settings.insert(it.key(), it.value());
        CHECK(fx.home.validateSettings(settings).isEmpty());

        const QVariantMap choices = fx.home.advancedChoices();
        const auto accepted = [&](const QString& key, const QVariant& value) {
            QVariantMap s = presetNamed(fx.home, "1080p25");
            s.insert(key, value);
            INFO(key.toStdString() << " = " << value.toString().toStdString());
            CHECK(fx.home.validateSettings(s).isEmpty());
        };
        for (const QVariant& c : choices.value("audioRates").toList())
            accepted(QStringLiteral("audioRate"), c.toMap().value("value"));
        for (const QVariant& c : choices.value("pixelAspects").toList())
            accepted(QStringLiteral("pixelAspect"), c.toMap());
        const std::pair<const char*, const char*> lists[] = {{"fields", "fields"},
                                                             {"masterFormats", "masterFormat"},
                                                             {"trackFormats", "trackFormat"},
                                                             {"previewFormats", "previewFormat"}};
        for (const auto& [list, setting] : lists) {
            const QVariantList entries = choices.value(QString::fromLatin1(list)).toList();
            CHECK_FALSE(entries.isEmpty());
            for (const QVariant& c : entries)
                accepted(QString::fromLatin1(setting), c.toMap().value("id"));
        }
        QStringList aspects;
        for (const QVariant& c : choices.value("pixelAspects").toList())
            aspects << c.toMap().value("id").toString();
        CHECK(aspects == QStringList{QStringLiteral("square"), QStringLiteral("d1"), QStringLiteral("anamorphic")});
    }
}

TEST_CASE("Home: bad Advanced settings are refused and nothing is created", "[app][home]") {
    Fixture fx;
    const auto refused = [&](const QString& key, const QVariant& value) {
        QVariantMap settings = presetNamed(fx.home, "1080p25");
        settings.insert(key, value);
        INFO(key.toStdString());
        CHECK_FALSE(fx.home.validateSettings(settings).isEmpty());
        CHECK_FALSE(fx.home.createProject(QStringLiteral("Refused"), fx.folder(), settings).isEmpty());
        CHECK_FALSE(fx.lifecycle.newProjectWith(settings));
        CHECK_FALSE(fx.lifecycle.lastError().isEmpty());
    };
    refused(QStringLiteral("audioRate"), 1000);
    refused(QStringLiteral("videoTracks"), 0);
    refused(QStringLiteral("audioTracks"), 100);
    refused(QStringLiteral("trackFormat"), QStringLiteral("quad"));
    refused(QStringLiteral("masterFormat"), QStringLiteral("mono"));
    refused(QStringLiteral("fields"), QStringLiteral("diagonal"));
    refused(QStringLiteral("pixelAspect"), 0);
    refused(QStringLiteral("previewFormat"), QStringLiteral("gif"));
    refused(QStringLiteral("startTimecode"), QStringLiteral("soon"));
    refused(QStringLiteral("colorManagement"), QVariantMap{{QStringLiteral("mode"), QStringLiteral("sepia")}});
    refused(QStringLiteral("colorManagement"),
            QVariantMap{{QStringLiteral("mode"), QStringLiteral("hdr-pq")}, {QStringLiteral("peakNits"), 5}});
    refused(QStringLiteral("width"), 20000);
    refused(QStringLiteral("height"), 8);
    refused(QStringLiteral("preset"), QStringLiteral("8k-imax"));
    refused(QStringLiteral("rateDen"), 0);
    CHECK(fx.lifecycle.demo()); // nothing was created
    CHECK_FALSE(QFile::exists(fx.dir.filePath(QStringLiteral("Refused.wabcut"))));
    CHECK(fx.home.recent().isEmpty());
}

TEST_CASE("Home: free project names and the settings summary", "[app][home]") {
    Fixture fx;
    const QString untitled = QStringLiteral("Untitled Project");
    CHECK(fx.home.uniqueProjectName(QString(), fx.folder()) == untitled);
    CHECK(fx.home.uniqueProjectName(untitled, fx.folder()) == untitled);
    REQUIRE(fx.home.createProject(untitled, fx.folder(), presetNamed(fx.home, "1080p30")).isEmpty());
    CHECK(fx.home.uniqueProjectName(untitled, fx.folder()) == QStringLiteral("Untitled Project 2"));
    writeFile(fx.dir.filePath(QStringLiteral("Untitled Project 2.wabcut")), "x");
    CHECK(fx.home.uniqueProjectName(untitled, fx.folder()) == QStringLiteral("Untitled Project 3"));
    CHECK(fx.home.uniqueProjectName(QStringLiteral("Ranked: match?"), fx.folder()) == QStringLiteral("Ranked match"));
    CHECK(fx.home.uniqueProjectName(QStringLiteral("Trailer."), fx.folder()) == QStringLiteral("Trailer"));
    for (const QString& base : {QStringLiteral("a/b"), QStringLiteral("  "), QStringLiteral("<>|")})
        CHECK(fx.home.validateProject(fx.home.uniqueProjectName(base, fx.folder()), fx.folder()).isEmpty());

    CHECK(fx.home.settingsSummary(presetNamed(fx.home, "1080p2997")) ==
          QString::fromUtf8("1920 × 1080 · 29.97 fps · 48 kHz stereo · 3 video / 3 audio tracks"));
    QVariantMap settings = presetNamed(fx.home, "vertical60");
    settings.insert(QStringLiteral("audioRate"), 44100);
    settings.insert(QStringLiteral("masterFormat"), QStringLiteral("5.1"));
    settings.insert(QStringLiteral("videoTracks"), 2);
    settings.insert(QStringLiteral("audioTracks"), 4);
    CHECK(fx.home.settingsSummary(settings) ==
          QString::fromUtf8("1080 × 1920 · 60 fps · 44.1 kHz 5.1 · 2 video / 4 audio tracks"));

    CHECK(fx.home.rateLabel(30000, 1001) == QStringLiteral("29.97"));
    CHECK(fx.home.rateLabel(120, 1) == QStringLiteral("120"));
    CHECK(fx.home.rateLabel(48000, 1001) == QStringLiteral("47.952"));
    QStringList labels;
    QStringList dropFrame;
    for (const QVariant& r : fx.home.rates()) {
        labels << r.toMap().value("label").toString();
        if (r.toMap().value("canDropFrame").toBool())
            dropFrame << r.toMap().value("label").toString();
    }
    CHECK(labels.contains(QStringLiteral("120")));
    CHECK(labels.contains(QStringLiteral("144")));
    CHECK(labels.contains(QStringLiteral("240")));
    CHECK(dropFrame == QStringList{QStringLiteral("29.97"), QStringLiteral("59.94")});
    // Every rate makes a project.
    for (const QVariant& r : fx.home.rates()) {
        QVariantMap s = presetNamed(fx.home, "1080p30");
        s.insert(QStringLiteral("rateNum"), r.toMap().value("rateNum"));
        s.insert(QStringLiteral("rateDen"), r.toMap().value("rateDen"));
        INFO(r.toMap().value("label").toString().toStdString());
        CHECK(fx.home.validateSettings(s).isEmpty());
    }
}

TEST_CASE("Home: the default folder setting wins over the last project's folder", "[app][home]") {
    Fixture fx;
    REQUIRE(fx.home.createProject(QStringLiteral("Last"), fx.folder(QStringLiteral("Projects")), presetNamed(fx.home, "1080p30"))
                .isEmpty());
    CHECK(QDir::fromNativeSeparators(fx.home.defaultFolder().toLocalFile()) == fx.dir.filePath(QStringLiteral("Projects")));
    CHECK(fx.home.defaultFolderSetting().isEmpty());
    int changes = 0;
    QObject::connect(&fx.home, &HomeBridge::defaultFolderChanged, [&] { ++changes; });
    fx.home.setDefaultFolderSetting(fx.folder(QStringLiteral("Chosen")));
    CHECK(changes == 1);
    CHECK(QDir::fromNativeSeparators(fx.home.defaultFolder().toLocalFile()) == fx.dir.filePath(QStringLiteral("Chosen")));
    CHECK(QSettings().value(QStringLiteral("home/defaultFolder")).toString() == fx.dir.filePath(QStringLiteral("Chosen")));
    fx.home.setDefaultFolderSetting(QUrl());
    CHECK(QDir::fromNativeSeparators(fx.home.defaultFolder().toLocalFile()) == fx.dir.filePath(QStringLiteral("Projects")));
}

TEST_CASE("Home: classifyImport routes files to the right place", "[app][home]") {
    Fixture fx;
    const QString mkv = fx.dir.filePath(QStringLiteral("Ranked match.mkv"));
    const QString wav = fx.dir.filePath(QStringLiteral("Voice.wav"));
    const QString png = fx.dir.filePath(QStringLiteral("Poster.png"));
    const QString srt = fx.dir.filePath(QStringLiteral("Subtitles.srt"));
    const QString lottie = fx.dir.filePath(QStringLiteral("Intro.json"));
    const QString json = fx.dir.filePath(QStringLiteral("data.json"));
    const QString psd = fx.dir.filePath(QStringLiteral("Cover.psd"));
    const QString txt = fx.dir.filePath(QStringLiteral("notes.txt"));
    const QString zip = fx.dir.filePath(QStringLiteral("Archive.zip"));
    const QString drp = fx.dir.filePath(QStringLiteral("Grade.drp"));
    const QString project = fx.dir.filePath(QStringLiteral("Edit.wabcut"));
    writeFile(mkv, QByteArray(2048, '\0'));
    writeFile(wav, QByteArray(1024, '\0'));
    writeFile(png, QByteArray(512, '\0'));
    writeFile(srt, "1\n00:00:01,000 --> 00:00:02,000\nHello\n\n");
    writeFile(lottie, R"({"v":"5.7.4","fr":30,"ip":0,"op":60,"w":512,"h":512,"layers":[]})");
    writeFile(json, R"({"a":1})");
    writeFile(psd, "8BPS");
    writeFile(txt, "hello");
    writeFile(zip, QByteArray("PK\x03\x04", 4) + QByteArray(64, '\0'));
    writeFile(drp, "not a timeline");
    writeFile(project, "{}");
    const QString xml = fixture("fcp7_23976_mac.xml");
    const QString prproj = fixture("prproj/07_transitions.prproj");

    SECTION("a project opens and nothing else is imported") {
        const QVariantMap r = classify(fx.home, {project, mkv});
        CHECK(r.value("route").toString() == QStringLiteral("project"));
        REQUIRE(r.value("projects").toList().size() == 1);
        CHECK(r.value("projects").toList()[0].toMap().value("url").toUrl() == QUrl::fromLocalFile(project));
        CHECK(r.value("notes").toStringList().size() == 1);
    }
    SECTION("timelines with media become a new project") {
        const QVariantMap r = classify(fx.home, {xml, mkv, prproj});
        CHECK(r.value("route").toString() == QStringLiteral("timelines"));
        REQUIRE(r.value("timelines").toList().size() == 1);
        CHECK(r.value("timelines").toList()[0].toMap().value("format").toString() == QStringLiteral("fcp7-xml"));
        CHECK(r.value("media").toList().size() == 1);
        CHECK(r.value("experimental").toList().size() == 1);
        REQUIRE(r.value("notes").toStringList().size() == 1);
        CHECK(r.value("notes").toStringList()[0].contains(QStringLiteral("07_transitions.prproj")));
    }
    SECTION("a Premiere project asks first") {
        const QVariantMap r = classify(fx.home, {prproj});
        CHECK(r.value("route").toString() == QStringLiteral("experimental"));
        REQUIRE(r.value("experimental").toList().size() == 1);
        CHECK_FALSE(r.value("experimental").toList()[0].toMap().value("guidance").toString().isEmpty());
    }
    SECTION("files Wabcut cannot read say why") {
        QVariantMap r = classify(fx.home, {drp});
        CHECK(r.value("route").toString() == QStringLiteral("unsupported"));
        CHECK(r.value("message").toString().contains(QStringLiteral("Grade.drp")));
        CHECK_FALSE(r.value("unsupported").toList()[0].toMap().value("guidance").toString().isEmpty());

        r = classify(fx.home, {txt, zip, json});
        CHECK(r.value("route").toString() == QStringLiteral("unsupported"));
        REQUIRE(r.value("unsupported").toList().size() == 3);
        for (const QVariant& e : r.value("unsupported").toList())
            CHECK_FALSE(e.toMap().value("guidance").toString().isEmpty());
        CHECK(r.value("unsupported").toList()[1].toMap().value("guidance").toString().contains(QStringLiteral("zip")));
        CHECK(r.value("notes").toStringList().size() == 2); // the first is the message
    }
    SECTION("media goes to the New project page") {
        const QVariantMap r = classify(fx.home, {mkv, wav, png, srt, lottie, psd, txt, mkv});
        CHECK(r.value("route").toString() == QStringLiteral("media"));
        const QVariantList media = r.value("media").toList();
        REQUIRE(media.size() == 3); // the second mkv is the same file
        QStringList kinds;
        for (const QVariant& m : media)
            kinds << m.toMap().value("kind").toString();
        CHECK(kinds == QStringList{QStringLiteral("video"), QStringLiteral("audio"), QStringLiteral("image")});
        CHECK_FALSE(media[0].toMap().value("sizeText").toString().isEmpty());
        REQUIRE(r.value("captions").toList().size() == 1);
        CHECK_FALSE(r.value("captions").toList()[0].toMap().value("format").toString().isEmpty());
        CHECK(namesOf(r.value("lottie").toList()) == QStringList{QStringLiteral("Intro.json")});
        CHECK(namesOf(r.value("layered").toList()) == QStringList{QStringLiteral("Cover.psd")});
        CHECK(namesOf(r.value("unsupported").toList()) == QStringList{QStringLiteral("notes.txt")});
        CHECK(r.value("notes").toStringList().size() == 1);
        CHECK(r.value("hasVideo").toBool());
        CHECK(r.value("firstVideo").toUrl() == QUrl::fromLocalFile(mkv));
        CHECK(r.value("suggestedName").toString() == QStringLiteral("Ranked match"));
    }
    SECTION("a dotLottie archive says what to do instead") {
        const QString sticker = fx.dir.filePath(QStringLiteral("sticker.lottie"));
        writeFile(sticker, QByteArray("PK\x03\x04", 4) + QByteArray(64, '\0'));
        QVariantMap r = classify(fx.home, {sticker});
        CHECK(r.value("route").toString() == QStringLiteral("unsupported"));
        CHECK(r.value("lottie").toList().isEmpty());
        CHECK(r.value("message").toString().contains(QStringLiteral("Lottie JSON")));
        // With media: the media start a project, the archive is a note (nothing is made for it alone).
        r = classify(fx.home, {mkv, sticker});
        CHECK(r.value("route").toString() == QStringLiteral("media"));
        CHECK(r.value("lottie").toList().isEmpty());
        CHECK(namesOf(r.value("unsupported").toList()) == QStringList{QStringLiteral("sticker.lottie")});
        CHECK(r.value("notes").toStringList().size() == 1);
        // createFromMedia does not take it either.
        const QVariantMap made = fx.home.createFromMedia(QStringLiteral("Sticker"), fx.folder(), {}, {QUrl::fromLocalFile(sticker)});
        CHECK_FALSE(made.value("ok").toBool());
        CHECK_FALSE(QFile::exists(fx.dir.filePath(QStringLiteral("Sticker.wabcut"))));
    }
    SECTION("web addresses are not files") {
        const QUrl web(QStringLiteral("https://example.com/images/poster.png?size=large"));
        QVariantMap r = fx.home.classifyImport({web});
        CHECK(r.value("route").toString() == QStringLiteral("unsupported"));
        CHECK(r.value("media").toList().isEmpty());
        REQUIRE(r.value("unsupported").toList().size() == 1);
        CHECK(r.value("unsupported").toList()[0].toMap().value("name").toString() == QStringLiteral("poster.png"));
        CHECK(r.value("message").toString().contains(QStringLiteral("web address")));
        r = fx.home.classifyImport({QUrl::fromLocalFile(mkv), web});
        CHECK(r.value("route").toString() == QStringLiteral("media"));
        CHECK(namesOf(r.value("media").toList()) == QStringList{QStringLiteral("Ranked match.mkv")});
        CHECK(r.value("notes").toStringList().size() == 1);
        const QVariantMap made = fx.home.createFromMedia(QStringLiteral("Web"), fx.folder(), {}, {web});
        CHECK_FALSE(made.value("ok").toBool());
        CHECK_FALSE(QFile::exists(fx.dir.filePath(QStringLiteral("Web.wabcut"))));
    }
    SECTION("captions alone start a project too") {
        const QVariantMap r = classify(fx.home, {srt});
        CHECK(r.value("route").toString() == QStringLiteral("media"));
        CHECK(r.value("captions").toList().size() == 1);
        CHECK_FALSE(r.value("hasVideo").toBool());
        CHECK(r.value("suggestedName").toString() == QStringLiteral("Subtitles"));
    }
    SECTION("nothing") {
        CHECK(fx.home.classifyImport({}).value("route").toString() == QStringLiteral("none"));
        CHECK_FALSE(fx.home.classifyImport({}).value("message").toString().isEmpty());
    }
}

TEST_CASE("Home: classifyImport expands folders and keeps only media", "[app][home]") {
    Fixture fx;
    const QString footage = fx.dir.filePath(QStringLiteral("Footage"));
    writeFile(footage + QStringLiteral("/A001.mp4"), QByteArray(256, '\0'));
    writeFile(footage + QStringLiteral("/sub/A002.mov"), QByteArray(256, '\0'));
    writeFile(footage + QStringLiteral("/sub/Theme.wav"), QByteArray(256, '\0'));
    writeFile(footage + QStringLiteral("/sub/readme.txt"), "notes");
    writeFile(footage + QStringLiteral("/sub/Old.wabcut"), "{}");
    writeFile(footage + QStringLiteral("/sub/cut.xml"), "<xmeml/>");
    QDir().mkpath(fx.dir.filePath(QStringLiteral("Empty")));

    const QVariantMap r = classify(fx.home, {footage});
    CHECK(r.value("route").toString() == QStringLiteral("media"));
    CHECK(namesOf(r.value("media").toList()) ==
          QStringList{QStringLiteral("A001.mp4"), QStringLiteral("A002.mov"), QStringLiteral("Theme.wav")});
    CHECK(r.value("projects").toList().isEmpty());
    CHECK(r.value("timelines").toList().isEmpty());
    CHECK(r.value("folders").toInt() == 1);
    CHECK(r.value("skipped").toInt() == 3);
    CHECK_FALSE(r.value("truncated").toBool());
    CHECK(r.value("notes").toStringList().size() == 1);
    CHECK(r.value("suggestedName").toString() == QStringLiteral("A001"));

    const QVariantMap empty = classify(fx.home, {fx.dir.filePath(QStringLiteral("Empty"))});
    CHECK(empty.value("route").toString() == QStringLiteral("none"));
    CHECK_FALSE(empty.value("message").toString().isEmpty());
}

TEST_CASE("Home: starred projects stay in the recent list past its limit", "[app][home]") {
    Fixture fx;
    CHECK(fx.home.recentLimit() == HomeBridge::kMaxRecent);
    const auto urlOf = [&](int i) { return QUrl::fromLocalFile(fx.dir.filePath(QStringLiteral("P%1.wabcut").arg(i))); };
    fx.home.rememberProject(urlOf(0));
    // Starred the way Preferences.qml stores it: the path Home.recent shows, in a JSON list.
    const QString starred = fx.home.recent()[0].toMap().value("path").toString();
    QSettings().setValue(QStringLiteral("home/starred"),
                         QString::fromUtf8(QJsonDocument(QJsonArray{starred}).toJson(QJsonDocument::Compact)));
    for (int i = 1; i <= 60; ++i)
        fx.home.rememberProject(urlOf(i));
    QVariantList recent = fx.home.recent();
    REQUIRE(recent.size() == HomeBridge::kMaxRecent);
    CHECK(recent.front().toMap().value("name").toString() == QStringLiteral("P60"));
    CHECK(recent[HomeBridge::kMaxRecent - 2].toMap().value("name").toString() == QStringLiteral("P12"));
    CHECK(recent.back().toMap().value("name").toString() == QStringLiteral("P0")); // the oldest, kept for its star

    // A lower limit (Settings > General > Projects) lists fewer at once, still the starred one (it hides the others
    // without forgetting them: test_qol_settings_export.cpp).
    fx.home.setRecentLimit(10);
    CHECK(fx.home.recentLimit() == 10);
    recent = fx.home.recent();
    REQUIRE(recent.size() == 10);
    CHECK(recent.back().toMap().value("name").toString() == QStringLiteral("P0"));
    fx.home.setRecentLimit(1);
    CHECK(fx.home.recentLimit() == HomeBridge::kMinRecent);

    // Starred projects that are not listed still get an entry (the Starred filter).
    const QString gone = QDir::toNativeSeparators(fx.dir.filePath(QStringLiteral("Gone.wabcut")));
    const QVariantList entries = fx.home.projectEntries({starred, gone, starred});
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].toMap().value("name").toString() == QStringLiteral("P0"));
    CHECK_FALSE(entries[0].toMap().value("opened").toString().isEmpty());
    CHECK(entries[1].toMap().value("name").toString() == QStringLiteral("Gone"));
    CHECK(entries[1].toMap().value("missing").toBool());
    CHECK(entries[1].toMap().value("openedText").toString().isEmpty());
}

TEST_CASE("Home: removing recent projects can be undone", "[app][home]") {
    Fixture fx;
    for (const char* name : {"A", "B", "C"})
        REQUIRE(fx.home.createProject(QString::fromLatin1(name), fx.folder(), presetNamed(fx.home, "1080p30")).isEmpty());
    const QStringList all{QStringLiteral("C"), QStringLiteral("B"), QStringLiteral("A")};
    REQUIRE(namesOf(fx.home.recent()) == all);
    const QVariantMap b = fx.home.recent()[1].toMap();
    const QString a = fx.home.recent()[2].toMap().value("path").toString();
    CHECK_FALSE(fx.home.canUndoRemove());

    fx.home.removeRecent(b.value("url").toUrl());
    CHECK(namesOf(fx.home.recent()) == QStringList{QStringLiteral("C"), QStringLiteral("A")});
    CHECK(fx.home.canUndoRemove());
    REQUIRE(fx.home.undoRemoveRecent());
    CHECK(namesOf(fx.home.recent()) == all);
    CHECK(fx.home.recent()[1].toMap().value("opened").toString() == b.value("opened").toString());
    CHECK_FALSE(fx.home.canUndoRemove());
    CHECK_FALSE(fx.home.undoRemoveRecent());

    CHECK(fx.home.clearUnstarred({a}) == 2);
    CHECK(namesOf(fx.home.recent()) == QStringList{QStringLiteral("A")});
    REQUIRE(fx.home.undoRemoveRecent());
    CHECK(namesOf(fx.home.recent()) == all);
    CHECK(fx.home.clearUnstarred({}) == 3);
    REQUIRE(fx.home.undoRemoveRecent());

    fx.home.clearRecent();
    CHECK(fx.home.recent().isEmpty());
    REQUIRE(fx.home.undoRemoveRecent());
    CHECK(namesOf(fx.home.recent()) == all);
}

TEST_CASE("Home: Locate points a missing project at its new file", "[app][home]") {
    Fixture fx;
    REQUIRE(fx.home.createProject(QStringLiteral("Moved"), fx.folder(QStringLiteral("Old")), presetNamed(fx.home, "1080p25"))
                .isEmpty());
    REQUIRE(fx.lifecycle.openDemo()); // not open any more
    const QString oldFile = fx.dir.filePath(QStringLiteral("Old/Moved.wabcut"));
    const QString newFile = fx.dir.filePath(QStringLiteral("New/Moved.wabcut"));
    REQUIRE(QDir().mkpath(fx.dir.filePath(QStringLiteral("New"))));
    REQUIRE(QFile::copy(oldFile, newFile));
    REQUIRE(QFile::remove(oldFile));
    fx.home.refreshRecent();
    const QVariantMap before = fx.home.recent()[0].toMap();
    REQUIRE(before.value("missing").toBool());
    // Locate...'s dialog starts in the nearest folder that is still there.
    CHECK(fx.home.existingFolder(QUrl::fromLocalFile(oldFile)).toLocalFile() == fx.dir.filePath(QStringLiteral("Old")));
    CHECK(fx.home.existingFolder(QUrl::fromLocalFile(fx.dir.filePath(QStringLiteral("Gone/Deeper/x.wabcut")))).toLocalFile() ==
          fx.dir.path());

    writeFile(fx.dir.filePath(QStringLiteral("New/readme.txt")), "x");
    const QVariantMap wrong =
        fx.home.locateProject(QUrl::fromLocalFile(oldFile), QUrl::fromLocalFile(fx.dir.filePath(QStringLiteral("New/readme.txt"))));
    CHECK_FALSE(wrong.value("ok").toBool());
    CHECK_FALSE(wrong.value("error").toString().isEmpty());

    const QVariantMap located = fx.home.locateProject(QUrl::fromLocalFile(oldFile), QUrl::fromLocalFile(newFile));
    REQUIRE(located.value("ok").toBool());
    CHECK(located.value("path").toString() == QDir::toNativeSeparators(newFile));
    CHECK(located.value("oldPath").toString() == QDir::toNativeSeparators(oldFile));
    const QVariantList recent = fx.home.recent();
    REQUIRE(recent.size() == 1);
    CHECK(recent[0].toMap().value("path").toString() == QDir::toNativeSeparators(newFile));
    CHECK_FALSE(recent[0].toMap().value("missing").toBool());
    CHECK(recent[0].toMap().value("format").toString() == before.value("format").toString());
    CHECK(recent[0].toMap().value("opened").toString() == before.value("opened").toString());

    // A starred project that left the list (Clear Recent; the Starred filter lists it from its star) is located too:
    // QML moves the star to the file found; the list is left as it is.
    const QString lostFile = fx.dir.filePath(QStringLiteral("Old/Lost.wabcut"));
    const QString foundFile = fx.dir.filePath(QStringLiteral("New/Lost.wabcut"));
    writeFile(foundFile, "{}");
    fx.home.clearRecent();
    const QVariantMap lost = fx.home.locateProject(QUrl::fromLocalFile(lostFile), QUrl::fromLocalFile(foundFile));
    INFO(lost.value("error").toString().toStdString());
    REQUIRE(lost.value("ok").toBool());
    CHECK(lost.value("path").toString() == QDir::toNativeSeparators(foundFile));
    CHECK(lost.value("oldPath").toString() == QDir::toNativeSeparators(lostFile));
    CHECK(lost.value("name").toString() == QStringLiteral("Lost"));
    CHECK(fx.home.recent().isEmpty());
}

TEST_CASE("Home: a project folder that cannot be made is refused before anything is created", "[app][home]") {
    Fixture fx;
    // A file where a folder of the path should be: said at once (validateProject: the page shows it live), and Create
    // refuses it the same way.
    writeFile(fx.dir.filePath(QStringLiteral("Notes.txt")), "x");
    const QUrl underFile = fx.folder(QStringLiteral("Notes.txt/Projects"));
    const QString why = fx.home.validateProject(QStringLiteral("Film"), underFile);
    CHECK(why.contains(QStringLiteral("Notes.txt")));
    CHECK(fx.home.createProject(QStringLiteral("Film"), underFile, presetNamed(fx.home, "1080p25")) == why);
#ifdef Q_OS_WIN
    // A drive that is not there (an unplugged disk set as the default location).
    QString missingDrive;
    for (char letter = 'Z'; letter >= 'D' && missingDrive.isEmpty(); --letter) {
        const QString root = QStringLiteral("%1:/").arg(QLatin1Char(letter));
        if (!QFileInfo::exists(root))
            missingDrive = root;
    }
    if (!missingDrive.isEmpty()) {
        INFO(missingDrive.toStdString());
        CHECK(fx.home.validateProject(QStringLiteral("Film"), QUrl::fromLocalFile(missingDrive + QStringLiteral("Projects")))
                  .contains(QStringLiteral("not available")));
    }
    // A folder the system will not make (a name Windows refuses): validateProject cannot tell, Create says so.
    const QUrl refused = fx.folder(QStringLiteral("bad<folder"));
    CHECK(fx.home.validateProject(QStringLiteral("Film"), refused).isEmpty());
    CHECK(fx.home.createProject(QStringLiteral("Film"), refused, presetNamed(fx.home, "1080p25"))
              .contains(QStringLiteral("Cannot create the folder")));
#endif
    CHECK(fx.lifecycle.demo()); // nothing was created
    CHECK(fx.home.recent().isEmpty());
    // A folder still to be made under one that is there is fine (<Movies>/Wabcut on a first run).
    CHECK(fx.home.validateProject(QStringLiteral("Film"), fx.folder(QStringLiteral("New/Deeper"))).isEmpty());
}

TEST_CASE("Home: Show in Explorer and Copy Path", "[app][home]") {
    Fixture fx;
    RevealerGuard guard;
    QStringList revealed;
    LibraryBridge::setRevealer([&](const QString& path) {
        revealed << path;
        return true;
    });
    REQUIRE(fx.home.createProject(QStringLiteral("Shown"), fx.folder(), presetNamed(fx.home, "1080p30")).isEmpty());
    const QString file = fx.dir.filePath(QStringLiteral("Shown.wabcut"));
    CHECK(fx.home.revealProject(QUrl::fromLocalFile(file)));
    REQUIRE(revealed.size() == 1);
    CHECK(QDir::fromNativeSeparators(revealed[0]) == file);
    CHECK_FALSE(fx.home.revealProject(QUrl::fromLocalFile(fx.dir.filePath(QStringLiteral("Gone.wabcut")))));
    CHECK(revealed.size() == 1);

    const QString copied = fx.home.copyPath(QUrl::fromLocalFile(file));
    CHECK(copied == QDir::toNativeSeparators(file));
    CHECK(QGuiApplication::clipboard()->text() == copied);
}

TEST_CASE("Home: links lead to the website", "[app][home]") {
    Fixture fx;
    const QString site = QStringLiteral("https://wabcut.zyphite.com");
    CHECK(fx.home.websiteUrl() == QUrl(site));
    CHECK(fx.home.guideUrl(QString()) == QUrl(site + QStringLiteral("/guide")));
    CHECK(fx.home.guideUrl(QStringLiteral("first-run")) == QUrl(site + QStringLiteral("/guide/first-run")));
    CHECK(fx.home.guideUrl(QStringLiteral("/guide/install")) == QUrl(site + QStringLiteral("/guide/install")));
    CHECK(fx.home.guideUrl(QStringLiteral("https://example.com/x")) == QUrl(QStringLiteral("https://example.com/x")));

    QStringList ids;
    QStringList guide;
    for (const QVariant& value : fx.home.links()) {
        const QVariantMap link = value.toMap();
        ids << link.value("id").toString();
        if (link.value("group").toString() == QLatin1String("guide"))
            guide << link.value("id").toString();
        CHECK_FALSE(link.value("title").toString().isEmpty());
        CHECK_FALSE(link.value("detail").toString().isEmpty());
        CHECK(link.value("url").toUrl().scheme() == QStringLiteral("https"));
    }
    CHECK(guide == QStringList{QStringLiteral("gettingStarted"), QStringLiteral("theWindow"), QStringLiteral("fromPremiere"),
                               QStringLiteral("exchangeTimelines"), QStringLiteral("shortcuts"), QStringLiteral("gamingVideo"),
                               QStringLiteral("shorts"), QStringLiteral("videoEssay"), QStringLiteral("documentary"),
                               QStringLiteral("allGuides")});
    CHECK(ids.contains(QStringLiteral("releaseNotes")));
    CHECK(ids.contains(QStringLiteral("reportIssue")));
    for (const QVariant& value : fx.home.links()) {
        const QVariantMap link = value.toMap();
        if (link.value("id").toString() == QLatin1String("gettingStarted"))
            CHECK(link.value("url").toUrl() == QUrl(site + QStringLiteral("/guide/first-run")));
        if (link.value("id").toString() == QLatin1String("reportIssue"))
            CHECK(link.value("url").toUrl() == QUrl(QStringLiteral("https://github.com/sorsama/wabcut/issues")));
    }
}

TEST_CASE("Home: the import dialog filters list every kind Home imports", "[app][home]") {
    Fixture fx;
    const QStringList filters = fx.home.importNameFilters();
    REQUIRE(filters.size() == 5);
    CHECK(filters[0].startsWith(QStringLiteral("All supported")));
    for (const char* pattern : {"*.wabcut", "*.mp4", "*.flv", "*.ts", "*.m2ts", "*.wmv", "*.psd", "*.json", "*.xml",
                                "*.fcpxml", "*.otio", "*.edl", "*.prproj", "*.srt", "*.wav", "*.png"}) {
        INFO(pattern);
        CHECK(filters[0].contains(QString::fromLatin1(pattern)));
    }
    CHECK_FALSE(filters[0].contains(QStringLiteral("*.lottie"))); // a dotLottie archive is not imported (it gets guidance)
    CHECK(filters[1].startsWith(QStringLiteral("Media")));
    CHECK(filters[2].startsWith(QStringLiteral("Timelines")));
    CHECK(filters[3] == QStringLiteral("Wabcut projects (*.wabcut)"));
    CHECK(filters[4] == QStringLiteral("All files (*)"));
    CHECK(fx.home.projectNameFilters().first() == QStringLiteral("Wabcut projects (*.wabcut)"));
}

// ---- In the shell ---------------------------------------------------------------------------------------------------

namespace {

QRectF sceneRect(QQuickItem* item) {
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

/// A real left click at the centre of `item`.
void clickItem(Shell& shell, QQuickItem* item) {
    REQUIRE(item != nullptr);
    polishWindow(shell);
    const QPointF p = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    QMouseEvent press(QEvent::MouseButtonPress, p, p, Qt::LeftButton, Qt::LeftButton, {});
    QCoreApplication::sendEvent(shell.window.get(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, p, p, Qt::LeftButton, Qt::NoButton, {});
    QCoreApplication::sendEvent(shell.window.get(), &release);
}

void tapKey(Shell& shell, int key, Qt::KeyboardModifiers mods = {}, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, key, mods, text);
    QCoreApplication::sendEvent(shell.window.get(), &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods, text);
    QCoreApplication::sendEvent(shell.window.get(), &release);
}

QString focusedName(Shell& shell) {
    QQuickItem* f = shell.window->activeFocusItem();
    return f ? f->objectName() : QString();
}

/// The focused item for a failure message: its objectName (or type) and its parents'.
std::string focusTrail(Shell& shell) {
    QStringList trail;
    for (QQuickItem* f = shell.window->activeFocusItem(); f && trail.size() < 6; f = f->parentItem())
        trail << (f->objectName().isEmpty() ? QString::fromLatin1(f->metaObject()->className()) : f->objectName());
    return trail.join(QLatin1String(" < ")).toStdString();
}

/// Three recent projects: two files that exist (the first with `thumbnail` as its picture), one that is gone.
QStringList seedRecent(const QTemporaryDir& dir, const QString& thumbnail = {}) {
    const auto touch = [&](const QString& name) {
        QFile f(dir.filePath(name));
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("{}");
        return dir.filePath(name);
    };
    const QDateTime now = QDateTime::currentDateTime();
    const QStringList paths{touch(QStringLiteral("Brand Film - Final.wabcut")), touch(QStringLiteral("Interview Selects.wabcut")),
                            dir.filePath(QStringLiteral("Old Promo.wabcut"))};
    QSettings().setValue(QStringLiteral("home/recentProjects"), QVariantList{
        QVariantMap{{"path", paths[0]}, {"opened", now.toString(Qt::ISODate)}, {"format", QStringLiteral("1080p23.976")},
                    {"thumbnail", thumbnail.isEmpty() ? QString() : QUrl::fromLocalFile(thumbnail).toString()}},
        QVariantMap{{"path", paths[1]}, {"opened", now.addDays(-1).toString(Qt::ISODate)}, {"format", QStringLiteral("2160p25")},
                    {"thumbnail", QString()}},
        QVariantMap{{"path", paths[2]}, {"opened", now.addDays(-9).toString(Qt::ISODate)}, {"format", QStringLiteral("1080p29.97")},
                    {"thumbnail", QString()}},
    });
    return paths;
}

/// Each item is shown and lies inside the window: nothing cut off, no page that would need scrolling.
void checkInside(Shell& shell, const QStringList& names, int width, int height) {
    polishWindow(shell);
    for (const QString& name : names) {
        INFO(name.toStdString() << " at " << width << "x" << height);
        QQuickItem* item = visibleItem(shell, name);
        REQUIRE(item != nullptr);
        const QRectF r = sceneRect(item);
        CHECK(r.left() >= -0.5);
        CHECK(r.top() >= -0.5);
        CHECK(r.right() <= width + 0.5);
        CHECK(r.bottom() <= height + 0.5);
    }
}

void showPage(QQuickItem* screen, const char* key) {
    REQUIRE(QMetaObject::invokeMethod(screen, "showPage", Q_ARG(QString, QString::fromLatin1(key))));
    spin(80);
}

QStringList shownNames(QObject* projects) {
    QStringList names;
    for (const QVariant& p : variantList(projects->property("shown")))
        names << p.toMap().value(QStringLiteral("name")).toString();
    return names;
}

QVariantMap jsMap(const QVariant& value) {
    if (value.metaType() == QMetaType::fromType<QJSValue>())
        return value.value<QJSValue>().toVariant().toMap();
    return value.toMap();
}

/// The messages a test must not cause (the clipboard's "Unable to obtain clipboard" comes from another test process
/// holding it, under parallel runs).
QStringList ownMessages() {
    QStringList messages = takeMessages();
    messages.removeIf([](const QString& m) { return m.contains(QLatin1String("clipboard")); });
    return messages;
}

void saveShot(Shell& shell, const char* name) {
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    REQUIRE(image.save(outputDir() + QLatin1Char('/') + QString::fromLatin1(name)));
}

} // namespace

TEST_CASE("Home screen in the shell: File > Home then drop an FCP7 XML - the sequence opens with the report; screenshots",
          "[app][home][screenshots][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* home = shell.engine.singletonInstance<HomeBridge*>("Wabcut", "Home");
    REQUIRE(controller != nullptr);
    REQUIRE(home != nullptr);
    CHECK_FALSE(home->showAtLaunch()); // the tests start on the editor
    QQuickItem* screen = shell.item("homeScreen");
    REQUIRE(screen != nullptr);
    CHECK_FALSE(screen->property("opened").toBool());

    // Recent projects to show: two files that exist (one with a picture of the editor as its thumbnail), one gone.
    QTemporaryDir dir;
    const QImage editor = shell.grab();
    REQUIRE_FALSE(editor.isNull());
    QQuickItem* monitor = shell.item("monitorPanel");
    REQUIRE(monitor != nullptr);
    const double dpr = shell.window->effectiveDevicePixelRatio();
    const QRectF monitorRect = sceneRect(monitor);
    const QString thumbFile = dir.filePath(QStringLiteral("thumb.png"));
    REQUIRE(editor.copy(QRectF(monitorRect.topLeft() * dpr, monitorRect.size() * dpr).toRect()).save(thumbFile));
    seedRecent(dir, thumbFile);

    // File > Home: the Home page - Continue, Start and Recent - fits the window; nothing scrolls.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showHome"));
    spin(400);
    CHECK(screen->property("opened").toBool());
    CHECK(screen->isVisible());
    CHECK(screen->property("page").toString() == QStringLiteral("home"));
    QQuickItem* editorShell = shell.item("editorShell");
    REQUIRE(editorShell != nullptr);
    CHECK_FALSE(editorShell->isEnabled());
    CHECK_FALSE(editorShell->isVisible()); // covered: not drawn
    REQUIRE(home->recent().size() == 3);
    CHECK(home->recent()[2].toMap().value("missing").toBool());
    checkInside(shell, {QStringLiteral("homeContinueCard"), QStringLiteral("homeStartCard"), QStringLiteral("homeRecentCard"),
                        QStringLiteral("homeRecent_0"), QStringLiteral("homeDropZone"), QStringLiteral("homeImportButton"),
                        QStringLiteral("homeNewButton"), QStringLiteral("homeOpenButton"), QStringLiteral("homeSearch")},
                kShotWidth, kShotHeight);
    QQuickItem* continueName = visibleItem(shell, QStringLiteral("homeContinueName"));
    REQUIRE(continueName != nullptr);
    CHECK(continueName->property("text").toString() == QStringLiteral("Brand Film - Final"));
    CHECK(visibleItem(shell, QStringLiteral("homeContinueReveal")) != nullptr);
    // File > Home from the sample project: Back and Esc go back to it (the user was in it).
    QQuickItem* back = visibleItem(shell, QStringLiteral("homeBackButton"));
    REQUIRE(back != nullptr);
    CHECK(back->property("text").toString() == QStringLiteral("Back to the sample project"));
    saveShot(shell, "home.png");
    tapKey(shell, Qt::Key_Escape);
    spin(300);
    CHECK_FALSE(screen->property("opened").toBool());
    CHECK(demoShown(shell));
    // File > New Project from it: Esc (as Cancel) goes back to the editor, not to Home's Home page.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "newProject"));
    spin(300);
    REQUIRE(screen->property("opened").toBool());
    CHECK(screen->property("page").toString() == QStringLiteral("new"));
    tapKey(shell, Qt::Key_Escape);
    spin(300);
    CHECK_FALSE(screen->property("opened").toBool());
    CHECK(demoShown(shell));
    // At launch (and after Close Project) the sample nobody asked for is behind Home: no Back, and Esc keeps Home.
    REQUIRE(QMetaObject::invokeMethod(screen, "open"));
    spin(300);
    CHECK(visibleItem(shell, QStringLiteral("homeBackButton")) == nullptr);
    tapKey(shell, Qt::Key_Escape);
    spin(50);
    CHECK(screen->property("opened").toBool());

    // A .prproj waits for the experimental reader, clearly labelled.
    REQUIRE(QMetaObject::invokeMethod(screen, "handleUrls",
                                      Q_ARG(QVariant, QVariant::fromValue(QList<QUrl>{QUrl::fromLocalFile(fixture("prproj/07_transitions.prproj"))}))));
    spin(100);
    QQuickItem* prompt = visibleItem(shell, QStringLiteral("homeExperimentalPrompt"));
    REQUIRE(prompt != nullptr);
    CHECK(prompt->property("title").toString().contains(QStringLiteral("07_transitions.prproj")));
    CHECK(screen->property("opened").toBool());
    saveShot(shell, "home-prproj.png");
    // Cancel (a real click): nothing is imported.
    clickItem(shell, visibleItem(shell, QStringLiteral("homeExperimentalCancel")));
    spin(100);
    CHECK(visibleItem(shell, QStringLiteral("homeExperimentalPrompt")) == nullptr);
    CHECK(screen->property("opened").toBool());
    CHECK(demoShown(shell));

    // A DaVinci Resolve project: what to export instead, with the guide link; nothing imported.
    {
        QFile drp(dir.filePath(QStringLiteral("Grade.drp")));
        REQUIRE(drp.open(QIODevice::WriteOnly));
        drp.write("PK\x03\x04 not really");
    }
    REQUIRE(QMetaObject::invokeMethod(screen, "handleUrls",
                                      Q_ARG(QVariant, QVariant::fromValue(QList<QUrl>{QUrl::fromLocalFile(dir.filePath(QStringLiteral("Grade.drp")))}))));
    spin(100);
    QQuickItem* guidance = visibleItem(shell, QStringLiteral("homeImportMessage"));
    REQUIRE(guidance != nullptr);
    CHECK(guidance->property("message").toString().contains(QStringLiteral("Grade.drp")));
    CHECK(visibleItem(shell, QStringLiteral("homeImportHowTo")) != nullptr);
    CHECK(demoShown(shell));
    clickItem(shell, visibleItem(shell, QStringLiteral("homeImportMessageClose")));
    spin(50);
    CHECK(visibleItem(shell, QStringLiteral("homeImportMessage")) == nullptr);

    // Drag an FCP7 XML over the drop well: it lights up.
    QQuickItem* zone = visibleItem(shell, QStringLiteral("homeDropZone"));
    REQUIRE(zone != nullptr);
    const QPoint at = zone->mapToScene(QPointF(zone->width() / 2, zone->height() / 2)).toPoint();
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(fixture("fcp7_23976_mac.xml"))});
    QDragEnterEvent enter(at, Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(shell.window.get(), &enter);
    QDragMoveEvent move(at, Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(shell.window.get(), &move);
    spin(150);
    QObject* start = shell.object("homeStartCard");
    REQUIRE(start != nullptr);
    CHECK(start->property("dragActive").toBool());
    CHECK(visibleItem(shell, QStringLiteral("homeDropOverlay")) == nullptr); // the Home page has its well
    saveShot(shell, "home-import.png");

    // Drop: a new project with the imported sequence open, and the import report.
    QDropEvent dropped(at, Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(shell.window.get(), &dropped);
    REQUIRE(waitFor([&] { return !screen->property("opened").toBool(); }, 5000));
    CHECK(controller->sequenceNames() == QStringList{QStringLiteral("Reel 2 Assembly")});
    CHECK(controller->sequenceName() == QStringLiteral("Reel 2 Assembly"));
    QObject* report = shell.object("importReportDialog");
    REQUIRE(report != nullptr);
    CHECK(report->property("opened").toBool());
    CHECK(editorShell->isEnabled());
    saveShot(shell, "home-import-report.png");
    QMetaObject::invokeMethod(report, "close");
    spin(100);

    // 1100x700, the smallest window: the Home page still fits, and a real project is behind it now.
    shell.resize(1100, 700);
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showHome"));
    spin(400);
    checkInside(shell, {QStringLiteral("homeContinueCard"), QStringLiteral("homeStartCard"), QStringLiteral("homeRecentCard"),
                        QStringLiteral("homeRecent_0"), QStringLiteral("homeDropZone"), QStringLiteral("homeBackButton"),
                        QStringLiteral("homeNewButton"), QStringLiteral("homeImportButton")},
                1100, 700);
    saveShot(shell, "home-1100x700.png");
    // Esc goes back to the project.
    tapKey(shell, Qt::Key_Escape);
    spin(300);
    CHECK_FALSE(screen->property("opened").toBool());

    const QStringList messages = ownMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Home screen pages: the nav and the keys switch Home - Projects - Learn - New project; each fits at 1100x700 and 1600x960; accessibility; screenshots",
          "[app][home][screenshots][qml]") {
    A11yScope a11y;
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* home = shell.engine.singletonInstance<HomeBridge*>("Wabcut", "Home");
    REQUIRE(home != nullptr);
    QTemporaryDir dir;
    seedRecent(dir);
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showHome"));
    spin(400);
    QQuickItem* screen = shell.item("homeScreen");
    REQUIRE(screen != nullptr);
    REQUIRE(screen->property("opened").toBool());
    const auto audit = [&](const char* name) {
        QQuickItem* item = visibleItem(shell, QString::fromLatin1(name));
        REQUIRE(item != nullptr);
        const QStringList issues = auditAccessibility(item);
        INFO(name << ": " << issues.join(QLatin1Char('\n')).toStdString());
        CHECK(issues.isEmpty());
    };
    audit("homePage");
    audit("homeNav");
    {
        // The header: every control named.
        const QStringList issues = auditAccessibility(screen, {QStringLiteral("homePages"), QStringLiteral("homeNav")});
        INFO(issues.join(QLatin1Char('\n')).toStdString());
        CHECK(issues.isEmpty());
    }
    CHECK(tabReaches(screen, QStringLiteral("homeSearch")));
    CHECK(tabReaches(screen, QStringLiteral("homeImportButton")));
    CHECK(tabReaches(screen, QStringLiteral("homeStartNew")));
    CHECK(tabReaches(screen, QStringLiteral("homeDropZone")));
    CHECK(tabReaches(screen, QStringLiteral("homeRecent_0")));
    // A card's star and its "..." take Tab after it ("..." shows while the star has the focus).
    REQUIRE(pressWithKey(shell.window.get(), visibleItem(shell, QStringLiteral("homeRecent_0")), Qt::Key_Tab));
    spin(50);
    CHECK(focusedName(shell) == QStringLiteral("homeRecentStar"));
    tapKey(shell, Qt::Key_Tab);
    spin(50);
    CHECK(focusedName(shell) == QStringLiteral("homeProjectMore"));

    // The nav: Down moves to Projects, Enter opens it.
    QQuickItem* navHome = visibleItem(shell, QStringLiteral("homeNav_home"));
    REQUIRE(navHome != nullptr);
    REQUIRE(pressWithKey(shell.window.get(), navHome, Qt::Key_Down));
    spin(50);
    CHECK(focusedName(shell) == QStringLiteral("homeNav_projects"));
    CHECK(screen->property("page").toString() == QStringLiteral("home"));
    tapKey(shell, Qt::Key_Return);
    spin(150);
    CHECK(screen->property("page").toString() == QStringLiteral("projects"));
    CHECK(visibleItem(shell, QStringLiteral("homePage")) == nullptr);
    REQUIRE(visibleItem(shell, QStringLiteral("homeProjectsPage")) != nullptr);
    REQUIRE(visibleItem(shell, QStringLiteral("homeRecent_2")) != nullptr);
    QQuickItem* title = visibleItem(shell, QStringLiteral("homePageTitle"));
    REQUIRE(title != nullptr);
    CHECK(title->property("text").toString() == QStringLiteral("Projects"));
    audit("homeProjectsPage");
    saveShot(shell, "home-projects.png");

    // Learn: Space on its entry; every website link and the Start here rows.
    REQUIRE(pressWithKey(shell.window.get(), visibleItem(shell, QStringLiteral("homeNav_learn")), Qt::Key_Space, QStringLiteral(" ")));
    spin(150);
    CHECK(screen->property("page").toString() == QStringLiteral("learn"));
    for (const QVariant& link : home->links()) {
        const QString id = link.toMap().value(QStringLiteral("id")).toString();
        INFO(id.toStdString());
        CHECK(visibleItem(shell, QStringLiteral("homeLink_") + id) != nullptr);
    }
    for (const char* row : {"homeSampleProject", "homeLearnTour", "homeLearnTrainer", "homeLearnWhatsNew"})
        CHECK(visibleItem(shell, QString::fromLatin1(row)) != nullptr);
    audit("homeLearnPage");
    saveShot(shell, "home-learn.png");

    // Ctrl+N: the New project page, the name ready to type, a free default name and the first preset.
    tapKey(shell, Qt::Key_N, Qt::ControlModifier);
    spin(150);
    CHECK(screen->property("page").toString() == QStringLiteral("new"));
    QQuickItem* newPage = visibleItem(shell, QStringLiteral("homeNewProjectPage"));
    REQUIRE(newPage != nullptr);
    REQUIRE(waitFor([&] { return focusedName(shell) == QStringLiteral("homeProjectName"); }, 2000));
    QQuickItem* name = visibleItem(shell, QStringLiteral("homeProjectName"));
    REQUIRE(name != nullptr);
    CHECK(name->property("text").toString().startsWith(QStringLiteral("Untitled Project")));
    for (const QVariant& p : home->presets()) {
        const QString id = p.toMap().value(QStringLiteral("id")).toString();
        INFO(id.toStdString());
        CHECK(visibleItem(shell, QStringLiteral("homePreset_") + id) != nullptr);
    }
    CHECK(visibleItem(shell, QStringLiteral("homePreset_match")) == nullptr); // no media: no Match my media
    QQuickItem* summary = visibleItem(shell, QStringLiteral("homeSequenceSummary"));
    REQUIRE(summary != nullptr);
    CHECK(summary->property("text").toString() ==
          QStringLiteral("1920 × 1080 · 23.976 fps · 48 kHz stereo · 3 video / 3 audio tracks"));
    CHECK(visibleItem(shell, QStringLiteral("homeAdvancedSettings")) == nullptr); // closed by default
    CHECK(visibleItem(shell, QStringLiteral("homeNewProjectError")) == nullptr);
    audit("homeNewProjectPage");
    saveShot(shell, "home-new.png");

    // The tiles are one radio group: Right chooses the next one and takes the focus.
    REQUIRE(pressWithKey(shell.window.get(), visibleItem(shell, QStringLiteral("homePreset_1080p23976")), Qt::Key_Right));
    spin(50);
    CHECK(newPage->property("presetId").toString() == QStringLiteral("1080p25"));
    CHECK(focusedName(shell) == QStringLiteral("homePreset_1080p25"));
    CHECK(summary->property("text").toString().startsWith(QStringLiteral("1920 × 1080 · 25 fps")));

    // Advanced settings (Space on the disclosure): every row, remembered open.
    REQUIRE(pressWithKey(shell.window.get(), visibleItem(shell, QStringLiteral("homeAdvancedToggle")), Qt::Key_Space, QStringLiteral(" ")));
    spin(100);
    REQUIRE(visibleItem(shell, QStringLiteral("homeAdvancedSettings")) != nullptr);
    CHECK(shell.preferences->property("homeAdvancedOpen").toBool());
    for (const char* row : {"homeFrameWidth", "homeFrameHeight", "homeSwapOrientation", "homeFrameRate", "homePixelAspect",
                            "homeFields", "homeStartTimecode", "homeAudioRate", "homeMasterFormat", "homeVideoTracks",
                            "homeAudioTracks", "homeTrackFormat", "homeColorManagement", "homePreviewFormat",
                            "homeAdvancedReset"}) {
        INFO(row);
        CHECK(findItem(newPage, [&](QQuickItem* i) { return i->objectName() == QLatin1String(row) && i->isVisible(); }) != nullptr);
    }
    CHECK(visibleItem(shell, QStringLiteral("homeDropFrame")) == nullptr); // 25 fps has no drop-frame timecode
    audit("homeNewProjectPage");
    for (const char* control : {"homeProjectName", "homeBrowseFolder", "homeAdvancedToggle", "homeFrameRate",
                                "homeAudioTracks", "homeCancelButton", "homeCreateButton"}) {
        INFO(control << ": " << describeTabChain(newPage).toStdString());
        CHECK(tabReaches(newPage, QString::fromLatin1(control)));
    }
    saveShot(shell, "home-new-advanced.png");

    // Esc leaves the New project page for the page it came from.
    tapKey(shell, Qt::Key_Escape);
    spin(100);
    CHECK(screen->property("page").toString() == QStringLiteral("learn"));
    CHECK(screen->property("opened").toBool());

    // Each page at both sizes: its islands and the header inside the window (no page scroll, nothing cut off).
    const QStringList header{QStringLiteral("homeSearch"), QStringLiteral("homeOpenButton"), QStringLiteral("homeImportButton"),
                             QStringLiteral("homeSettingsButton")};
    for (const QSize size : {QSize(1100, 700), QSize(1600, 960)}) {
        shell.resize(size.width(), size.height());
        spin(100);
        showPage(screen, "home");
        checkInside(shell, header + QStringList{QStringLiteral("homeContinueCard"), QStringLiteral("homeStartCard"),
                                                QStringLiteral("homeRecentCard"), QStringLiteral("homeNewButton")},
                    size.width(), size.height());
        showPage(screen, "projects");
        checkInside(shell, header + QStringList{QStringLiteral("homeProjectsPage"), QStringLiteral("homeProjectFilter"),
                                                QStringLiteral("homeProjectsMenuButton"), QStringLiteral("homeRecent_0")},
                    size.width(), size.height());
        showPage(screen, "learn");
        checkInside(shell, header + QStringList{QStringLiteral("homeLearnPage"), QStringLiteral("homeLearnStart"),
                                                QStringLiteral("homeLearnGuide"), QStringLiteral("homeSampleProject")},
                    size.width(), size.height());
        showPage(screen, "new");
        checkInside(shell, header + QStringList{QStringLiteral("homeNewProjectPage"), QStringLiteral("homeFormatCard"),
                                                QStringLiteral("homeProjectDetails"), QStringLiteral("homeCreateButton"),
                                                QStringLiteral("homeAdvancedToggle"), QStringLiteral("homeNewBack")},
                    size.width(), size.height());
        if (size.width() == 1100)
            saveShot(shell, "home-new-1100x700.png");
        tapKey(shell, Qt::Key_Escape);
        spin(50);
    }

    // The light theme.
    shell.setAppearance(false, "ultraviolet");
    REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    showPage(screen, "home");
    saveShot(shell, "home-light.png");

    // Ctrl+, (Cmd+, on macOS) opens Settings over Home, as in the editor (the keymap's shortcuts are off here).
    screen->forceActiveFocus();
    tapKey(shell, Qt::Key_Comma, Qt::ControlModifier, QStringLiteral(","));
    QObject* settingsLoader = shell.object("settingsWindowLoader");
    REQUIRE(settingsLoader != nullptr);
    QObject* settings = settingsLoader->property("item").value<QObject*>();
    REQUIRE(settings != nullptr);
    CHECK(waitFor([&] { return settings->property("opened").toBool(); }, 3000));
    REQUIRE(QMetaObject::invokeMethod(settings, "close"));
    spin(200);
    CHECK(screen->property("opened").toBool());

    const QStringList messages = ownMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Home screen Projects page: search and star and filter and sort and view persist; the project menu and keys; Undo",
          "[app][home][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* home = shell.engine.singletonInstance<HomeBridge*>("Wabcut", "Home");
    REQUIRE(home != nullptr);
    QTemporaryDir dir;
    const QStringList paths = seedRecent(dir);
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showHome"));
    spin(400);
    QQuickItem* screen = shell.item("homeScreen");
    REQUIRE(screen != nullptr);
    REQUIRE(home->recent().size() == 3);
    CHECK_FALSE(screen->property("greeting").toString().isEmpty());

    // Typing in the search on the Home page opens Projects, narrowed by name (any case).
    QQuickItem* search = visibleItem(shell, QStringLiteral("homeSearch"));
    REQUIRE(search != nullptr);
    search->forceActiveFocus(Qt::TabFocusReason);
    tapKey(shell, Qt::Key_I, Qt::NoModifier, QStringLiteral("I"));
    spin(100);
    CHECK(screen->property("page").toString() == QStringLiteral("projects"));
    QObject* projects = shell.object("homeProjectsPage");
    REQUIRE(projects != nullptr);
    search->setProperty("text", QStringLiteral("INTERVIEW"));
    spin(50);
    CHECK(shownNames(projects) == QStringList{QStringLiteral("Interview Selects")});
    search->setProperty("text", QStringLiteral("no such project"));
    spin(50);
    CHECK(shownNames(projects).isEmpty());
    CHECK(visibleItem(shell, QStringLiteral("homeProjectsEmpty")) != nullptr);
    search->setProperty("text", QString());
    spin(50);
    CHECK(shownNames(projects).size() == 3);

    // Star the second project (a click on its star): it persists.
    QQuickItem* second = visibleItem(shell, QStringLiteral("homeRecent_1"));
    REQUIRE(second != nullptr);
    QQuickItem* star = findItem(second, [](QQuickItem* i) { return i->objectName() == QStringLiteral("homeRecentStar"); });
    REQUIRE(star != nullptr);
    CHECK_FALSE(star->property("starred").toBool());
    clickItem(shell, star);
    spin(50);
    CHECK(star->property("starred").toBool());
    CHECK(projects->property("starredCount").toInt() == 1);
    const QJsonArray starred = QJsonDocument::fromJson(shell.preferences->property("homeStarred").toString().toUtf8()).array();
    REQUIRE(starred.size() == 1);
    CHECK(QDir::fromNativeSeparators(starred[0].toString()) == paths[1]);
    CHECK(waitFor([] { return QSettings().value(QStringLiteral("home/starred")).toString().contains(QStringLiteral("Interview Selects")); },
                  3000));

    // Starred: only it - and still after it left the recent list (stars outlive the list's limit).
    REQUIRE(QMetaObject::invokeMethod(projects, "setFilter", Q_ARG(QString, QStringLiteral("starred"))));
    spin(50);
    CHECK(shownNames(projects) == QStringList{QStringLiteral("Interview Selects")});
    home->removeRecent(QUrl::fromLocalFile(paths[1]));
    spin(50);
    CHECK(home->recent().size() == 2);
    CHECK(shownNames(projects) == QStringList{QStringLiteral("Interview Selects")});
    REQUIRE(home->undoRemoveRecent());
    spin(50);
    CHECK(shell.preferences->property("homeProjectFilter").toString() == QStringLiteral("starred"));

    // Sort by name, the list view: remembered.
    REQUIRE(QMetaObject::invokeMethod(projects, "setFilter", Q_ARG(QString, QStringLiteral("all"))));
    REQUIRE(QMetaObject::invokeMethod(projects, "setSort", Q_ARG(QString, QStringLiteral("name"))));
    spin(50);
    CHECK(shownNames(projects) == (QStringList{QStringLiteral("Brand Film - Final"), QStringLiteral("Interview Selects"),
                                               QStringLiteral("Old Promo")}));
    clickItem(shell, visibleItem(shell, QStringLiteral("homeListView")));
    spin(100);
    CHECK(visibleItem(shell, QStringLiteral("homeRecentRows")) != nullptr);
    CHECK(visibleItem(shell, QStringLiteral("homeRecentList")) == nullptr);
    REQUIRE(visibleItem(shell, QStringLiteral("homeRecent_0")) != nullptr);
    CHECK(waitFor([] { return QSettings().value(QStringLiteral("home/projectView")).toString() == QStringLiteral("list")
                              && QSettings().value(QStringLiteral("home/projectSort")).toString() == QStringLiteral("name"); },
                  3000));
    saveShot(shell, "home-projects-list.png");
    {
        A11yScope a11y;
        const QStringList issues = auditAccessibility(visibleItem(shell, QStringLiteral("homeProjectsPage")));
        INFO(issues.join(QLatin1Char('\n')).toStdString());
        CHECK(issues.isEmpty());
    }

    // Keys on the rows: Down moves, Shift+F10 opens the project's menu (Esc closes it).
    QQuickItem* first = visibleItem(shell, QStringLiteral("homeRecent_0"));
    REQUIRE(pressWithKey(shell.window.get(), first, Qt::Key_Down));
    spin(50);
    CHECK(focusedName(shell) == QStringLiteral("homeRecent_1"));
    tapKey(shell, Qt::Key_F10, Qt::ShiftModifier);
    spin(100);
    QObject* menu = shell.object("homeProjectMenu");
    if (!menu)
        menu = screen->findChild<QObject*>(QStringLiteral("homeProjectMenu"));
    REQUIRE(menu != nullptr);
    CHECK(menu->property("visible").toBool());
    CHECK(jsMap(menu->property("project")).value(QStringLiteral("name")).toString() == QStringLiteral("Interview Selects"));
    CHECK_FALSE(menu->property("missing").toBool());

    // Copy Path from the menu (it runs once the menu has closed).
    QObject* copyItem = menu->findChild<QObject*>(QStringLiteral("homeProjectMenuCopyPath"));
    REQUIRE(copyItem != nullptr);
    REQUIRE(QMetaObject::invokeMethod(copyItem, "click"));
    REQUIRE(waitFor([&] { return !menu->property("visible").toBool(); }, 2000));
    spin(100);
    CHECK(QDir::fromNativeSeparators(QGuiApplication::clipboard()->text()) == paths[1]);
    CHECK(visibleItem(shell, QStringLiteral("homeUndoStrip")) != nullptr);

    // Delete removes the focused project (and its star); the focus stays in the list, on the row now in its place (the
    // keys go on working). The Undo strip puts both back.
    REQUIRE(pressWithKey(shell.window.get(), visibleItem(shell, QStringLiteral("homeRecent_1")), Qt::Key_Delete));
    spin(100);
    CHECK(home->recent().size() == 2);
    CHECK(shell.preferences->property("homeStarred").toString() == QStringLiteral("[]"));
    {
        const bool kept = waitFor([&] { return focusedName(shell) == QStringLiteral("homeRecent_1"); }, 2000);
        INFO(focusTrail(shell));
        CHECK(kept);
    }
    REQUIRE(visibleItem(shell, QStringLiteral("homeUndoButton")) != nullptr);
    clickItem(shell, visibleItem(shell, QStringLiteral("homeUndoButton")));
    spin(100);
    CHECK(home->recent().size() == 3);
    CHECK(shell.preferences->property("homeStarred").toString().contains(QStringLiteral("Interview Selects")));
    CHECK(visibleItem(shell, QStringLiteral("homeUndoStrip")) == nullptr);

    // The same in the grid: Delete, and the card now in its place has the focus; Ctrl+Z puts the project back.
    REQUIRE(QMetaObject::invokeMethod(projects, "setView", Q_ARG(QString, QStringLiteral("grid"))));
    spin(100);
    REQUIRE(pressWithKey(shell.window.get(), visibleItem(shell, QStringLiteral("homeRecent_1")), Qt::Key_Delete));
    spin(100);
    CHECK(home->recent().size() == 2);
    {
        const bool kept = waitFor([&] { return focusedName(shell) == QStringLiteral("homeRecent_1"); }, 2000);
        INFO(focusTrail(shell));
        CHECK(kept);
    }
    tapKey(shell, Qt::Key_Z, Qt::ControlModifier);
    spin(100);
    CHECK(home->recent().size() == 3);
    CHECK(shell.preferences->property("homeStarred").toString().contains(QStringLiteral("Interview Selects")));
    CHECK(focusedName(shell).startsWith(QStringLiteral("homeRecent_")));
    REQUIRE(QMetaObject::invokeMethod(projects, "setView", Q_ARG(QString, QStringLiteral("list"))));
    spin(100);

    // A missing project: a click offers Locate... (in the menu); Locate points the entry at the moved file.
    clickItem(shell, visibleItem(shell, QStringLiteral("homeRecent_2")));
    spin(100);
    REQUIRE(menu->property("visible").toBool());
    CHECK(menu->property("missing").toBool());
    QObject* locateItem = menu->findChild<QObject*>(QStringLiteral("homeProjectMenuLocate"));
    REQUIRE(locateItem != nullptr);
    CHECK(locateItem->property("visible").toBool());
    QMetaObject::invokeMethod(menu, "close");
    spin(100);
    {
        QFile moved(dir.filePath(QStringLiteral("Old Promo (moved).wabcut")));
        REQUIRE(moved.open(QIODevice::WriteOnly));
        moved.write("{}");
    }
    screen->setProperty("locating", home->recent()[2]);
    REQUIRE(QMetaObject::invokeMethod(screen, "locate", Q_ARG(QUrl, QUrl::fromLocalFile(dir.filePath(QStringLiteral("Old Promo (moved).wabcut"))))));
    spin(50);
    REQUIRE(home->recent().size() == 3);
    CHECK(home->recent()[2].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Old Promo (moved)"));
    CHECK_FALSE(home->recent()[2].toMap().value(QStringLiteral("missing")).toBool());

    // Clear Unstarred Projects... asks first; the starred one stays; Undo brings the others back.
    REQUIRE(QMetaObject::invokeMethod(projects, "clearUnstarredRequested"));
    spin(100);
    QObject* confirm = shell.object("homeClearConfirm");
    if (!confirm)
        confirm = screen->findChild<QObject*>(QStringLiteral("homeClearConfirm"));
    REQUIRE(confirm != nullptr);
    REQUIRE(confirm->property("opened").toBool());
    QObject* accept = confirm->findChild<QObject*>(QStringLiteral("homeClearAccept"));
    if (!accept)
        accept = shell.object("homeClearAccept");
    REQUIRE(accept != nullptr);
    REQUIRE(QMetaObject::invokeMethod(accept, "click"));
    spin(100);
    REQUIRE(home->recent().size() == 1);
    CHECK(home->recent()[0].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Interview Selects"));
    screen->forceActiveFocus();
    tapKey(shell, Qt::Key_Z, Qt::ControlModifier);
    spin(100);
    CHECK(home->recent().size() == 3);

    const QStringList messages = ownMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Home screen New project: Create makes the chosen format with the Advanced values - remembered next time - and a taken name is refused",
          "[app][home][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* lifecycle = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    REQUIRE(controller != nullptr);
    REQUIRE(lifecycle != nullptr);
    QTemporaryDir dir;
    QQuickItem* screen = shell.item("homeScreen");
    REQUIRE(screen != nullptr);

    // No project yet (a first run): Continue and Recent each say so in one quiet line (Start beside them has New project
    // and Open...); Recent points to Learn, where the sample project and the guides are.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showHome"));
    spin(300);
    for (const char* name : {"homeContinueEmpty", "homeContinueEmptyText", "homeRecentEmpty", "homeRecentLearn"}) {
        INFO(name);
        CHECK(visibleItem(shell, QString::fromLatin1(name)) != nullptr);
    }
    for (const char* name : {"homeContinueNew", "homeContinueOpen", "homeRecentSample", "homeRecentTour", "homeRecentGuide"}) {
        INFO(name);
        CHECK(visibleItem(shell, QString::fromLatin1(name)) == nullptr);
    }
    saveShot(shell, "home-empty.png");
    clickItem(shell, visibleItem(shell, QStringLiteral("homeRecentLearn")));
    spin(150);
    CHECK(screen->property("page").toString() == QStringLiteral("learn"));
    CHECK(visibleItem(shell, QStringLiteral("homeSampleProject")) != nullptr);

    // File > New Project opens Home on the New project page.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "newProject"));
    spin(300);
    REQUIRE(screen->property("opened").toBool());
    CHECK(screen->property("page").toString() == QStringLiteral("new"));
    QQuickItem* page = visibleItem(shell, QStringLiteral("homeNewProjectPage"));
    REQUIRE(page != nullptr);
    page->setProperty("folder", QUrl::fromLocalFile(dir.path()));
    REQUIRE(QMetaObject::invokeMethod(page, "setDefaultName"));
    QQuickItem* name = visibleItem(shell, QStringLiteral("homeProjectName"));
    REQUIRE(name != nullptr);
    CHECK(name->property("text").toString() == QStringLiteral("Untitled Project"));

    // Vertical 9:16, five video tracks (typed), 44.1 kHz (the arrow keys on the pills).
    clickItem(shell, visibleItem(shell, QStringLiteral("homePreset_vertical")));
    spin(50);
    CHECK(page->property("presetId").toString() == QStringLiteral("vertical"));
    REQUIRE(QMetaObject::invokeMethod(page, "setAdvancedOpen", Q_ARG(bool, true)));
    spin(100);
    QQuickItem* tracks = visibleItem(shell, QStringLiteral("homeVideoTracks"));
    REQUIRE(tracks != nullptr);
    tracks->forceActiveFocus(Qt::TabFocusReason);
    QMetaObject::invokeMethod(tracks, "selectAll");
    tapKey(shell, Qt::Key_5, Qt::NoModifier, QStringLiteral("5"));
    tapKey(shell, Qt::Key_Return);
    spin(50);
    REQUIRE(pressWithKey(shell.window.get(), visibleItem(shell, QStringLiteral("homeAudioRate")), Qt::Key_Left));
    spin(50);
    const QVariantMap advanced = jsMap(page->property("advanced"));
    CHECK(advanced.value(QStringLiteral("videoTracks")).toInt() == 5);
    CHECK(advanced.value(QStringLiteral("audioRate")).toInt() == 44100);
    CHECK(visibleItem(shell, QStringLiteral("homeAdvancedChanged")) != nullptr);
    QQuickItem* summary = visibleItem(shell, QStringLiteral("homeSequenceSummary"));
    REQUIRE(summary != nullptr);
    CHECK(summary->property("text").toString() ==
          QStringLiteral("1080 × 1920 · 30 fps · 44.1 kHz stereo · 5 video / 3 audio tracks"));

    // Create (a click): saved where asked, opened, Home closed.
    clickItem(shell, visibleItem(shell, QStringLiteral("homeCreateButton")));
    REQUIRE(waitFor([&] { return !screen->property("opened").toBool(); }, 5000));
    CHECK(QFile::exists(dir.filePath(QStringLiteral("Untitled Project.wabcut"))));
    CHECK_FALSE(lifecycle->demo());
    const SequencePtr seq = controller->currentSequence();
    REQUIRE(seq);
    CHECK(seq->width == 1080);
    CHECK(seq->height == 1920);
    CHECK(seq->videoTracks.size() == 5);
    CHECK(seq->audioRate == Rate{44100, 1});

    // Next time: the same format and Advanced values, the next free name, Reset to defaults.
    REQUIRE(QMetaObject::invokeMethod(screen, "openPage", Q_ARG(QString, QStringLiteral("new"))));
    spin(200);
    page = visibleItem(shell, QStringLiteral("homeNewProjectPage"));
    REQUIRE(page != nullptr);
    CHECK(page->property("presetId").toString() == QStringLiteral("vertical"));
    CHECK(jsMap(page->property("advanced")).value(QStringLiteral("videoTracks")).toInt() == 5);
    CHECK(QDir::fromNativeSeparators(page->property("folder").toUrl().toLocalFile()) == dir.path());
    name = visibleItem(shell, QStringLiteral("homeProjectName"));
    REQUIRE(name != nullptr);
    CHECK(name->property("text").toString() == QStringLiteral("Untitled Project 2"));
    clickItem(shell, visibleItem(shell, QStringLiteral("homeAdvancedReset")));
    spin(50);
    CHECK(jsMap(page->property("advanced")).value(QStringLiteral("videoTracks")).toInt() == 3);
    CHECK(visibleItem(shell, QStringLiteral("homeAdvancedChanged")) == nullptr);

    // A name that is taken: said at once; Create keeps Home open.
    name->setProperty("text", QStringLiteral("Untitled Project"));
    spin(50);
    QQuickItem* error = visibleItem(shell, QStringLiteral("homeNewProjectError"));
    REQUIRE(error != nullptr);
    CHECK_FALSE(error->property("text").toString().isEmpty());
    clickItem(shell, visibleItem(shell, QStringLiteral("homeCreateButton")));
    spin(300);
    CHECK(screen->property("opened").toBool());
    CHECK(screen->property("page").toString() == QStringLiteral("new"));

#ifdef Q_OS_WIN
    // A folder that cannot be made (here a name Windows refuses): nothing was created, so Home stays on the page with
    // the reason, and the project behind it is the same.
    const QString created = lifecycle->filePath();
    name->setProperty("text", QStringLiteral("Unmade"));
    page->setProperty("folder", QUrl::fromLocalFile(dir.filePath(QStringLiteral("bad<folder"))));
    spin(50);
    clickItem(shell, visibleItem(shell, QStringLiteral("homeCreateButton")));
    spin(300);
    CHECK(screen->property("opened").toBool());
    CHECK(screen->property("page").toString() == QStringLiteral("new"));
    error = visibleItem(shell, QStringLiteral("homeNewProjectError"));
    REQUIRE(error != nullptr);
    CHECK(error->property("text").toString().contains(QStringLiteral("bad<folder")));
    CHECK(lifecycle->filePath() == created);
    page->setProperty("folder", QUrl::fromLocalFile(dir.path()));
#endif

    // Cancel goes back; Esc then returns to the new project behind Home.
    clickItem(shell, visibleItem(shell, QStringLiteral("homeCancelButton")));
    spin(100);
    CHECK(screen->property("page").toString() == QStringLiteral("home"));
    tapKey(shell, Qt::Key_Escape);
    spin(200);
    CHECK_FALSE(screen->property("opened").toBool());

    // File > New Project from the editor: Cancel goes back to the editor.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "newProject"));
    spin(300);
    REQUIRE(screen->property("opened").toBool());
    CHECK(screen->property("page").toString() == QStringLiteral("new"));
    clickItem(shell, visibleItem(shell, QStringLiteral("homeCancelButton")));
    spin(300);
    CHECK_FALSE(screen->property("opened").toBool());

    // The project's file cannot be written (a folder is in the way of the save): the new project exists all the same
    // (it replaced the one shown), so Home closes and the editor says what went wrong.
    REQUIRE(QDir(dir.path()).mkpath(QStringLiteral("Blocked.wabcut.tmp")));
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "newProject"));
    spin(300);
    page = visibleItem(shell, QStringLiteral("homeNewProjectPage"));
    REQUIRE(page != nullptr);
    page->setProperty("folder", QUrl::fromLocalFile(dir.path()));
    name = visibleItem(shell, QStringLiteral("homeProjectName"));
    REQUIRE(name != nullptr);
    name->setProperty("text", QStringLiteral("Blocked"));
    spin(50);
    clickItem(shell, visibleItem(shell, QStringLiteral("homeCreateButton")));
    REQUIRE(waitFor([&] { return !screen->property("opened").toBool(); }, 5000));
    QObject* toast = shell.object("toast");
    REQUIRE(toast != nullptr);
    CHECK(toast->property("text").toString().contains(QStringLiteral("Blocked.wabcut")));
    CHECK(lifecycle->untitled());
    CHECK_FALSE(lifecycle->demo());

    const QStringList messages = ownMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Home screen Learn: the sample project comes back from a real project and closes Home; import notes show in the message strip or the editor",
          "[app][home][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* home = shell.engine.singletonInstance<HomeBridge*>("Wabcut", "Home");
    auto* lifecycle = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    REQUIRE(home != nullptr);
    REQUIRE(lifecycle != nullptr);
    QTemporaryDir dir;
    REQUIRE(home->createProject(QStringLiteral("Real"), QUrl::fromLocalFile(dir.path()), presetNamed(*home, "1080p25")).isEmpty());
    REQUIRE_FALSE(lifecycle->demo());
    QQuickItem* screen = shell.item("homeScreen");
    REQUIRE(screen != nullptr);

    // An empty folder: nothing to import, said in the message strip.
    REQUIRE(QDir(dir.path()).mkdir(QStringLiteral("empty")));
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showHome"));
    spin(300);
    REQUIRE(QMetaObject::invokeMethod(screen, "handleUrls",
                                      Q_ARG(QVariant, QVariant::fromValue(QList<QUrl>{QUrl::fromLocalFile(dir.filePath(QStringLiteral("empty")))}))));
    spin(100);
    QQuickItem* message = visibleItem(shell, QStringLiteral("homeMessage"));
    REQUIRE(message != nullptr);
    CHECK(message->property("message").toString().contains(QStringLiteral("folder")));
    // home.showMessage (other streams use it while Home is open).
    REQUIRE(QMetaObject::invokeMethod(screen, "showMessage", Q_ARG(QVariant, QStringLiteral("Could not open Missing.wabcut")),
                                      Q_ARG(QVariant, QStringLiteral("danger"))));
    spin(50);
    CHECK(message->property("message").toString() == QStringLiteral("Could not open Missing.wabcut"));
    CHECK(message->property("tone").toString() == QStringLiteral("danger"));
    // A long message (an import's notes, one per line) is four lines high and scrolls: the last line can be read.
    {
        QStringList notes;
        for (int i = 1; i <= 10; ++i)
            notes << QStringLiteral("Note %1 about a file that was left out").arg(i);
        REQUIRE(QMetaObject::invokeMethod(screen, "showMessage", Q_ARG(QVariant, notes.join(QLatin1Char('\n'))),
                                          Q_ARG(QVariant, QStringLiteral("info"))));
        spin(50);
        QQuickItem* text = visibleItem(shell, QStringLiteral("homeMessageText"));
        REQUIRE(text != nullptr);
        CHECK(text->property("interactive").toBool());
        CHECK(text->height() < text->property("contentHeight").toReal());
        CHECK(text->height() < 8 * 20);
        REQUIRE(pressWithKey(shell.window.get(), text, Qt::Key_PageDown));
        spin(50);
        CHECK(text->property("contentY").toReal() > 0);
    }
    clickItem(shell, visibleItem(shell, QStringLiteral("homeMessageClose")));
    spin(50);
    CHECK(visibleItem(shell, QStringLiteral("homeMessage")) == nullptr);

    // Back shows the real project's name; Learn > Explore the sample project brings the demo back and closes Home.
    QQuickItem* back = visibleItem(shell, QStringLiteral("homeBackButton"));
    REQUIRE(back != nullptr);
    CHECK(back->property("text").toString() == QStringLiteral("Back to Real"));
    showPage(screen, "learn");
    clickItem(shell, visibleItem(shell, QStringLiteral("homeSampleProject")));
    REQUIRE(waitFor([&] { return !screen->property("opened").toBool(); }, 3000));
    CHECK(lifecycle->demo());
    CHECK(demoShown(shell));

    // A project dropped with other files: the project opens, and the editor says what was left out.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showHome"));
    spin(200);
    REQUIRE(QMetaObject::invokeMethod(screen, "handleUrls", Q_ARG(QVariant, QVariant::fromValue(QList<QUrl>{
        QUrl::fromLocalFile(dir.filePath(QStringLiteral("Real.wabcut"))), QUrl::fromLocalFile(dir.filePath(QStringLiteral("b-roll.mov")))}))));
    REQUIRE(waitFor([&] { return !screen->property("opened").toBool(); }, 3000));
    CHECK_FALSE(lifecycle->demo());
    CHECK(QDir::fromNativeSeparators(lifecycle->filePath()) == dir.filePath(QStringLiteral("Real.wabcut")));
    QObject* toast = shell.object("toast");
    REQUIRE(toast != nullptr);
    CHECK(toast->property("text").toString().contains(QStringLiteral("Real.wabcut")));

    // A note goes with its request: a dropped project that cannot be opened (with another file) leaves nothing behind
    // for the next project opened from Home.
    writeFile(dir.filePath(QStringLiteral("Broken.wabcut")), "not a project");
    REQUIRE(home->createProject(QStringLiteral("Second"), QUrl::fromLocalFile(dir.path()), presetNamed(*home, "1080p25")).isEmpty());
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showHome"));
    spin(200);
    REQUIRE(QMetaObject::invokeMethod(screen, "handleUrls", Q_ARG(QVariant, QVariant::fromValue(QList<QUrl>{
        QUrl::fromLocalFile(dir.filePath(QStringLiteral("Broken.wabcut"))), QUrl::fromLocalFile(dir.filePath(QStringLiteral("b-roll.mov")))}))));
    REQUIRE(waitFor([&] { return screen->property("messageTone").toString() == QStringLiteral("danger"); }, 3000));
    CHECK(screen->property("opened").toBool());
    REQUIRE(QMetaObject::invokeMethod(screen, "openRequested", Q_ARG(QUrl, QUrl::fromLocalFile(dir.filePath(QStringLiteral("Real.wabcut"))))));
    REQUIRE(waitFor([&] { return !screen->property("opened").toBool(); }, 3000));
    CHECK(QDir::fromNativeSeparators(lifecycle->filePath()) == dir.filePath(QStringLiteral("Real.wabcut")));
    CHECK_FALSE(toast->property("text").toString().contains(QStringLiteral("Broken")));

    const QStringList messages = ownMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Home screen start-up: Reopen the last project starts in it instead of Home; a missing one shows Home",
          "[app][home][qml]") {
    resetSettings();
    clearMessages();
    QTemporaryDir dir;
    // A saved project, remembered as the last one (QSettings home/recentProjects), and the start-up choice.
    {
        Fixture fx;
        REQUIRE(fx.home.createProject(QStringLiteral("Last"), QUrl::fromLocalFile(dir.path()), presetNamed(fx.home, "1080p30"))
                    .isEmpty());
        REQUIRE(fx.home.recent().size() == 1);
    }
    QSettings().setValue(QStringLiteral("startup/launchAction"), QStringLiteral("reopen"));
    struct LaunchHome {
        LaunchHome() { HomeBridge::setShowAtLaunch(true); }
        ~LaunchHome() { HomeBridge::setShowAtLaunch(false); }
    };
    {
        std::optional<LaunchHome> launch(std::in_place);
        Shell shell;
        launch.reset();
        REQUIRE(shell.window != nullptr);
        auto* lifecycle = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
        REQUIRE(lifecycle != nullptr);
        QObject* screen = shell.object("homeScreen");
        REQUIRE(screen != nullptr);
        // Not while the window is being made (opening is synchronous: nothing would paint until it ended). Home says
        // it is opening the project, which opens once the window is on screen (this hidden window: after 2 s); then
        // Home closes.
        CHECK(screen->property("opened").toBool());
        CHECK(screen->property("busyText").toString() == QStringLiteral("Opening project"));
        CHECK(QDir::fromNativeSeparators(lifecycle->filePath()) != dir.filePath(QStringLiteral("Last.wabcut")));
        REQUIRE(waitFor([&] { return !screen->property("opened").toBool(); }, 8000));
        CHECK(screen->property("busyText").toString().isEmpty());
        CHECK_FALSE(lifecycle->demo());
        CHECK(QDir::fromNativeSeparators(lifecycle->filePath()) == dir.filePath(QStringLiteral("Last.wabcut")));
    }
    // The file is gone: Home, as with the default choice.
    REQUIRE(QFile::remove(dir.filePath(QStringLiteral("Last.wabcut"))));
    {
        std::optional<LaunchHome> launch(std::in_place);
        Shell shell;
        launch.reset();
        REQUIRE(shell.window != nullptr);
        QObject* screen = shell.object("homeScreen");
        REQUIRE(screen != nullptr);
        CHECK(screen->property("opened").toBool());
        CHECK(screen->property("busyText").toString().isEmpty());
    }
    const QStringList messages = ownMessages();
    INFO(join(messages));
    CHECK(messages.filter(QStringLiteral("qml")).isEmpty());
}
