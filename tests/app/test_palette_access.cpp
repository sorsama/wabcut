// M5c: the command palette (search, ranking, disabled reasons, execution, the typed command bridge with dry run and
// undo), the timeline's accessible clip list (sync from ChangeSets, keyboard-only selection and trims), and drag
// polish (auto-scroll, a new track when dropping below the last one). Offscreen; no window is shown.

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "ClipAccessModel.h"
#include "CommandBridge.h"
#include "CommandPalette.h"
#include "ProjectController.h"
#include "TimelineEditing.h"
#include "TimelineView.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QHash>
#include <QKeyEvent>
#include <QMouseEvent>
#include <qpa/qwindowsysteminterface.h>

#include <algorithm>
#include <memory>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

QStringList titles(const QVariantList& rows, int count = 5) {
    QStringList out;
    for (int i = 0; i < rows.size() && i < count; ++i)
        out << rows[i].toMap().value(QStringLiteral("title")).toString();
    return out;
}

QVariantMap rowWithKey(const QVariantList& rows, const QString& key) {
    for (const QVariant& r : rows) {
        if (r.toMap().value(QStringLiteral("key")).toString() == key)
            return r.toMap();
    }
    return {};
}

/// A palette over a small set of keymap commands whose availability and trigger the test controls.
struct PaletteRig {
    CommandPalette palette;
    QStringList triggered;
    std::shared_ptr<KeymapCommandProvider> provider;

    PaletteRig() {
        std::vector<KeymapCommandProvider::Command> commands{
            {QStringLiteral("edit.undo"), QStringLiteral("Undo"), QStringLiteral("Edit"), QStringLiteral("Undo")},
            {QStringLiteral("edit.redo"), QStringLiteral("Redo"), QStringLiteral("Edit"), QStringLiteral("Redo")},
            {QStringLiteral("edit.clear"), QStringLiteral("Clear"), QStringLiteral("Edit"), QStringLiteral("Clear")},
            {QStringLiteral("timeline.rippleDelete"), QStringLiteral("Ripple Delete"), QStringLiteral("Timeline"), {}},
            {QStringLiteral("sequence.addEdit"), QStringLiteral("Add Edit"), QStringLiteral("Sequence"), {}},
            {QStringLiteral("tool.razor"), QStringLiteral("Razor Tool"), QStringLiteral("Tools"), {}},
            {QStringLiteral("file.save"), QStringLiteral("Save"), QStringLiteral("File"), {}},
            {QStringLiteral("marker.addMarker"), QStringLiteral("Add Marker"), QStringLiteral("Markers"), {}},
        };
        provider = std::make_shared<KeymapCommandProvider>(
            std::move(commands),
            [](const QString& id) {
                if (id == QLatin1String("sequence.addEdit"))
                    return QStringLiteral("Ctrl+K");
                if (id == QLatin1String("edit.undo"))
                    return QStringLiteral("Ctrl+Z");
                return QString();
            },
            [](const QString& id) {
                if (id == QLatin1String("edit.redo"))
                    return QVariantMap{{QStringLiteral("handled"), true}, {QStringLiteral("enabled"), false},
                                       {QStringLiteral("reason"), QStringLiteral("Nothing to redo")}};
                if (id == QLatin1String("file.save") || id == QLatin1String("marker.addMarker") || id.startsWith(QLatin1String("tool.")))
                    return QVariantMap{};
                return QVariantMap{{QStringLiteral("handled"), true}, {QStringLiteral("enabled"), true}};
            },
            [this](const QString& id) {
                triggered << id;
                return true;
            });
        palette.addProvider(provider);
        palette.addProvider(std::make_shared<AskAiProvider>([this](const QString& q) { emit palette.aiRequested(q); }));
    }
};

} // namespace

// ---- Palette: search, ranking, disabled rows, execution ------------------------------------------------------------

TEST_CASE("Fuzzy matching rewards word starts, runs and early matches", "[app][palette]") {
    CHECK(fuzzyScore(u"xyz", u"Ripple Delete") == 0);
    CHECK(fuzzyScore(u"rd", u"Ripple Delete") > 0);
    CHECK(fuzzyScore(u"rd", u"Ripple Delete") > fuzzyScore(u"rd", u"Reverse Dissolve Order") - 1000); // both match
    CHECK(fuzzyScore(u"del", u"Ripple Delete") > fuzzyScore(u"del", u"Model Selection"));        // word start + run
    CHECK(fuzzyScore(u"und", u"Undo") > fuzzyScore(u"und", u"Sound Editor"));                    // prefix
    CHECK(fuzzyScore(u"UNDO", u"undo") > 0);                                                      // case-insensitive
}

