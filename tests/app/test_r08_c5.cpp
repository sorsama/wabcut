// Stream C, batch C5 (app level):
//   * 025 Consolidate Duplicates: the Library bridge (preview, merge in one undo step), Edit > Consolidate Duplicates… and
//         Remove Unused… (greyed with the reason, then the Library's confirmation), screenshots (dark and light);
//   * 179 the rights and clearance record: the Rights bridge (log, set, export, credits roll) and the dialog (rows, editing
//         several items, errors, the credits roll), screenshots (dark, light, narrow);
//   * 062 a poly WAV's channel names in the Properties lines and the Audio Channels dialog's information;
//   * 076 the Collect Files dialog's Folders choice.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "AudioChannelsBridge.h"
#include "BridgeIds.h"
#include "CommandPalette.h"
#include "KeymapDispatcher.h"
#include "LibraryBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "RightsBridge.h"

#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Rights.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>

#include <functional>
#include <memory>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

QQuickItem* findWith(QQuickItem* root, const std::function<bool(QQuickItem*)>& match) {
    if (!root)
        return nullptr;
    if (match(root))
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findWith(child, match))
            return found;
    }
    return nullptr;
}

QQuickItem* byName(QQuickItem* root, const QString& name) {
    return findWith(root, [&](QQuickItem* i) { return i->objectName() == name; });
}

/// The popup (menu, popover, dialog) holding `item`: its item under the overlay.
QQuickItem* popupOf(QQuickItem* item) {
    QQuickItem* at = item;
    while (at && at->parentItem() && !QByteArray(at->parentItem()->metaObject()->className()).contains("Overlay"))
        at = at->parentItem();
    return at;
}

bool saveCard(const Shell& shell, QQuickItem* inside, const QString& file) {
    const QImage image = shell.grab();
    if (image.isNull() || !inside)
        return false;
    QQuickItem* card = popupOf(inside);
    const double dpr = image.devicePixelRatio();
    const QRectF area = card->mapRectToScene(QRectF(0, 0, card->width(), card->height()))
                            .adjusted(-8, -8, 8, 8)
                            .intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    return image.copy(QRectF(area.x() * dpr, area.y() * dpr, area.width() * dpr, area.height() * dpr).toRect())
        .save(QDir(outputDir()).filePath(file));
}

/// Media `letter` of `state` as a file with the identity `hash`.
ProjectState withFile(ProjectState state, MediaId id, const std::string& hash) {
    for (MediaPtr& m : state.media) {
        if (m->id == id) {
            auto item = std::make_shared<MediaItem>(*m);
            item->locator.path = "D:/Footage/" + item->name + ".mp4";
            item->locator.fingerprint = SourceFingerprint{7'000'000, 1, hash};
            m = item;
        }
    }
    return state;
}

ProjectState renamed(ProjectState state, MediaId id, const std::string& name) {
    for (MediaPtr& m : state.media) {
        if (m->id == id) {
            auto item = std::make_shared<MediaItem>(*m);
            item->name = name;
            m = item;
        }
    }
    return state;
}

ProjectState withRights(const ProjectState& state, MediaId id, const std::function<void(edit::MediaRightsChange&)>& fill) {
    edit::MediaRightsChange change;
    fill(change);
    auto out = edit::setMediaRights(state, std::vector<MediaId>{id}, change);
    REQUIRE(out);
    return *out;
}

/// A project from a timeline picture and a Library over it (synchronous, no settings).
struct Rig {
    Timeline tl;
    ProjectController controller;
    LibraryBridge library;

    explicit Rig(std::string_view picture) : tl(picture) {
        controller.resetProject(tl.state, tl.seq);
        library.setPersistent(false);
        library.setSynchronous(true);
        library.setController(&controller);
    }
    void reset(const ProjectState& state) { controller.resetProject(state, tl.seq); }
    std::size_t undo() { return controller.session().undoCount(); }
};

} // namespace

// ---- 025 ------------------------------------------------------------------------------------------------------------