TEST_CASE("Palette ranking: titles, initials, shortcuts, recents; disabled rows keep their reason", "[app][palette]") {
    PaletteRig rig;
    CommandPalette& p = rig.palette;

    p.setQuery(QStringLiteral("undo"));
    CHECK(titles(p.results(), 1) == QStringList{QStringLiteral("Undo")});
    p.setQuery(QStringLiteral("rd"));
    CHECK(titles(p.results(), 1) == QStringList{QStringLiteral("Ripple Delete")}); // initials
    p.setQuery(QStringLiteral("ctrl+k"));
    CHECK(titles(p.results(), 1) == QStringList{QStringLiteral("Add Edit")}); // by its shortcut
    p.setQuery(QStringLiteral("add"));
    CHECK(titles(p.results(), 2).contains(QStringLiteral("Add Edit")));

    // Disabled rows: dimmed with the reason, below runnable matches; running one returns the reason.
    p.setQuery(QStringLiteral("re"));
    const QVariantMap redo = rowWithKey(p.results(), QStringLiteral("command:edit.redo"));
    CHECK_FALSE(redo.value(QStringLiteral("enabled")).toBool());
    CHECK(redo.value(QStringLiteral("reason")).toString() == QStringLiteral("Nothing to redo"));
    const QVariantMap save = rowWithKey(p.results(), QStringLiteral("command:file.save"));
    if (!save.isEmpty())
        CHECK(save.value(QStringLiteral("reason")).toString() == QStringLiteral("Not available yet"));
    CHECK(p.activate(QStringLiteral("command:edit.redo")) == QStringLiteral("Nothing to redo"));
    CHECK(rig.triggered.isEmpty());
    // The view-handled tool command is runnable without a controller answer.
    p.setQuery(QStringLiteral("razor"));
    CHECK(rowWithKey(p.results(), QStringLiteral("command:tool.razor")).value(QStringLiteral("enabled")).toBool());

    // Running records the command as recent; an empty query lists recents first.
    CHECK(p.activate(QStringLiteral("command:timeline.rippleDelete")).isEmpty());
    CHECK(p.activate(QStringLiteral("command:edit.undo")).isEmpty());
    CHECK(rig.triggered == (QStringList{QStringLiteral("timeline.rippleDelete"), QStringLiteral("edit.undo")}));
    CHECK(p.recent().first() == QStringLiteral("command:edit.undo"));
    p.setQuery({});
    const QVariantList rows = p.results();
    CHECK(titles(rows, 2) == (QStringList{QStringLiteral("Undo"), QStringLiteral("Ripple Delete")}));
    CHECK(rows.first().toMap().value(QStringLiteral("section")).toString() == QStringLiteral("recent"));

    // "Ask AI…" is always the last row, separate, and carries the query.
    p.setQuery(QStringLiteral("make the intro shorter"));
    const QVariantMap ai = p.results().last().toMap();
    CHECK(ai.value(QStringLiteral("key")).toString() == QStringLiteral("ai:ask"));
    CHECK(ai.value(QStringLiteral("section")).toString() == QStringLiteral("ai"));
    QString asked;
    QObject::connect(&p, &CommandPalette::aiRequested, [&](const QString& q) { asked = q; });
    CHECK(p.activate(QStringLiteral("ai:ask")).isEmpty());
    CHECK(asked == QStringLiteral("make the intro shorter"));
}

TEST_CASE("Command availability explains why a command cannot run now", "[app][palette]") {
    ProjectController controller;
    const auto state = [&](const char* id) { return controller.commandAvailability(QString::fromLatin1(id)); };
    controller.clearSelection();
    CHECK(state("edit.undo").value(QStringLiteral("reason")).toString() == QStringLiteral("Nothing to undo"));
    CHECK(state("edit.clear").value(QStringLiteral("reason")).toString() == QStringLiteral("Select a clip first"));
    CHECK(state("edit.paste").value(QStringLiteral("reason")).toString() == QStringLiteral("Nothing has been copied"));
    CHECK(state("marker.goToIn").value(QStringLiteral("reason")).toString() == QStringLiteral("No In mark is set"));
    CHECK_FALSE(state("file.save").value(QStringLiteral("handled")).toBool()); // not this controller's
    CHECK(state("transport.goToStart").value(QStringLiteral("enabled")).toBool());

    controller.selectAdjacentClip(1); // something selected now
    CHECK(state("edit.clear").value(QStringLiteral("enabled")).toBool());
    controller.copySelection();
    CHECK(state("edit.paste").value(QStringLiteral("enabled")).toBool());
    controller.deleteSelection();
    CHECK(state("edit.undo").value(QStringLiteral("enabled")).toBool());

    // In the app's provider, commands nobody runs yet are listed as unavailable, view commands as runnable.
    const auto provider = KeymapCommandProvider::forApp(nullptr, &controller);
    const auto items = provider->items({});
    const auto find = [&](const char* id) {
        return std::ranges::find_if(items, [&](const PaletteItem& i) { return i.key == QStringLiteral("command:") + QLatin1String(id); });
    };
    REQUIRE(find("metadata.loop") != items.end()); // the Metadata panel's Loop: nothing runs it yet
    CHECK_FALSE(find("metadata.loop")->enabled);
    CHECK(find("metadata.loop")->reason == QStringLiteral("Not available yet"));
    REQUIRE(find("sequence.renderInToOut") != items.end()); // UX rework: Main.qml routes it (its key works)
    CHECK(find("sequence.renderInToOut")->enabled);
    REQUIRE(find("file.save") != items.end()); // Main.qml runs the File menu (ProjectFile)
    CHECK(find("file.save")->enabled);
    REQUIRE(find("tool.razor") != items.end());
    CHECK(find("tool.razor")->enabled);
    REQUIRE(find("edit.undo") != items.end());
    CHECK(find("edit.undo")->enabled);
}

// UX rework (stream S): what Main.qml routes - the panel and workspace keys, the shell's window and file commands, Render
// In to Out, the timeline's track keys, Graphics and Moments - is listed as ready, not "Not available yet"; the Library
// and Multi-Camera answer for their own keys.
TEST_CASE("Palette: the window and workspace commands Main.qml routes are ready", "[app][palette]") {
    for (const char* id :
         {"window.projects", "window.effects", "window.mediaBrowser", "window.timeline", "window.audioTrackMixer",
          "window.audioClipMixer", "window.workspace1", "window.workspace5", "window.workspace9",
          "window.effectControls", "window.resetToSavedLayout", "window.maximizeFrame", "file.close",
          "file.closeProject", "file.saveCopy", "file.sendToMediaEncoder", "edit.preferences", "edit.find",
          "clip.makeSubclip", "sequence.renderInToOut", "sequence.trimEdit", "sequence.toggleTrimType",
          "timeline.showVideoFadeHandles", "timeline.expandAllTracks", "timeline.nextSequenceTab", "marker.goToNext",
          "marker.clearSelected", "graphics.newTextLayer", "graphics.selectPreviousLayer", "moments.slowMo",
          "history.stepBackward", "history.stepForward", "project.newBin", "multicam.toggleView",
          "multicam.cutToCamera3"}) {
        INFO(id);
        CHECK(KeymapCommandProvider::viewHandles(QString::fromLatin1(id)));
    }
    // Typed edits keep their prompts (Add Marker asks for a name and a colour); nothing routes these yet.
    for (const char* id : {"marker.add", "effects.newCustomBin", "history.delete", "metadata.loop"}) {
        INFO(id);
        CHECK_FALSE(KeymapCommandProvider::viewHandles(QString::fromLatin1(id)));
    }

    // The app's provider: ready rows, and a bridge's own answer for its ids (the Multi-Camera view is off).
    ProjectController controller;
    QStringList asked;
    const auto provider = KeymapCommandProvider::forApp(nullptr, &controller, [&](const QString& id) -> QVariantMap {
        asked << id;
        if (!id.startsWith(QLatin1String("multicam.")))
            return {};
        return {{QStringLiteral("handled"), true},
                {QStringLiteral("enabled"), id == QLatin1String("multicam.toggleView")},
                {QStringLiteral("reason"), QStringLiteral("Turn on the multi-camera view first")}};
    });
    const auto items = provider->items({});
    const auto find = [&](const char* id) {
        return std::ranges::find_if(items, [&](const PaletteItem& i) { return i.key == QStringLiteral("command:") + QLatin1String(id); });
    };
    for (const char* id : {"window.workspace3", "window.effectControls", "file.saveCopy", "file.closeProject",
                           "timeline.expandAllTracks", "graphics.newTextLayer"}) {
        INFO(id);
        REQUIRE(find(id) != items.end());
        CHECK(find(id)->enabled);
    }
    REQUIRE(find("multicam.cutToCamera2") != items.end());
    CHECK_FALSE(find("multicam.cutToCamera2")->enabled);
    CHECK(find("multicam.cutToCamera2")->reason == QStringLiteral("Turn on the multi-camera view first"));
    REQUIRE(find("multicam.toggleView") != items.end());
    CHECK(find("multicam.toggleView")->enabled);
    // The controller answers first: the bridges are not asked for its ids.
    CHECK_FALSE(asked.contains(QStringLiteral("edit.undo")));
    CHECK(asked.contains(QStringLiteral("multicam.toggleView")));
}