TEST_CASE("Library: Consolidate Duplicates previews the sets and merges them in one undo step (025)", "[app][library][v07]") {
    Rig r("    V1 tgt |Aaaa..Bbbb..Cccc|\n");
    const MediaId a = r.tl.media('A');
    const MediaId b = r.tl.media('B');
    const MediaId c = r.tl.media('C');
    r.reset(withFile(withFile(withFile(r.tl.state, a, "aaaaaaaaaaaaaaaa"), b, "aaaaaaaaaaaaaaaa"), c, "cccccccccccccccc"));
    REQUIRE(r.library.duplicateGroups().size() == 1);

    const QVariantList preview = r.library.consolidatePreview();
    REQUIRE(preview.size() == 1);
    const QVariantMap set = preview.front().toMap();
    CHECK(set.value(QStringLiteral("keeper")).toString() == mediaKey(a));
    REQUIRE(set.value(QStringLiteral("removed")).toList().size() == 1);
    CHECK(set.value(QStringLiteral("removed")).toList().front().toMap().value(QStringLiteral("key")).toString() == mediaKey(b));
    CHECK(set.value(QStringLiteral("clips")).toInt() == 1);
    CHECK(r.controller.snapshot()->findMedia(b) != nullptr); // a preview changes nothing

    const std::size_t before = r.undo();
    CHECK(r.library.consolidateDuplicates() == 1);
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.snapshot()->findMedia(b) == nullptr);
    CHECK(r.controller.snapshot()->findMedia(c) != nullptr);
    CHECK(r.library.message().contains(QStringLiteral("Merged 1 duplicate item")));
    CHECK(r.library.message().contains(QStringLiteral("1 clip")));
    CHECK(r.library.duplicateGroups().isEmpty());
    CHECK(r.library.consolidatePreview().isEmpty());

    REQUIRE(r.controller.session().undo());
    CHECK(r.controller.snapshot()->findMedia(b) != nullptr);
    CHECK(r.library.duplicateGroups().size() == 1);
}