TEST_CASE("Palette: rows the shell hands over run through shellActionRequested", "[app][palette]") {
    PaletteRig rig;
    CommandPalette& p = rig.palette;
    QStringList requested;
    QObject::connect(&p, &CommandPalette::shellActionRequested, [&](const QString& id) { requested << id; });

    const QVariantList rows{
        QVariantMap{{QStringLiteral("id"), QStringLiteral("panel:library")},
                    {QStringLiteral("title"), QStringLiteral("Open panel: Library")},
                    {QStringLiteral("group"), QStringLiteral("Window")},
                    {QStringLiteral("shortcut"), QStringLiteral("Shift+1")},
                    {QStringLiteral("keywords"), QStringList{QStringLiteral("project panel"), QStringLiteral("bins")}}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("workspace:7")},
                    {QStringLiteral("title"), QStringLiteral("Workspace: Color grading")},
                    {QStringLiteral("group"), QStringLiteral("Window")},
                    {QStringLiteral("detail"), QStringLiteral("From the Color preset")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("panel:generate")},
                    {QStringLiteral("title"), QStringLiteral("Open panel: Generate")},
                    {QStringLiteral("enabled"), false},
                    {QStringLiteral("reason"), QStringLiteral("AI features are off")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("panel:library")}, {QStringLiteral("title"), QStringLiteral("Twice")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("no-title")}},
        QVariantMap{{QStringLiteral("title"), QStringLiteral("No id")}},
    };
    CHECK(p.setShellActions(rows) == 3); // the second "panel:library" and the rows without id or title are skipped

    // Found by title, keywords and shortcut like the keymap's rows.
    p.setQuery(QStringLiteral("open panel lib"));
    CHECK(titles(p.results(), 1) == QStringList{QStringLiteral("Open panel: Library")});
    p.setQuery(QStringLiteral("bins"));
    CHECK(!rowWithKey(p.results(), QStringLiteral("shell:panel:library")).isEmpty());
    p.setQuery(QStringLiteral("color grading"));
    const QVariantMap workspace = rowWithKey(p.results(), QStringLiteral("shell:workspace:7"));
    CHECK(workspace.value(QStringLiteral("detail")).toString() == QStringLiteral("From the Color preset"));
    CHECK(workspace.value(QStringLiteral("enabled")).toBool());

    // Running one hands its id back to the shell and makes it recent; a disabled one gives its reason.
    CHECK(p.activate(QStringLiteral("shell:panel:library")).isEmpty());
    CHECK(requested == QStringList{QStringLiteral("panel:library")});
    CHECK(p.recent().first() == QStringLiteral("shell:panel:library"));
    CHECK(p.activate(QStringLiteral("shell:panel:generate")) == QStringLiteral("AI features are off"));
    CHECK(requested.size() == 1);
    CHECK(rig.triggered.isEmpty()); // no keymap command ran

    // Setting them again replaces them.
    CHECK(p.setShellActions({QVariantMap{{QStringLiteral("id"), QStringLiteral("recent:1")},
                                         {QStringLiteral("title"), QStringLiteral("Open Recent: My film")}}}) == 1);
    p.setQuery(QStringLiteral("open"));
    CHECK(rowWithKey(p.results(), QStringLiteral("shell:panel:library")).isEmpty());
    CHECK(!rowWithKey(p.results(), QStringLiteral("shell:recent:1")).isEmpty());
    CHECK(p.activate(QStringLiteral("shell:panel:library")) == QStringLiteral("Unknown command"));
}

// UX rework (stream S): the menu bar asks the palette whether a command can run now (commandState), so a menu item and
// its palette row agree.
TEST_CASE("Palette: menus read whether a command can run now from commandState", "[app][palette]") {
    ProjectController controller;
    controller.clearSelection();
    CommandPalette p;
    CHECK_FALSE(p.commandState(QStringLiteral("edit.undo")).value(QStringLiteral("enabled")).toBool()); // no keymap rows
    p.addProvider(KeymapCommandProvider::forApp(nullptr, &controller));
    const auto state = [&](const char* id) { return p.commandState(QString::fromLatin1(id)); };

    // The controller's answer, with its reason.
    CHECK(state("edit.undo").value(QStringLiteral("known")).toBool());
    CHECK_FALSE(state("edit.undo").value(QStringLiteral("enabled")).toBool());
    CHECK(state("edit.undo").value(QStringLiteral("reason")).toString() == QStringLiteral("Nothing to undo"));
    CHECK(state("edit.clear").value(QStringLiteral("reason")).toString() == QStringLiteral("Select a clip first"));
    controller.selectAdjacentClip(1);
    CHECK(state("edit.clear").value(QStringLiteral("enabled")).toBool());
    // What the shell routes is ready; the registry's title and group come along.
    const QVariantMap workspace = state("window.workspace3");
    CHECK(workspace.value(QStringLiteral("enabled")).toBool());
    CHECK(workspace.value(QStringLiteral("title")).toString() == QStringLiteral("Workspace: Audio"));
    CHECK(workspace.value(QStringLiteral("group")).toString() == QStringLiteral("Window"));
    CHECK(state("file.saveCopy").value(QStringLiteral("enabled")).toBool());
    // The key of a typed edit (Add Marker) runs: enabled, although its palette row is the edit with prompts.
    CHECK(state("marker.add").value(QStringLiteral("enabled")).toBool());
    // Nothing runs it yet; not a command at all.
    CHECK_FALSE(state("metadata.loop").value(QStringLiteral("enabled")).toBool());
    CHECK(state("metadata.loop").value(QStringLiteral("reason")).toString() == QStringLiteral("Not available yet"));
    CHECK_FALSE(state("no.suchCommand").value(QStringLiteral("known")).toBool());
    CHECK_FALSE(state("no.suchCommand").value(QStringLiteral("enabled")).toBool());
}

// ---- Command bridge: catalog, dry run, apply, undo -----------------------------------------------------------------

TEST_CASE("CommandBridge: typed commands with prompts, a dry run that changes nothing, apply and undo", "[app][palette]") {
    ProjectController controller;
    CommandBridge bridge;
    bridge.setController(&controller);

    // The catalog's edit commands, minus those the keymap already runs.
    QStringList names;
    QVariantMap markerAdd;
    for (const QVariant& c : bridge.commands()) {
        names << c.toMap().value(QStringLiteral("name")).toString();
        if (names.last() == QLatin1String("marker.add"))
            markerAdd = c.toMap();
    }
    CHECK(names.contains(QStringLiteral("marker.add")));
    CHECK(names.contains(QStringLiteral("clip.setColor")));
    CHECK(names.contains(QStringLiteral("sequence.addTrack")));
    CHECK_FALSE(names.contains(QStringLiteral("timeline.rippleDelete"))); // the keymap's Ripple Delete
    CHECK_FALSE(names.contains(QStringLiteral("clip.find")));             // queries are not actions
    QStringList kinds;
    for (const QVariant& p : markerAdd.value(QStringLiteral("params")).toList())
        kinds << p.toMap().value(QStringLiteral("key")).toString() + QLatin1Char('=') + p.toMap().value(QStringLiteral("kind")).toString();
    CHECK(kinds.contains(QStringLiteral("time=timecode")));
    CHECK(kinds.contains(QStringLiteral("name=text")));
    CHECK(kinds.contains(QStringLiteral("colorIndex=color")));

    SECTION("Add Marker: defaults from the playhead, preview, apply, undo") {
        const QVariantMap defaults = bridge.defaults(QStringLiteral("marker.add"));
        CHECK(defaults.value(QStringLiteral("time")).toString() == controller.playheadText());
        const auto before = controller.snapshot();
        const int markers = static_cast<int>(controller.markers().size());
        QVariantMap params{{QStringLiteral("time"), QStringLiteral("00:00:02:00")}, {QStringLiteral("name"), QStringLiteral("Beat")}};
        const QVariantMap preview = bridge.preview(QStringLiteral("marker.add"), params);
        INFO(preview.value(QStringLiteral("error")).toString().toStdString() << " " << preview.value(QStringLiteral("detail")).toString().toStdString());
        REQUIRE(preview.value(QStringLiteral("ok")).toBool());
        CHECK(preview.value(QStringLiteral("summary")).toString() == QStringLiteral("Updates markers"));
        CHECK(controller.snapshot() == before); // a dry run changes nothing
        const QVariantMap applied = bridge.apply(QStringLiteral("marker.add"), params);
        REQUIRE(applied.value(QStringLiteral("ok")).toBool());
        CHECK(controller.markers().size() == markers + 1);
        CHECK(controller.undoText() == QStringLiteral("Undo Add Marker"));
        REQUIRE(controller.undo());
        CHECK(controller.markers().size() == markers);
    }
    SECTION("Set Label Color acts on the selection; without one it says so") {
        controller.clearSelection();
        CHECK(bridge.preview(QStringLiteral("clip.setColor"), {{QStringLiteral("colorIndex"), 3}})
                  .value(QStringLiteral("error"))
                  .toString()
              == QStringLiteral("Select a clip first"));
        controller.selectAdjacentClip(1);
        const ClipId clip = controller.primaryClip();
        const QVariantMap preview = bridge.preview(QStringLiteral("clip.setColor"), {{QStringLiteral("colorIndex"), 6}});
        REQUIRE(preview.value(QStringLiteral("ok")).toBool());
        CHECK(preview.value(QStringLiteral("summary")).toString().startsWith(QStringLiteral("Changes")));
        REQUIRE(bridge.apply(QStringLiteral("clip.setColor"), {{QStringLiteral("colorIndex"), 6}}).value(QStringLiteral("ok")).toBool());
        CHECK(controller.currentSequence()->findClip(clip)->clip().colorIndex == 6);
        REQUIRE(controller.undo());
        CHECK(controller.currentSequence()->findClip(clip)->clip().colorIndex != 6);
    }
    SECTION("the palette runs edit commands through the bridge") {
        CommandPalette palette;
        palette.addProvider(std::make_shared<EditCommandProvider>(&bridge));
        palette.setQuery(QStringLiteral("add marker"));
        REQUIRE_FALSE(palette.results().isEmpty());
        const QVariantMap row = palette.results().first().toMap();
        CHECK(row.value(QStringLiteral("key")).toString() == QStringLiteral("edit:marker.add"));
        CHECK_FALSE(row.value(QStringLiteral("params")).toList().isEmpty());
        const int markers = static_cast<int>(controller.markers().size());
        CHECK(palette.activate(QStringLiteral("edit:marker.add"), {{QStringLiteral("time"), QStringLiteral("00:00:03:00")}}).isEmpty());
        CHECK(controller.markers().size() == markers + 1);
    }
}