TEST_CASE("Edit: Consolidate Duplicates and Remove Unused grey out with the reason and ask in the Library; screenshots (025)",
          "[app][library][menubar][qml][screenshot][v07]") {
    for (const bool dark : {true, false}) {
        resetSettings();
        clearMessages();
        Shell shell;
        REQUIRE(shell.window);
        INFO(join(shell.errors));
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        if (!dark) {
            shell.setAppearance(false, "ultraviolet");
            REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
        }
        auto* project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
        auto* palette = shell.engine.singletonInstance<CommandPalette*>("Wabcut", "CommandPalette");
        auto* keys = shell.engine.singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher");
        REQUIRE(project);
        REQUIRE(controller);
        REQUIRE(library);
        REQUIRE(palette);
        REQUIRE(keys);
        project->newProject();
        // A and B are one file (B is the one the cut does not use); D is in no sequence at all.
        Timeline tl("V1 tgt |Aaaa..Cccc|");
        ProjectState state = withFile(withFile(tl.state, tl.media('A'), "aaaaaaaaaaaaaaaa"), tl.media('B'), "aaaaaaaaaaaaaaaa");
        state = renamed(renamed(state, tl.media('A'), "Harbour 1931.mov"), tl.media('B'), "Harbour 1931 copy.mov");
        controller->resetProject(tl.state, tl.seq);
        spin(50);
        REQUIRE_FALSE(project->demo());
        library->setPersistent(false);
        REQUIRE_FALSE(shell.grab().isNull());

        // Nothing is a duplicate yet: greyed, with the reason (Remove Unused is available: B..Z are not on the timeline).
        CHECK_FALSE(palette->commandState(QStringLiteral("edit.consolidateDuplicates")).value(QStringLiteral("enabled")).toBool());
        CHECK(palette->commandState(QStringLiteral("edit.consolidateDuplicates")).value(QStringLiteral("reason")).toString() ==
              QStringLiteral("There are no duplicate media items in the project"));
        CHECK(palette->commandState(QStringLiteral("edit.removeUnused")).value(QStringLiteral("enabled")).toBool());

        controller->resetProject(state, tl.seq);
        REQUIRE(waitFor([&] { return library->duplicateGroups().size() == 1; }, 5000));
        CHECK(palette->commandState(QStringLiteral("edit.consolidateDuplicates")).value(QStringLiteral("enabled")).toBool());

        // The rows are in the Edit menu.
        QObject* bar = shell.object("appMenuBar");
        REQUIRE(bar);
        REQUIRE(QMetaObject::invokeMethod(bar, "open", Q_ARG(int, 1), Q_ARG(bool, false)));
        QObject* editMenu = shell.object("editMenu");
        REQUIRE(editMenu);
        REQUIRE(waitFor([&] { return editMenu->property("opened").toBool(); }, 3000));
        QObject* row = editMenu->findChild<QObject*>(QStringLiteral("editMenuConsolidateDuplicates"));
        REQUIRE(row);
        CHECK(row->property("enabled").toBool());
        CHECK(editMenu->findChild<QObject*>(QStringLiteral("editMenuRemoveUnused")) != nullptr);
        REQUIRE(QMetaObject::invokeMethod(editMenu, "close"));
        REQUIRE(waitFor([&] { return !editMenu->property("visible").toBool(); }, 3000));

        // The key (palette, menu) asks in the Library first: the popover lists the kept item and the one that goes.
        QQuickItem* panel = shell.item("libraryPanel");
        REQUIRE(panel);
        REQUIRE(keys->trigger(QStringLiteral("edit.consolidateDuplicates")));
        QObject* popover = nullptr;
        REQUIRE(waitFor([&] {
            popover = panel->findChild<QObject*>(QStringLiteral("libraryConsolidatePopover"));
            return popover && popover->property("opened").toBool();
        }, 5000));
        QQuickItem* root = shell.window->contentItem();
        QQuickItem* text = byName(root, QStringLiteral("libraryConsolidateText"));
        REQUIRE(text);
        CHECK(text->property("text").toString().contains(QStringLiteral("Merge 1 duplicate item")));
        QQuickItem* groups = byName(root, QStringLiteral("libraryConsolidateGroups"));
        REQUIRE(groups);
        CHECK(groups->property("count").toInt() == 1);
        spin(150);
        REQUIRE(saveCard(shell, text, dark ? QStringLiteral("library-consolidate-dark.png") : QStringLiteral("library-consolidate-light.png")));

        const std::size_t before = controller->session().undoCount();
        QMetaObject::invokeMethod(byName(root, QStringLiteral("libraryConsolidateConfirm")), "clicked");
        REQUIRE(waitFor([&] { return !popover->property("opened").toBool(); }, 3000));
        CHECK(controller->session().undoCount() == before + 1);
        CHECK(controller->snapshot()->findMedia(tl.media('B')) == nullptr);
        CHECK(controller->snapshot()->findMedia(tl.media('A')) != nullptr);
        CHECK_FALSE(palette->commandState(QStringLiteral("edit.consolidateDuplicates")).value(QStringLiteral("enabled")).toBool());

        // Remove Unused asks the same way.
        REQUIRE(keys->trigger(QStringLiteral("edit.removeUnused")));
        QObject* unused = nullptr;
        REQUIRE(waitFor([&] {
            unused = panel->findChild<QObject*>(QStringLiteral("libraryRemoveUnusedPopover"));
            return unused && unused->property("opened").toBool();
        }, 5000));
        QMetaObject::invokeMethod(unused, "close");
        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
}

// ---- 179 ------------------------------------------------------------------------------------------------------------

namespace {

/// A cut of three archive sources and the Rights bridge over its controller.
struct RightsRig {
    Rig rig;
    RightsBridge rights;
    MediaId a, b, c;

    RightsRig() : rig("    V1 tgt |Aaaa..Bbbb..Cccc|\n") {
        rights.setController(&rig.controller);
        a = rig.tl.media('A');
        b = rig.tl.media('B');
        c = rig.tl.media('C');
        ProjectState s = renamed(renamed(renamed(rig.tl.state, a, "Harbour 1931"), b, "Parade 1946"), c, "Own interview");
        s = withRights(s, a, [](edit::MediaRightsChange& x) {
            x.status = ClearanceStatus::Cleared;
            x.source = "Newsreel Archive";
            x.licence = "Worldwide, all media";
            x.territory = "Worldwide";
            x.expiry = "2026-12-15";
            x.costMinor = std::optional<std::int64_t>(40000);
            x.currency = "USD";
            x.creditLine = "Archive footage courtesy of Newsreel Archive";
        });
        s = withRights(s, b, [](edit::MediaRightsChange& x) {
            x.status = ClearanceStatus::Pending;
            x.source = "Pathe";
            x.costMinor = std::optional<std::int64_t>(25000);
            x.currency = "USD";
        });
        rig.reset(s);
    }
};

} // namespace

TEST_CASE("Rights bridge: the log, setting rights, the spreadsheet and the credits roll (179)", "[app][rights][v07]") {
    RightsRig r;
    const QVariantMap log = r.rights.log({{QStringLiteral("asOf"), QStringLiteral("2026-12-01")}});
    REQUIRE(log.value(QStringLiteral("ok")).toBool());
    const QVariantList rows = log.value(QStringLiteral("rows")).toList();
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].toMap().value(QStringLiteral("key")).toString() == mediaKey(r.a));
    CHECK(rows[0].toMap().value(QStringLiteral("statusLabel")).toString() == QStringLiteral("Cleared"));
    CHECK(rows[0].toMap().value(QStringLiteral("expiresSoon")).toBool());
    CHECK(rows[1].toMap().value(QStringLiteral("uncleared")).toBool());
    CHECK_FALSE(rows[2].toMap().value(QStringLiteral("hasRecord")).toBool());
    CHECK(log.value(QStringLiteral("totals")).toMap().value(QStringLiteral("needAttention")).toInt() == 2);
    // Only what needs attention; the whole project; a sequence-less request is explained.
    CHECK(r.rights.log({{QStringLiteral("asOf"), QStringLiteral("2026-12-01")}, {QStringLiteral("flaggedOnly"), true}})
              .value(QStringLiteral("rows")).toList().size() == 2);
    CHECK(r.rights.log({{QStringLiteral("scope"), QStringLiteral("project")}}).value(QStringLiteral("ok")).toBool());

    // Setting rights: several items, one undo step, the cost typed the way people type it.
    const std::size_t before = r.rig.undo();
    CHECK(r.rights.setRights({mediaKey(r.b), mediaKey(r.c)},
                             {{QStringLiteral("status"), QStringLiteral("cleared")}, {QStringLiteral("cost"), QStringLiteral("1,250.5")},
                              {QStringLiteral("currency"), QStringLiteral("GBP")}}).isEmpty());
    CHECK(r.rig.undo() == before + 1);
    CHECK(r.rig.controller.snapshot()->findMedia(r.b)->rights->costMinor == 125050);
    CHECK(r.rig.controller.snapshot()->findMedia(r.c)->rights->status == ClearanceStatus::Cleared);
    CHECK(r.rights.setRights({mediaKey(r.b)}, {{QStringLiteral("expiry"), QStringLiteral("soon")}}).contains(QStringLiteral("YYYY-MM-DD")));
    CHECK(r.rights.setRights({}, {{QStringLiteral("status"), QStringLiteral("cleared")}}).contains(QStringLiteral("Choose")));
    CHECK(r.rig.undo() == before + 1); // the refusals changed nothing
    CHECK(r.rights.normalisedCost(QStringLiteral("1.250,5")) == QStringLiteral("1250.50"));
    CHECK(r.rights.normalisedCost(QStringLiteral("lots")).isEmpty());

    // The spreadsheet: written atomically with a byte order mark so Excel reads UTF-8; JSON without.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString csvFile = dir.filePath(QStringLiteral("rights.csv"));
    const QVariantMap written = r.rights.writeLog(csvFile, {{QStringLiteral("asOf"), QStringLiteral("2026-12-01")}});
    REQUIRE(written.value(QStringLiteral("ok")).toBool());
    QFile csv(csvFile);
    REQUIRE(csv.open(QIODevice::ReadOnly));
    const QByteArray bytes = csv.readAll();
    CHECK(bytes.startsWith("\xEF\xBB\xBF" "Media Name,File Path,Clearance,"));
    CHECK(bytes.contains("Harbour 1931,"));
    CHECK(bytes.contains("Archive footage courtesy of Newsreel Archive"));
    const QString jsonFile = dir.filePath(QStringLiteral("rights.json"));
    REQUIRE(r.rights.writeLog(jsonFile, {}).value(QStringLiteral("ok")).toBool());
    QFile json(jsonFile);
    REQUIRE(json.open(QIODevice::ReadOnly));
    CHECK(json.readAll().startsWith("{"));
    CHECK_FALSE(r.rights.writeLog(QStringLiteral("relative.csv"), {}).value(QStringLiteral("ok")).toBool());

    // The credits roll: a graphic clip after the cut, one undo step; with no credit line anywhere it says so.
    const std::size_t beforeRoll = r.rig.undo();
    const QVariantMap roll = r.rights.makeCreditsRoll({{QStringLiteral("heading"), QStringLiteral("Archive Material")}});
    REQUIRE(roll.value(QStringLiteral("ok")).toBool());
    CHECK(r.rig.undo() == beforeRoll + 1);
    CHECK(roll.value(QStringLiteral("credits")).toStringList() == QStringList{QStringLiteral("Archive footage courtesy of Newsreel Archive")});
    CHECK(roll.value(QStringLiteral("clip")).toString().startsWith(QLatin1Char('c')));
    REQUIRE(r.rig.controller.session().undo());
    ProjectState bare = r.rig.controller.snapshot() ? *r.rig.controller.snapshot() : ProjectState{};
    bare = withRights(bare, r.a, [](edit::MediaRightsChange& x) { x.creditLine = ""; });
    r.rig.reset(bare);
    const QVariantMap none = r.rights.makeCreditsRoll({});
    CHECK_FALSE(none.value(QStringLiteral("ok")).toBool());
    CHECK(none.value(QStringLiteral("error")).toString().contains(QStringLiteral("credit line")));
}