TEST_CASE("CommandBridge: finishing commands ask for values, never for raw ids; Speed/Duration round trip", "[app][palette]") {
    ProjectController controller;
    CommandBridge bridge;
    bridge.setController(&controller);
    QHash<QString, QVariantMap> byName;
    for (const QVariant& c : bridge.commands())
        byName.insert(c.toMap().value(QStringLiteral("name")).toString(), c.toMap());
    const auto prompts = [&](const QString& name) {
        QStringList out;
        for (const QVariant& p : byName.value(name).value(QStringLiteral("params")).toList())
            out << p.toMap().value(QStringLiteral("key")).toString() + QLatin1Char('=') + p.toMap().value(QStringLiteral("kind")).toString();
        return out;
    };

    // No prompt asks for an effect instance or a transition id (principle 9); every length is a timecode.
    for (auto it = byName.cbegin(); it != byName.cend(); ++it) {
        INFO(it.key().toStdString());
        for (const QString& prompt : prompts(it.key())) {
            CHECK(prompt != QStringLiteral("effect=integer"));
            CHECK_FALSE(prompt.startsWith(QStringLiteral("transition=")));
            if (prompt.section(QLatin1Char('='), 0, 0).endsWith(QStringLiteral("uration")))
                CHECK(prompt.endsWith(QStringLiteral("=timecode")));
        }
    }
    REQUIRE(byName.contains(QStringLiteral("transition.set")));
    CHECK_FALSE(byName.value(QStringLiteral("transition.set")).value(QStringLiteral("enabled")).toBool());
    CHECK(byName.value(QStringLiteral("transition.set")).value(QStringLiteral("reason")).toString()
          == QStringLiteral("Needs values the palette cannot ask for yet"));
    CHECK(prompts(QStringLiteral("clip.addEffect")).contains(QStringLiteral("effect=choice")));
    CHECK(prompts(QStringLiteral("clip.setParam")).contains(QStringLiteral("param=choice")));
    CHECK_FALSE(prompts(QStringLiteral("clip.setParam")).contains(QStringLiteral("effect=integer")));

    // Speed/Duration… opens the dialog (Main.qml): the keymap row is runnable and the catalog command is not listed
    // twice. Apply Default Transitions runs in ProjectController.
    const auto keymapRows = KeymapCommandProvider::forApp(nullptr, &controller)->items({});
    const auto keymapRow = [&](const char* id) {
        return std::ranges::find_if(keymapRows, [&](const PaletteItem& i) { return i.key == QStringLiteral("command:") + QLatin1String(id); });
    };
    REQUIRE(keymapRow("clip.speedDuration") != keymapRows.end());
    CHECK(keymapRow("clip.speedDuration")->enabled);
    CHECK_FALSE(byName.contains(QStringLiteral("clip.speedDuration")));
    CHECK_FALSE(byName.contains(QStringLiteral("sequence.applyDefaultTransitions")));
    CHECK(keymapRow("edit.undo") != keymapRows.end());

    // 200 % on the selected clip (City B-roll, 14 s) halves it; the dry run changes nothing; one undo restores it.
    const ClipId clip = controller.primaryClip();
    REQUIRE(clip);
    const Ticks before = controller.currentSequence()->findClip(clip)->clip().duration();
    const QVariantMap params{{QStringLiteral("speed"), 200.0}};
    const auto snapshot = controller.snapshot();
    const QVariantMap preview = bridge.preview(QStringLiteral("clip.speedDuration"), params);
    INFO(preview.value(QStringLiteral("error")).toString().toStdString() << " " << preview.value(QStringLiteral("detail")).toString().toStdString());
    REQUIRE(preview.value(QStringLiteral("ok")).toBool());
    CHECK(controller.snapshot() == snapshot);
    REQUIRE(bridge.apply(QStringLiteral("clip.speedDuration"), params).value(QStringLiteral("ok")).toBool());
    const Ticks after = controller.currentSequence()->findClip(clip)->clip().duration();
    CHECK(after + after == before);
    CHECK(controller.undoText() == QStringLiteral("Undo Speed/Duration"));
    REQUIRE(controller.undo());
    CHECK(controller.currentSequence()->findClip(clip)->clip().duration() == before);
}

// ---- Accessible clip list -------------------------------------------------------------------------------------------

TEST_CASE("The accessible clip list follows edits, undo and the selection", "[app][a11y]") {
    ProjectController controller;
    ClipAccessModel* list = controller.clipList();
    REQUIRE(list->rowCount() == 16);
    const auto name = [&](int row) { return list->data(list->index(row), ClipAccessModel::AccessibleNameRole).toString(); };
    // The demo starts with City B-roll selected (V2, 4 s .. 18 s).
    const int city = list->currentRow();
    REQUIRE(city >= 0);
    // Name, track, first to last frame, length (M5j), then the state.
    CHECK(name(city) == QStringLiteral("City B-roll, V2, 00:00:04:00 to 00:00:17:23, %1 long, selected")
                            .arg(list->data(list->index(city), ClipAccessModel::DurationRole).toString()));
    CHECK(list->data(list->index(city), ClipAccessModel::TrackKindRole) == QStringLiteral("video"));

    // Delete it: one row fewer; undo brings it back.
    controller.deleteSelection();
    CHECK(list->rowCount() == 15);
    CHECK(list->currentRow() == -1);
    REQUIRE(controller.undo());
    CHECK(list->rowCount() == 16);

    // Ripple delete Interview A (V1, with its audio): later clips on V1 now start earlier.
    const SequencePtr seq = controller.currentSequence();
    controller.selectClipById(seq->videoTracks[0]->clips.front().id, ProjectController::Replace);
    const ClipId interviewB = seq->videoTracks[0]->clips[1].id;
    controller.rippleDeleteSelection();
    const int row = list->rowOf(interviewB);
    REQUIRE(row >= 0);
    CHECK(name(row).startsWith(QStringLiteral("Interview B, V1, 00:00:00:00 to")));
    REQUIRE(controller.undo());
    CHECK(name(list->rowOf(interviewB)).startsWith(QStringLiteral("Interview B, V1, 00:00:24:00 to")));

    // Selection and lock flags.
    controller.selectClipById(interviewB, ProjectController::Replace, false);
    CHECK(list->data(list->index(list->rowOf(interviewB)), ClipAccessModel::SelectedRole).toBool());
    CHECK(list->currentRow() == list->rowOf(interviewB));
    controller.setTrackFlag(trackKey(seq->videoTracks[0]->id), QStringLiteral("locked"), true);
    CHECK(name(list->rowOf(interviewB)).endsWith(QStringLiteral(", selected, locked")));
}