TEST_CASE("Rights and Clearance Log dialog: rows, editing several items, errors and the credits roll; screenshots (179)",
          "[app][rights][qml][screenshot][v07]") {
    for (const bool dark : {true, false}) {
        resetSettings();
        clearMessages();
        Shell shell;
        REQUIRE(shell.window);
        INFO(join(shell.errors));
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        if (!dark) {
            shell.setAppearance(false, "ultraviolet");
            REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
        }
        auto* project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        auto* rights = shell.engine.singletonInstance<RightsBridge*>("Wabcut", "Rights");
        REQUIRE(project);
        REQUIRE(controller);
        REQUIRE(rights);
        project->newProject();
        Timeline tl("V1 tgt |Aaaa..Bbbb..Cccc|");
        const MediaId a = tl.media('A');
        const MediaId b = tl.media('B');
        const MediaId c = tl.media('C');
        ProjectState s = renamed(renamed(renamed(tl.state, a, "Harbour 1931"), b, "Parade 1946"), c, "Own interview");
        s = withRights(s, a, [](edit::MediaRightsChange& x) {
            x.status = ClearanceStatus::Cleared;
            x.source = "Newsreel Archive";
            x.licence = "Worldwide, all media";
            x.territory = "Worldwide";
            x.expiry = "2099-12-15";
            x.costMinor = std::optional<std::int64_t>(40000);
            x.currency = "USD";
            x.creditLine = "Archive footage courtesy of Newsreel Archive";
        });
        s = withRights(s, b, [](edit::MediaRightsChange& x) {
            x.status = ClearanceStatus::Pending;
            x.source = "Pathe";
        });
        controller->resetProject(s, tl.seq);
        spin(50);
        REQUIRE_FALSE(project->demo());
        REQUIRE_FALSE(shell.grab().isNull());
        QQuickItem* root = shell.window->contentItem();

        rights->openFor({mediaKey(a)});
        QObject* loader = shell.object("rightsLogDialogLoader");
        REQUIRE(loader);
        QObject* dialog = nullptr;
        REQUIRE(waitFor([&] {
            dialog = qvariant_cast<QObject*>(loader->property("item"));
            return dialog && dialog->property("opened").toBool();
        }, 5000));
        spin(150);
        QQuickItem* list = byName(root, QStringLiteral("rightsRows"));
        REQUIRE(list);
        CHECK(list->property("count").toInt() == 3);
        // The item the Library menu named is picked and the editor shows what it says.
        CHECK(dialog->property("selectedKeys").toStringList() == QStringList{mediaKey(a)});
        QQuickItem* sourceInput = byName(root, QStringLiteral("rightsInput_source"));
        REQUIRE(sourceInput);
        CHECK(sourceInput->property("text").toString() == QStringLiteral("Newsreel Archive"));
        CHECK(byName(root, QStringLiteral("rightsSummary"))->property("text").toString().contains(QStringLiteral("3 source")));
        spin(100);
        REQUIRE(saveCard(shell, list, dark ? QStringLiteral("rights-log-dark.png") : QStringLiteral("rights-log-light.png")));

        // Edit two items together: the field they differ in stays empty ("various") until typed in; only changed fields are written.
        REQUIRE(QMetaObject::invokeMethod(dialog, "select", Q_ARG(QString, mediaKey(b)), Q_ARG(bool, true), Q_ARG(bool, false)));
        CHECK(dialog->property("selectedKeys").toStringList().size() == 2);
        CHECK(sourceInput->property("text").toString().isEmpty());
        QQuickItem* apply = byName(root, QStringLiteral("rightsApply"));
        REQUIRE(apply);
        CHECK_FALSE(apply->isEnabled());
        QQuickItem* territory = byName(root, QStringLiteral("rightsInput_territory"));
        REQUIRE(territory);
        territory->setProperty("text", QStringLiteral("UK and Ireland"));
        REQUIRE(QMetaObject::invokeMethod(territory, "textEdited"));
        CHECK(apply->isEnabled());
        const std::size_t before = controller->session().undoCount();
        REQUIRE(QMetaObject::invokeMethod(apply, "clicked"));
        CHECK(controller->session().undoCount() == before + 1);
        CHECK(controller->snapshot()->findMedia(a)->rights->territory == "UK and Ireland");
        CHECK(controller->snapshot()->findMedia(b)->rights->territory == "UK and Ireland");
        CHECK(controller->snapshot()->findMedia(a)->rights->source == "Newsreel Archive"); // not named: kept
        CHECK(controller->snapshot()->findMedia(b)->rights->source == "Pathe");

        // A bad date is explained and nothing is written.
        QQuickItem* expiry = byName(root, QStringLiteral("rightsInput_expiry"));
        REQUIRE(expiry);
        expiry->setProperty("text", QStringLiteral("someday"));
        REQUIRE(QMetaObject::invokeMethod(expiry, "textEdited"));
        const std::size_t steps = controller->session().undoCount();
        REQUIRE(QMetaObject::invokeMethod(apply, "clicked"));
        CHECK(controller->session().undoCount() == steps);
        CHECK(byName(root, QStringLiteral("rightsError"))->property("text").toString().contains(QStringLiteral("YYYY-MM-DD")));

        // Pending -> cleared through the status tabs, for the one item picked alone.
        REQUIRE(QMetaObject::invokeMethod(dialog, "select", Q_ARG(QString, mediaKey(b)), Q_ARG(bool, false), Q_ARG(bool, false)));
        QQuickItem* tabs = byName(root, QStringLiteral("rightsStatusTabs"));
        REQUIRE(tabs);
        CHECK(tabs->property("currentIndex").toInt() == 2); // Pending
        REQUIRE(QMetaObject::invokeMethod(tabs, "select", Q_ARG(QVariant, 1)));
        REQUIRE(QMetaObject::invokeMethod(apply, "clicked"));
        CHECK(controller->snapshot()->findMedia(b)->rights->status == ClearanceStatus::Cleared);

        // Needs attention only: with everything cleared and a far expiry nothing is flagged.
        dialog->setProperty("flaggedOnly", true);
        REQUIRE(QMetaObject::invokeMethod(dialog, "refresh"));
        CHECK(list->property("count").toInt() == 0);
        dialog->setProperty("flaggedOnly", false);
        REQUIRE(QMetaObject::invokeMethod(dialog, "refresh"));

        // The credits roll closes the dialog and adds the graphic after the cut.
        const std::size_t beforeRoll = controller->session().undoCount();
        REQUIRE(QMetaObject::invokeMethod(byName(root, QStringLiteral("rightsCreditsRoll")), "clicked"));
        CHECK(controller->session().undoCount() == beforeRoll + 1);
        REQUIRE(waitFor([&] { return !dialog->property("opened").toBool(); }, 3000));
        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
}

TEST_CASE("Rights and Clearance Log dialog: nothing is cut off at the smallest window (179)", "[app][rights][qml][screenshot][v07]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(1100, 700);
    shell.theme->setProperty("reduceMotion", true);
    auto* project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* rights = shell.engine.singletonInstance<RightsBridge*>("Wabcut", "Rights");
    REQUIRE(project);
    project->newProject();
    Timeline tl("V1 tgt |Aaaa..Bbbb|");
    ProjectState s = withRights(renamed(tl.state, tl.media('A'), "A clip with a long name that has to be shortened somewhere in the row"),
                                tl.media('A'), [](edit::MediaRightsChange& x) {
        x.status = ClearanceStatus::Pending;
        x.source = "An archive with a very long name, Inc.";
        x.licence = "Worldwide, all media, in perpetuity, for the film and its trailers";
        x.creditLine = "Archive footage courtesy of an archive with a very long name, Inc.";
    });
    controller->resetProject(s, tl.seq);
    spin(50);
    REQUIRE_FALSE(shell.grab().isNull());
    rights->openFor({mediaKey(tl.media('A'))});
    QObject* loader = shell.object("rightsLogDialogLoader");
    REQUIRE(loader);
    QObject* dialog = nullptr;
    REQUIRE(waitFor([&] {
        dialog = qvariant_cast<QObject*>(loader->property("item"));
        return dialog && dialog->property("opened").toBool();
    }, 5000));
    spin(200);
    QQuickItem* root = shell.window->contentItem();
    QQuickItem* list = byName(root, QStringLiteral("rightsRows"));
    REQUIRE(list);
    QQuickItem* card = popupOf(list);
    const QRectF cardRect = card->mapRectToScene(QRectF(0, 0, card->width(), card->height()));
    CHECK(cardRect.left() >= 0.0);
    CHECK(cardRect.right() <= 1100.0);
    CHECK(cardRect.top() >= 0.0);
    CHECK(cardRect.bottom() <= 700.0);
    // The buttons stay in view below the scrolling body.
    QQuickItem* close = byName(root, QStringLiteral("rightsClose"));
    REQUIRE(close);
    const QRectF closeRect = close->mapRectToScene(QRectF(0, 0, close->width(), close->height()));
    CHECK(closeRect.bottom() <= 700.0);
    CHECK(closeRect.right() <= 1100.0);
    REQUIRE(saveCard(shell, list, QStringLiteral("rights-log-narrow.png")));
}

// ---- 062 ------------------------------------------------------------------------------------------------------------

TEST_CASE("A poly WAV's channel names show in Properties and in the Audio Channels information (062)", "[app][library][audio][v07]") {
    Rig r("    V1 tgt |Aaaa|\n    A1 tgt |Aaaa|\n");
    const MediaId a = r.tl.media('A');
    ProjectState s = r.tl.state;
    for (MediaPtr& m : s.media) {
        if (m->id == a) {
            auto item = std::make_shared<MediaItem>(*m);
            item->info.audio.front().channels = 3;
            item->info.audio.front().channelNames = {"Boom", "", "Lav A"};
            m = item;
        }
    }
    r.reset(s);
    const QString text = r.library.propertiesText(mediaKey(a));
    CHECK(text.contains(QStringLiteral("1 Boom")));
    CHECK(text.contains(QStringLiteral("3 Lav A")));
    CHECK_FALSE(text.contains(QStringLiteral("2 ") + QStringLiteral("Lav"))); // the channel with no name is left out

    AudioChannelsBridge channels;
    channels.setController(&r.controller);
    const QVariantMap media = channels.mediaInfo({mediaKey(a)});
    REQUIRE(media.value(QStringLiteral("ok")).toBool());
    CHECK(media.value(QStringLiteral("sourceNames")).toStringList() == QStringList{QStringLiteral("Boom"), QString(), QStringLiteral("Lav A")});
    const ClipId clip = clipAt(*r.controller.snapshot(), r.tl.seq, "A1@0");
    const QVariantMap clips = channels.info({clipKey(clip)});
    REQUIRE(clips.value(QStringLiteral("ok")).toBool());
    CHECK(clips.value(QStringLiteral("sourceNames")).toStringList() == QStringList{QStringLiteral("Boom"), QString(), QStringLiteral("Lav A")});
}

// ---- 076 ------------------------------------------------------------------------------------------------------------

TEST_CASE("Collect Files dialog: the Folders choice is passed on and remembered; screenshot (076)", "[app][collect][qml][screenshot][v07]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    INFO(join(shell.errors));
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(project);
    project->newProject();
    Timeline tl("V1 tgt |Aaaa|");
    controller->resetProject(tl.state, tl.seq);
    spin(50);
    REQUIRE_FALSE(shell.grab().isNull());
    QObject* loader = shell.object("collectFilesDialogLoader");
    REQUIRE(loader);
    loader->setProperty("active", true);
    QObject* dialog = qvariant_cast<QObject*>(loader->property("item"));
    REQUIRE(dialog);
    REQUIRE(QMetaObject::invokeMethod(dialog, "open"));
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 5000));
    spin(150);
    QQuickItem* root = shell.window->contentItem();
    QQuickItem* tabs = byName(root, QStringLiteral("collectLayoutTabs"));
    REQUIRE(tabs);
    const auto options = [&] {
        QVariant out;
        REQUIRE(QMetaObject::invokeMethod(dialog, "options", Q_RETURN_ARG(QVariant, out)));
        return out.toMap();
    };
    CHECK(options().value(QStringLiteral("layout")).toString() == QStringLiteral("flat")); // the default stays one folder
    const QVariantList model = tabs->property("model").toList();
    REQUIRE(model.size() == 3);
    CHECK(model[1].toString() == QStringLiteral("Project bins"));
    CHECK(model[2].toString() == QStringLiteral("Original folders"));
    REQUIRE(QMetaObject::invokeMethod(tabs, "select", Q_ARG(QVariant, 1)));
    CHECK(options().value(QStringLiteral("layout")).toString() == QStringLiteral("bins"));
    CHECK(byName(root, QStringLiteral("collectLayoutNote"))->property("text").toString().contains(QStringLiteral("bins")));
    REQUIRE(QMetaObject::invokeMethod(tabs, "select", Q_ARG(QVariant, 2)));
    CHECK(options().value(QStringLiteral("layout")).toString() == QStringLiteral("folders"));
    // Remembered for the next time the dialog opens.
    CHECK(waitFor([&] { return QSettings().value(QStringLiteral("collect/layout")).toString() == QStringLiteral("folders"); }, 5000));
    spin(100);
    REQUIRE(saveCard(shell, tabs, QStringLiteral("collect-layout-dark.png")));
    resetSettings();
}

TEST_CASE("Audio Channels dialog: a poly WAV's channel names are shown above the source channels; screenshots (062)",
          "[app][audio][qml][screenshot][v07]") {
    for (const bool dark : {true, false}) {
        resetSettings();
        clearMessages();
        Shell shell;
        REQUIRE(shell.window);
        INFO(join(shell.errors));
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        if (!dark) {
            shell.setAppearance(false, "ultraviolet");
            REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
        }
        auto* project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        auto* channels = shell.engine.singletonInstance<AudioChannelsBridge*>("Wabcut", "AudioChannels");
        REQUIRE(project);
        REQUIRE(controller);
        REQUIRE(channels);
        project->newProject();
        Timeline tl("V1 tgt |Aaaa|\n        A1 tgt |Aaaa|");
        ProjectState s = tl.state;
        for (MediaPtr& m : s.media) {
            if (m->id == tl.media('A')) {
                auto item = std::make_shared<MediaItem>(*m);
                item->info.audio.front().channels = 4;
                item->info.audio.front().channelNames = {"Boom", "Lav A", "", "Mix with a rather long recorder name"};
                m = item;
            }
        }
        controller->resetProject(s, tl.seq);
        spin(100);
        REQUIRE_FALSE(shell.grab().isNull());
        const QString clip = clipKey(clipAt(*controller->snapshot(), tl.seq, "A1@0"));
        channels->openDialog({clip});
        QObject* loader = shell.object("audioChannelsDialogLoader");
        REQUIRE(loader);
        QObject* dialog = nullptr;
        REQUIRE(waitFor([&] {
            dialog = qvariant_cast<QObject*>(loader->property("item"));
            return dialog && dialog->property("opened").toBool();
        }, 5000));
        spin(200);
        QQuickItem* root = shell.window->contentItem();
        QQuickItem* first = byName(root, QStringLiteral("sourceName1"));
        QQuickItem* second = byName(root, QStringLiteral("sourceName2"));
        QQuickItem* third = byName(root, QStringLiteral("sourceName3"));
        QQuickItem* fourth = byName(root, QStringLiteral("sourceName4"));
        REQUIRE(first);
        REQUIRE(second);
        REQUIRE(third);
        REQUIRE(fourth);
        CHECK(first->property("text").toString() == QStringLiteral("Boom"));
        CHECK(second->property("text").toString() == QStringLiteral("Lav A"));
        CHECK_FALSE(third->isVisible()); // no name: nothing shown
        CHECK(fourth->isVisible());
        CHECK(fourth->property("elide").toInt() == static_cast<int>(Qt::ElideRight)); // a long name is elided, not allowed to widen the column
        REQUIRE(saveCard(shell, first, dark ? QStringLiteral("audio-channels-names-dark.png") : QStringLiteral("audio-channels-names-light.png")));
        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
}