TEST_CASE("Keyboard only: move between clips, choose an edge and trim it", "[app][a11y]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    QQuickItem* access = shell.item("timelineClipAccess");
    REQUIRE(access != nullptr);
    access->forceActiveFocus(Qt::TabFocusReason);
    REQUIRE(access->hasActiveFocus());
    const auto tap = [&](int key, Qt::KeyboardModifiers mods = {}, const QString& text = {}) {
        QKeyEvent press(QEvent::KeyPress, key, mods, text);
        QCoreApplication::sendEvent(shell.window.get(), &press);
        QKeyEvent release(QEvent::KeyRelease, key, mods, text);
        QCoreApplication::sendEvent(shell.window.get(), &release);
    };
    const auto selected = [&] { return controller->selectedClip().value(QStringLiteral("name")).toString(); };
    const auto range = [&] { return controller->selectedClip().value(QStringLiteral("rangeText")).toString(); };
    const Qt::KeyboardModifier trim = QGuiApplication::platformName() == QLatin1String("cocoa") ? Qt::AltModifier : Qt::ControlModifier;

    REQUIRE(selected() == QStringLiteral("City B-roll"));
    tap(Qt::Key_Right); // the next clip on V2
    CHECK(selected() == QStringLiteral("Drone Coast"));
    tap(Qt::Key_Left);
    CHECK(selected() == QStringLiteral("City B-roll"));
    tap(Qt::Key_Up); // V3 above: the clip nearest in time
    CHECK(selected() == QStringLiteral("Lower Third"));
    tap(Qt::Key_Down);
    CHECK(selected() == QStringLiteral("City B-roll"));
    CHECK(controller->playheadText() == QStringLiteral("00:00:12:08")); // arrows here do not move the playhead

    // ] chooses the tail; Ctrl+→ (⌥→ on macOS) trims it a frame, Shift five.
    tap(Qt::Key_BracketRight, {}, QStringLiteral("]"));
    CHECK(controller->selectedEdge() == QStringLiteral("tail"));
    tap(Qt::Key_Right, trim);
    CHECK(range() == QStringLiteral("00:00:04:00 → 00:00:18:01"));
    tap(Qt::Key_Left, trim | Qt::ShiftModifier);
    CHECK(range() == QStringLiteral("00:00:04:00 → 00:00:17:20"));
    // [ chooses the head.
    tap(Qt::Key_BracketLeft, {}, QStringLiteral("["));
    tap(Qt::Key_Right, trim);
    CHECK(range() == QStringLiteral("00:00:04:01 → 00:00:17:20"));
    CHECK(controller->undoText() == QStringLiteral("Undo Trim"));
    REQUIRE(controller->undo());
    REQUIRE(controller->undo());
    REQUIRE(controller->undo());
    CHECK(range() == QStringLiteral("00:00:04:00 → 00:00:18:00"));

    // The current row of the accessible list follows, with a spoken name.
    auto* list = qobject_cast<QQuickItem*>(shell.object("timelineClipList"));
    REQUIRE(list != nullptr);
    CHECK(list->property("currentIndex").toInt() == controller->clipList()->currentRow());
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// ---- Drag polish: auto-scroll and a new track on drop ---------------------------------------------------------------

TEST_CASE("Auto-scroll speed grows with the depth into the edge zone", "[app][timeline][edit]") {
    CHECK(autoScrollVelocity(500, 1000, 50, 1000) == 0.0);
    CHECK(autoScrollVelocity(50, 1000, 50, 1000) == 0.0);        // zone border
    CHECK(autoScrollVelocity(25, 1000, 50, 1000) == Catch::Approx(-500)); // halfway in: half speed
    CHECK(autoScrollVelocity(0, 1000, 50, 1000) == Catch::Approx(-1000));
    CHECK(autoScrollVelocity(-80, 1000, 50, 1000) == Catch::Approx(-1000)); // beyond the edge: full speed
    CHECK(autoScrollVelocity(990, 1000, 50, 1000) == Catch::Approx(800));
    CHECK(autoScrollVelocity(10, 60, 50, 1000) == Catch::Approx(-2000.0 / 3)); // the zone is at most half the view (30 px)
}

TEST_CASE("Dragging near the edge scrolls the timeline and the drag follows", "[app][timeline][edit]") {
    Rig rig(R"(
        V1 |AaaaBbbb........................................................................................................|
    )");
    // Move B towards the right edge of the 900 px view and hold it there.
    rig.drag({rig.x(5.5), rig.y("V1")}, {880, rig.y("V1")}, {}, false);
    REQUIRE(rig.view->gestureActive());
    const QString readout = rig.view->gestureReadout();
    const double scrollBefore = rig.view->scrollX();
    for (int i = 0; i < 20; ++i) {
        rig.view->stepAutoScroll(0.05);
        rig.harness.frame();
    }
    CHECK(rig.view->scrollX() > scrollBefore + 100);
    CHECK(rig.view->gestureReadout() != readout); // the clip kept moving with the content
    rig.release({880, rig.y("V1")});
    CHECK_FALSE(rig.view->autoScrolling());
    // B ended up further right than the pointer alone would have put it.
    const auto seq = rig.controller.currentSequence();
    CHECK(seq->videoTracks[0]->clips.back().start() > f(87));
}

TEST_CASE("Dropping an audio clip below the last track creates a new track", "[app][timeline][edit]") {
    Rig rig(R"(
        V1 |AaaaBbbb|
        A1 |AaaaBbbb|
    )");
    const double below = rig.y("A1") + 60; // under the last track
    rig.drag({rig.x(5.5), rig.y("A1")}, {rig.x(5.5), below}, {}, false);
    REQUIRE(rig.view->gestureActive());
    CHECK(rig.view->newTrackLabel() == QStringLiteral("A2"));
    CHECK(rig.view->rows().size() == 3); // the preview shows the new track
    CHECK(rig.controller.currentSequence()->audioTracks.size() == 1); // the model does not have it yet
    rig.release({rig.x(5.5), below});
    const auto seq = rig.controller.currentSequence();
    REQUIRE(seq->audioTracks.size() == 2);
    CHECK(seq->audioTracks[1]->clips.size() == 1);
    CHECK(rig.controller.undoText() == QStringLiteral("Undo Move"));
    REQUIRE(rig.controller.undo());
    CHECK(rig.controller.currentSequence()->audioTracks.size() == 1); // one undo removes the track too
}

// ---- Screenshots: palette, Ask AI, selection styling -----------------------------------------------------------------

TEST_CASE("Palette and accessibility screenshots", "[app][qml][screenshots][palette]") {
    struct Shot {
        const char* file;
        const char* query;
        bool ai;
    };
    const Shot shots[] = {
        {"palette-commands.png", "ri", false},
        {"palette-ask-ai.png", "tighten the interview", true},
    };
    for (const Shot& shot : shots) {
        resetSettings();
        Shell shell;
        REQUIRE(shell.window != nullptr);
        shell.resize(kShotWidth, kShotHeight);
        QObject* overlay = shell.object("commandPalette");
        REQUIRE(overlay != nullptr);
        spin(300);
        // Ctrl+Shift+P through the window's shortcut map (as the platform would deliver it).
        QWindowSystemInterface::handleShortcutEvent(shell.window.get(), 0, Qt::Key_P,
                                                    Qt::ControlModifier | Qt::ShiftModifier, 0, 0, 0, QStringLiteral("P"));
        QCoreApplication::processEvents();
        if (!overlay->property("opened").toBool())
            QMetaObject::invokeMethod(overlay, "openPalette");
        REQUIRE(waitFor([&] { return overlay->property("opened").toBool(); }));
        auto* palette = shell.engine.singletonInstance<CommandPalette*>("Wabcut", "CommandPalette");
        REQUIRE(palette != nullptr);
        auto* field = qobject_cast<QQuickItem*>(shell.object("paletteField"));
        REQUIRE(field != nullptr);
        field->setProperty("text", QString::fromLatin1(shot.query));
        palette->setQuery(QString::fromLatin1(shot.query));
        if (shot.ai) {
            const QVariantList rows = palette->results();
            INFO("rows " << rows.size() << ", last " << (rows.isEmpty() ? std::string() : rows.last().toMap().value(QStringLiteral("key")).toString().toStdString()));
            const QString error = palette->activate(QStringLiteral("ai:ask"));
            INFO(error.toStdString());
            REQUIRE(error.isEmpty());
        }
        spin(600); // the spring settles
        if (shot.ai)
            CHECK(overlay->property("mode").toString() == QStringLiteral("ai"));
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(outputDir() + QLatin1Char('/') + QLatin1String(shot.file)));
    }

    // Selected clip: accent outline plus a corner bracket; the chosen edge as a bracket; targeted tracks ringed.
    resetSettings();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    controller->setSelectedEdge(QStringLiteral("tail"));
    spin(300);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    REQUIRE(image.save(outputDir() + QStringLiteral("/selection-a11y.png")));
    auto* lanes = shell.item("timelineLanes");
    REQUIRE(lanes != nullptr);
    const QRectF area = lanes->mapRectToScene(QRectF(0, 0, lanes->width(), lanes->height()));
    const double dpr = shell.window->effectiveDevicePixelRatio();
    REQUIRE(image.copy(QRectF(area.x() * dpr - 190 * dpr, area.y() * dpr, 1080 * dpr, 130 * dpr).toRect())
                .save(outputDir() + QStringLiteral("/selection-a11y-crop.png")));
}
