// 0.2: the Effects panel's presets, custom bins and favourites (EffectPresetsBridge), Save Preset from Effect Controls,
// and presets dropped on the timeline or Effect Controls. Offscreen; no window is shown.
// UX rework: stable folder keys, Import Presets into a bin and Export of one presets bin, the delete confirmation and
// the keymap's requests, applying to every selected clip, and the panel's folder tree (EffectsTreeModel): Premiere's
// folders, nested presets bins, search, filters, the Icons view's tiles, in-place updates and the kept expanded state.
// Then the panel itself in the real shell (EffectsPanel.qml): its folders and views (screenshots effects-list,
// effects-compact, effects-icons, effects-search, effects-list-light), the keyboard, deletes that ask first, renames in
// place, drops onto bins and presets files from Explorer, and Import / Export Presets through the folder menus.

#include "A11yAudit.h"
#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "EffectControls.h"
#include "EffectPresetsBridge.h"
#include "EffectsTreeModel.h"
#include "ProjectController.h"
#include "TimelineView.h"

#include <wabcut/command/ExecutionContext.h>

#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QSettings>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QUrl>
#include <qpa/qwindowsysteminterface.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

/// The group titled `title`, or {}.
QVariantMap groupNamed(const QVariantList& groups, const QString& title) {
    for (const QVariant& g : groups) {
        if (g.toMap().value(QStringLiteral("title")).toString() == title)
            return g.toMap();
    }
    return {};
}

/// The names of the items of `bin` in `group`.
QStringList itemNames(const QVariantMap& group, const QString& bin) {
    QStringList out;
    for (const QVariant& b : group.value(QStringLiteral("bins")).toList()) {
        if (b.toMap().value(QStringLiteral("title")).toString() != bin)
            continue;
        for (const QVariant& i : b.toMap().value(QStringLiteral("items")).toList())
            out << i.toMap().value(QStringLiteral("name")).toString();
    }
    return out;
}

/// The bin titled `title` of `group`, or {}.
QVariantMap binNamed(const QVariantMap& group, const QString& title) {
    for (const QVariant& b : group.value(QStringLiteral("bins")).toList()) {
        if (b.toMap().value(QStringLiteral("title")).toString() == title)
            return b.toMap();
    }
    return {};
}

/// The group of kind `kind` in a list of groups, or {}.
QVariantMap groupOfKind(const QVariantList& groups, const QString& kind) {
    for (const QVariant& g : groups) {
        if (g.toMap().value(QStringLiteral("kind")).toString() == kind)
            return g.toMap();
    }
    return {};
}

/// Every row key of the tree, in order.
QStringList rowKeys(const EffectsTreeModel& tree) {
    QStringList out;
    for (int i = 0; i < tree.rowCount(); ++i)
        out << tree.get(i).value(QStringLiteral("rowKey")).toString();
    return out;
}

/// The keys of the tree's top-level rows, in order.
QStringList topKeys(const EffectsTreeModel& tree) {
    QStringList out;
    for (int i = 0; i < tree.rowCount(); ++i) {
        if (tree.get(i).value(QStringLiteral("depth")).toInt() == 0)
            out << tree.get(i).value(QStringLiteral("rowKey")).toString();
    }
    return out;
}

/// A role of the row with key `key` (an invalid QVariant when the row does not show).
QVariant roleOf(const EffectsTreeModel& tree, const QString& key, const char* role) {
    const int row = tree.indexOf(key);
    return row < 0 ? QVariant() : tree.get(row).value(QLatin1String(role));
}

constexpr std::string_view kTwoClips = R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa..Bbbbbbbbbbbbbbbbbbbb|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa..Bbbbbbbbbbbbbbbbbbbb|
    )";

} // namespace

TEST_CASE("Effects panel: the Presets bins, favourites and custom bins", "[app][effects][presets]") {
    sharedEffectLibrary()->set({});
    Rig rig(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    EffectPresetsBridge presets;
    presets.setController(&rig.controller);

    QVariantList lead = presets.leadingGroups({});
    const QVariantMap builtIn = groupNamed(lead, QStringLiteral("Presets"));
    CHECK(itemNames(builtIn, QStringLiteral("Twirls")) == QStringList{QStringLiteral("Twirl In"), QStringLiteral("Twirl Out")});
    CHECK(itemNames(builtIn, QStringLiteral("PiPs/25%")).contains(QStringLiteral("PiP 25% UL")));
    CHECK(itemNames(groupNamed(lead, QStringLiteral("Lumetri Presets")), QStringLiteral("Filmstock")).contains(QStringLiteral("Bleach Bypass")));
    CHECK(groupNamed(lead, QStringLiteral("Favorites")).isEmpty());
    // A search keeps matching presets only.
    const QVariantList twirl = presets.leadingGroups(QStringLiteral("twirl"));
    REQUIRE(twirl.size() == 1);
    CHECK(itemNames(twirl[0].toMap(), QStringLiteral("Twirls")).size() == 2);

    // Favourites and custom bins (effects, transitions and presets).
    CHECK(presets.setFavorite(QStringLiteral("preset:Twirl In"), true).isEmpty());
    CHECK(presets.setFavorite(QStringLiteral("crossDissolve"), true).isEmpty());
    CHECK(presets.isFavorite(QStringLiteral("crossDissolve")));
    lead = presets.leadingGroups({});
    CHECK(lead[0].toMap().value(QStringLiteral("title")).toString() == QStringLiteral("Favorites"));
    CHECK(itemNames(lead[0].toMap(), QString()) == QStringList{QStringLiteral("Twirl In"), QStringLiteral("Cross Dissolve")});
    CHECK(presets.newCustomBin(QStringLiteral("wabcut.blur.gaussian")) == QStringLiteral("Custom Bin 01"));
    CHECK(presets.addToCustomBin(QStringLiteral("Custom Bin 01"), QStringLiteral("preset:Mosaic In")).isEmpty());
    CHECK(presets.renameCustomBin(QStringLiteral("Custom Bin 01"), QStringLiteral("Mine")).isEmpty());
    const QVariantList custom = presets.customGroups({});
    REQUIRE(custom.size() == 1);
    CHECK(custom[0].toMap().value(QStringLiteral("customBin")).toString() == QStringLiteral("Mine"));
    CHECK(itemNames(custom[0].toMap(), QString()) == QStringList{QStringLiteral("Gaussian Blur"), QStringLiteral("Mosaic In")});
    // The keymap's Delete Custom Item takes the current item out of its custom bin.
    presets.setCurrentItem(QStringLiteral("preset:Mosaic In"));
    presets.setCurrentBin(QStringLiteral("Mine"));
    CHECK(presets.deleteCurrentItem().isEmpty());
    CHECK(itemNames(presets.customGroups({})[0].toMap(), QString()) == QStringList{QStringLiteral("Gaussian Blur")});
    CHECK(presets.revision() > 0);
}

TEST_CASE("Presets drop on clips, apply from Effect Controls and save from it", "[app][effects][presets]") {
    sharedEffectLibrary()->set({});
    Rig rig(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa..Bbbbbbbbbbbbbbbbbbbb|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa..Bbbbbbbbbbbbbbbbbbbb|
    )");
    EffectPresetsBridge presets;
    presets.setController(&rig.controller);
    TimelineView& view = *rig.view;
    ProjectController& controller = rig.controller;
    const ClipId video = rig.clip("V1@0");

    // A video preset dropped on the audio half of a linked clip applies to its video half.
    const QVariantMap target = view.effectDropTarget(rig.x(10), rig.y("A1"), QStringLiteral("preset:Twirl In"));
    CHECK(target.value(QStringLiteral("ok")).toBool());
    CHECK(target.value(QStringLiteral("message")).toString() == QStringLiteral("Apply Twirl In"));
    CHECK_FALSE(view.effectDropTarget(rig.x(10), rig.y("A1"), QStringLiteral("preset:Nothing")).value(QStringLiteral("ok")).toBool());
    const std::size_t before = controller.session().undoCount();
    CHECK(view.dropEffect(rig.x(10), rig.y("A1"), QStringLiteral("preset:Twirl In")).isEmpty());
    CHECK(controller.session().undoCount() == before + 1);
    const EffectStack& stack = controller.currentSequence()->findClip(video)->clip().effects();
    REQUIRE(stack.size() == 1);
    CHECK(stack[0].effect == "wabcut.distort.twirl");

    // Save Preset from Effect Controls: the twirl's section of the selected clip.
    controller.selectClipById(video, ProjectController::Replace);
    EffectControlsModel& fx = *controller.effectControls();
    const QString section = QStringLiteral("e") + QString::number(stack[0].id.value);
    CHECK(presets.sectionsOfClip(section).contains(QStringLiteral("motion")));
    CHECK(presets.sectionTitle(section) == QStringLiteral("Twirl"));
    CHECK(presets.suggestName(QStringLiteral("Twirl In")) == QStringLiteral("Twirl In 2"));
    CHECK(presets.savePreset({section, QStringLiteral("motion")}, QStringLiteral("My twirl"), QStringLiteral("anchorToInPoint"),
                             QStringLiteral("Mine"), QStringLiteral("Mine")).isEmpty());
    CHECK(itemNames(groupNamed(presets.leadingGroups({}), QStringLiteral("Presets")), QStringLiteral("Mine")) ==
          QStringList{QStringLiteral("My twirl")});
    CHECK(presets.preset(QStringLiteral("My twirl")).value(QStringLiteral("typeName")).toString() == QStringLiteral("Anchor to In Point"));

    // Applied through Effect Controls (a drop there, or Enter in the Effects panel) to the other clip.
    controller.selectClipById(rig.clip("V1@42"), ProjectController::Replace);
    CHECK(fx.addEffect(QStringLiteral("preset:My twirl")).isEmpty());
    CHECK(controller.currentSequence()->findClip(rig.clip("V1@42"))->clip().effects().size() == 1);
    CHECK(presets.applyToSelection(QStringLiteral("Mosaic Out")).isEmpty());
    CHECK(controller.currentSequence()->findClip(rig.clip("V1@42"))->clip().effects().size() == 2);

    // Preset Properties and Delete.
    CHECK(presets.setPresetProperties(QStringLiteral("My twirl"), QStringLiteral("Spin"), QStringLiteral("scale"), QString()).isEmpty());
    CHECK(presets.preset(QStringLiteral("Spin")).value(QStringLiteral("type")).toString() == QStringLiteral("scale"));
    CHECK(presets.deleteCustomItem(QVariantMap{{QStringLiteral("preset"), QStringLiteral("Spin")}}).isEmpty());
    CHECK(presets.preset(QStringLiteral("Spin")).isEmpty());
}

// ---- UX rework ----------------------------------------------------------------------------------------------------------

TEST_CASE("Effects panel: stable folder keys and counts in the groups and bins", "[app][effects][presets]") {
    sharedEffectLibrary()->set({});
    Rig rig(kTwoClips);
    EffectPresetsBridge presets;
    presets.setController(&rig.controller);

    // Presets and Lumetri Presets: untranslated keys, the bins' paths and counts.
    QVariantList lead = presets.leadingGroups({});
    const QVariantMap builtIn = groupNamed(lead, QStringLiteral("Presets"));
    CHECK(builtIn.value(QStringLiteral("key")).toString() == QStringLiteral("presets"));
    const QVariantMap pips = binNamed(builtIn, QStringLiteral("PiPs/25%"));
    CHECK(pips.value(QStringLiteral("key")).toString() == QStringLiteral("presets/b:PiPs/25%"));
    CHECK(pips.value(QStringLiteral("path")).toString() == QStringLiteral("PiPs/25%"));
    CHECK(pips.value(QStringLiteral("builtIn")).toBool());
    CHECK(pips.value(QStringLiteral("count")).toInt() == pips.value(QStringLiteral("items")).toList().size());
    int sum = 0;
    for (const QVariant& b : builtIn.value(QStringLiteral("bins")).toList())
        sum += b.toMap().value(QStringLiteral("count")).toInt();
    CHECK(builtIn.value(QStringLiteral("count")).toInt() == sum);
    const QVariantMap lumetri = groupNamed(lead, QStringLiteral("Lumetri Presets"));
    CHECK(lumetri.value(QStringLiteral("key")).toString() == QStringLiteral("lumetriPresets"));
    CHECK(binNamed(lumetri, QStringLiteral("Filmstock")).value(QStringLiteral("key")).toString() == QStringLiteral("lumetriPresets/b:Filmstock"));

    // The effect groups: the kind as key, "<kind>/<English category>" per bin (one bin, never split on "/").
    const QVariantList effects = rig.controller.effectControls()->library();
    for (const QVariant& g : effects)
        CHECK(g.toMap().value(QStringLiteral("key")).toString() == g.toMap().value(QStringLiteral("kind")).toString());
    const QVariantMap audio = groupOfKind(effects, QStringLiteral("audio"));
    const QVariantMap restoration = binNamed(audio, QStringLiteral("Noise Reduction/Restoration"));
    CHECK(restoration.value(QStringLiteral("key")).toString() == QStringLiteral("audio/Noise Reduction/Restoration"));
    CHECK(restoration.value(QStringLiteral("count")).toInt() >= 3);
    // The Essential Sound record (it does nothing to the sound) and Title Style are not listed.
    for (const QVariant& g : effects) {
        for (const QVariant& i : g.toMap().value(QStringLiteral("items")).toList()) {
            CHECK(i.toMap().value(QStringLiteral("id")).toString() != QStringLiteral("wabcut.audio.essentialSound"));
            CHECK(i.toMap().value(QStringLiteral("id")).toString() != QStringLiteral("wabcut.title.style"));
        }
    }
    const QVariantMap blurBin = binNamed(groupOfKind(effects, QStringLiteral("video")), QStringLiteral("Blur & Sharpen"));
    CHECK(blurBin.value(QStringLiteral("key")).toString() == QStringLiteral("video/Blur & Sharpen"));
    QString blurIcon;
    for (const QVariant& i : blurBin.value(QStringLiteral("items")).toList()) {
        if (i.toMap().value(QStringLiteral("id")).toString() == QStringLiteral("wabcut.blur.gaussian"))
            blurIcon = i.toMap().value(QStringLiteral("icon")).toString();
    }
    CHECK(blurIcon == QStringLiteral("droplet"));

    // Favorites: the same icon as in its own bin.
    CHECK(presets.setFavorite(QStringLiteral("wabcut.blur.gaussian"), true).isEmpty());
    lead = presets.leadingGroups({});
    const QVariantMap favorites = groupNamed(lead, QStringLiteral("Favorites"));
    CHECK(favorites.value(QStringLiteral("key")).toString() == QStringLiteral("favorites"));
    CHECK(favorites.value(QStringLiteral("count")).toInt() == 1);
    const QVariantList favoriteItems = binNamed(favorites, QString()).value(QStringLiteral("items")).toList();
    REQUIRE(favoriteItems.size() == 1);
    CHECK(favoriteItems[0].toMap().value(QStringLiteral("icon")).toString() == QStringLiteral("droplet"));
    CHECK(favoriteItems[0].toMap().value(QStringLiteral("plugin")).toBool() == false);

    // Custom bins and the user's presets bins.
    CHECK(presets.newCustomBin(QStringLiteral("crossDissolve")) == QStringLiteral("Custom Bin 01"));
    const QVariantList custom = presets.customGroups({});
    REQUIRE(custom.size() == 1);
    CHECK(custom[0].toMap().value(QStringLiteral("key")).toString() == QStringLiteral("custom:Custom Bin 01"));
    CHECK(presets.customBinKey(QStringLiteral("Custom Bin 01")) == QStringLiteral("custom:Custom Bin 01"));
    CHECK(custom[0].toMap().value(QStringLiteral("count")).toInt() == 1);
    CHECK(presets.newPresetsBin(QStringLiteral("Lower thirds/Blue")).isEmpty());
    const QVariantMap withBins = groupNamed(presets.leadingGroups({}), QStringLiteral("Presets"));
    const QVariantMap blue = binNamed(withBins, QStringLiteral("Lower thirds/Blue"));
    CHECK(blue.value(QStringLiteral("key")).toString() == QStringLiteral("presets/u:Lower thirds/Blue"));
    CHECK(blue.value(QStringLiteral("key")).toString() == presets.presetsBinKey(QStringLiteral("Lower thirds/Blue")));
    CHECK_FALSE(blue.value(QStringLiteral("builtIn")).toBool());
    CHECK(blue.value(QStringLiteral("presetsBin")).toString() == QStringLiteral("Lower thirds/Blue"));
    CHECK(presets.presetsBinKey(QString()) == QStringLiteral("presets"));
}

TEST_CASE("Effects panel: Import Presets into a bin and Export Presets of one bin", "[app][effects][presets]") {
    sharedEffectLibrary()->set({});
    Rig rig(kTwoClips);
    EffectPresetsBridge presets;
    presets.setController(&rig.controller);
    presets.setPersistent(false);
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    QList<QPair<QString, QString>> renamed;
    QObject::connect(&presets, &EffectPresetsBridge::binRenamed,
                     [&](const QString& from, const QString& to) { renamed.append({from, to}); });

    // Export Preset... of a built-in preset: a name without ".json" gets the presets suffix; the folder is remembered.
    CHECK(presets.userPresetCount() == 0);
    const QUrl typed = QUrl::fromLocalFile(dir.filePath(QStringLiteral("twirl")));
    CHECK(presets.exportPresets(typed, {QStringLiteral("Twirl In")}).isEmpty());
    const QString file = dir.filePath(QStringLiteral("twirl.wabcut-presets.json"));
    CHECK(QFileInfo::exists(file));
    CHECK(QFileInfo(presets.presetsFolder().toLocalFile()) == QFileInfo(dir.path()));
    // A name the bridge completed is never replaced silently; the dialog's own name is.
    CHECK_FALSE(presets.exportPresets(typed, {QStringLiteral("Twirl In")}).isEmpty());
    CHECK_FALSE(presets.lastError().isEmpty());
    CHECK(presets.exportPresets(QUrl::fromLocalFile(file), {QStringLiteral("Twirl In")}).isEmpty());
    CHECK(presets.lastError().isEmpty());
    // Every preset of the user's: none yet.
    CHECK_FALSE(presets.exportPresets(QUrl::fromLocalFile(dir.filePath(QStringLiteral("all.wabcut-presets.json"))), {}).isEmpty());

    // Import into a bin: every preset goes there; lastImported has the names they got.
    const QUrl url = QUrl::fromLocalFile(file);
    CHECK(presets.importPresets(url, QStringLiteral("Mine/Spins")).isEmpty());
    CHECK(presets.lastImported() == QStringList{QStringLiteral("Twirl In 2")});
    CHECK(presets.preset(QStringLiteral("Twirl In 2")).value(QStringLiteral("bin")).toString() == QStringLiteral("Mine/Spins"));
    CHECK(presets.userPresetCount() == 1);
    CHECK(presets.userPresetsIn(QStringLiteral("Mine")) == 1);
    CHECK(presets.userPresetsIn(QStringLiteral("mine/spins")) == 1);
    CHECK(presets.userPresetsIn(QStringLiteral("Other")) == 0);
    CHECK(presets.userPresetsIn(QString()) == 1);
    // No bin: the bin it was exported from. "": the Presets bin itself.
    CHECK(presets.importPresets(url).isEmpty());
    CHECK(presets.preset(QStringLiteral("Twirl In 3")).value(QStringLiteral("bin")).toString() == QStringLiteral("Twirls"));
    CHECK(presets.importPresets(url, QString()).isEmpty());
    CHECK(presets.preset(QStringLiteral("Twirl In 4")).value(QStringLiteral("bin")).toString().isEmpty());
    CHECK(presets.userPresetCount() == 3);

    // Export Presets... of one bin (and the bins inside it), under the bin's name in the remembered folder.
    const QUrl mine = presets.presetsFileUrl(QStringLiteral("Mine"));
    CHECK(mine.fileName() == QStringLiteral("Mine.wabcut-presets.json"));
    CHECK(QFileInfo(QFileInfo(mine.toLocalFile()).absolutePath()) == QFileInfo(dir.path()));
    CHECK(presets.exportPresetsBin(mine, QStringLiteral("Mine")).isEmpty());
    CHECK(presets.importPresets(mine, QStringLiteral("Copies")).isEmpty());
    CHECK(presets.lastImported().size() == 1);
    CHECK(presets.userPresetsIn(QStringLiteral("Copies")) == 1);
    CHECK_FALSE(presets.exportPresetsBin(QUrl::fromLocalFile(dir.filePath(QStringLiteral("none.wabcut-presets.json"))),
                                         QStringLiteral("Nothing")).isEmpty());
    CHECK_FALSE(presets.lastError().isEmpty());

    // A built-in look exported from Lumetri Presets comes back in a presets bin of its own, which can be renamed and
    // deleted (the Lumetri Presets bin holds the built-in looks only).
    const QUrl look = QUrl::fromLocalFile(dir.filePath(QStringLiteral("look.wabcut-presets.json")));
    CHECK(presets.exportPresets(look, {QStringLiteral("Bleach Bypass")}).isEmpty());
    CHECK(presets.importPresets(look).isEmpty());
    REQUIRE(presets.lastImported().size() == 1);
    const QString copy = presets.lastImported().front();
    CHECK(presets.preset(copy).value(QStringLiteral("bin")).toString() == QStringLiteral("Filmstock"));
    CHECK(presets.renamePresetsBin(QStringLiteral("Filmstock"), QStringLiteral("Looks")).isEmpty());
    REQUIRE(renamed.size() == 1);
    CHECK(renamed[0].first == QStringLiteral("presets/u:Filmstock"));
    CHECK(renamed[0].second == QStringLiteral("presets/u:Looks"));
    CHECK(presets.deleteCustomItem(QVariantMap{{QStringLiteral("presetsBin"), QStringLiteral("Looks")}}).isEmpty());
    CHECK(presets.preset(copy).isEmpty());

    // The dialogs' file names and the drop check.
    CHECK(presets.presetsFileName(QStringLiteral("Lower thirds/Blue")) == QStringLiteral("Blue.wabcut-presets.json"));
    CHECK(presets.presetsFileName(QStringLiteral("a:b?")) == QStringLiteral("a_b_.wabcut-presets.json"));
    CHECK(presets.presetsFileName(QString()) == QStringLiteral("Presets.wabcut-presets.json"));
    CHECK(presets.isPresetsFile(QUrl::fromLocalFile(QStringLiteral("C:/x/team.wabcut-presets.json"))));
    CHECK(presets.isPresetsFile(QUrl::fromLocalFile(QStringLiteral("C:/x/team.JSON"))));
    CHECK_FALSE(presets.isPresetsFile(QUrl::fromLocalFile(QStringLiteral("C:/x/clip.mp4"))));
}

TEST_CASE("Effects panel: deletes ask first and the keymap requests reach the panel", "[app][effects][presets]") {
    sharedEffectLibrary()->set({});
    Rig rig(kTwoClips);
    EffectPresetsBridge presets;
    presets.setController(&rig.controller);
    presets.setPersistent(false);
    QStringList created;
    QStringList messages;
    QVariantList requests;
    QList<QPair<QString, QString>> renamed;
    QObject::connect(&presets, &EffectPresetsBridge::customBinCreated, [&](const QString& name) { created << name; });
    QObject::connect(&presets, &EffectPresetsBridge::statusMessage, [&](const QString& text) { messages << text; });
    QObject::connect(&presets, &EffectPresetsBridge::deleteCurrentItemRequested, [&](const QVariantMap& info) { requests << info; });
    QObject::connect(&presets, &EffectPresetsBridge::binRenamed,
                     [&](const QString& from, const QString& to) { renamed.append({from, to}); });

    // New Custom Bin (the keymap's Ctrl+/ too): the panel hears of the new bin; a failure says why.
    CHECK(presets.newCustomBin() == QStringLiteral("Custom Bin 01"));
    CHECK(created == QStringList{QStringLiteral("Custom Bin 01")});
    CHECK(presets.lastError().isEmpty());
    CHECK(presets.newCustomBin(QStringLiteral("preset:No Such Preset")).isEmpty());
    CHECK_FALSE(presets.lastError().isEmpty());
    REQUIRE(messages.size() == 1);
    CHECK(messages[0] == presets.lastError());
    CHECK(presets.renameCustomBin(QStringLiteral("Custom Bin 01"), QStringLiteral("Mine")).isEmpty());
    REQUIRE(renamed.size() == 1);
    CHECK(renamed[0].first == QStringLiteral("custom:Custom Bin 01"));
    CHECK(renamed[0].second == QStringLiteral("custom:Mine"));

    // Nothing current: no request, a message.
    presets.requestDeleteCurrentItem();
    CHECK(requests.isEmpty());
    CHECK(messages.size() == 2);
    // A built-in preset cannot be deleted.
    presets.setCurrentItem(QStringLiteral("preset:Twirl In"));
    presets.setCurrentBin(QString());
    QVariantMap info = presets.currentItemInfo();
    CHECK_FALSE(info.value(QStringLiteral("deletable")).toBool());
    CHECK_FALSE(info.value(QStringLiteral("reason")).toString().isEmpty());
    CHECK_FALSE(presets.deleteCurrentItem().isEmpty());
    presets.requestDeleteCurrentItem();
    CHECK(requests.isEmpty());

    // An item of a custom bin: Remove from the bin, after the panel's confirmation.
    CHECK(presets.addToCustomBin(QStringLiteral("Mine"), QStringLiteral("crossDissolve")).isEmpty());
    presets.setCurrentItem(QStringLiteral("effect:crossDissolve"));
    presets.setCurrentBin(QStringLiteral("Mine"));
    info = presets.currentItemInfo();
    CHECK(info.value(QStringLiteral("deletable")).toBool());
    CHECK(info.value(QStringLiteral("action")).toString() == QStringLiteral("removeFromBin"));
    CHECK(info.value(QStringLiteral("name")).toString() == QStringLiteral("Cross Dissolve"));
    CHECK(info.value(QStringLiteral("question")).toString().contains(QStringLiteral("Cross Dissolve")));
    CHECK_FALSE(info.value(QStringLiteral("confirmText")).toString().isEmpty());
    presets.requestDeleteCurrentItem();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].toMap().value(QStringLiteral("action")).toString() == QStringLiteral("removeFromBin"));
    CHECK(itemNames(presets.customGroups({})[0].toMap(), QString()) == QStringList{QStringLiteral("Cross Dissolve")}); // asked only
    CHECK(presets.deleteCurrentItem().isEmpty());
    CHECK(itemNames(presets.customGroups({})[0].toMap(), QString()).isEmpty());
    CHECK(presets.currentItem().isEmpty());
    CHECK(presets.currentBin().isEmpty());

    // One of the user's presets: Delete.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QUrl file = QUrl::fromLocalFile(dir.filePath(QStringLiteral("twirl.wabcut-presets.json")));
    REQUIRE(presets.exportPresets(file, {QStringLiteral("Twirl In")}).isEmpty());
    REQUIRE(presets.importPresets(file).isEmpty());
    presets.setCurrentItem(QStringLiteral("preset:Twirl In 2"));
    info = presets.currentItemInfo();
    CHECK(info.value(QStringLiteral("action")).toString() == QStringLiteral("deletePreset"));
    CHECK(info.value(QStringLiteral("name")).toString() == QStringLiteral("Twirl In 2"));
    CHECK(presets.deleteCurrentItem().isEmpty());
    CHECK(presets.preset(QStringLiteral("Twirl In 2")).isEmpty());
}

TEST_CASE("Effects panel: an effect or a preset goes on every selected clip in one undo step", "[app][effects][presets]") {
    sharedEffectLibrary()->set({});
    Rig rig(kTwoClips);
    ProjectController& controller = rig.controller;
    EffectControlsModel& fx = *controller.effectControls();
    const ClipId a = rig.clip("V1@0");
    const ClipId b = rig.clip("V1@42");
    const ClipId aAudio = rig.clip("A1@0");
    const ClipId bAudio = rig.clip("A1@42");
    const auto stackOf = [&](ClipId clip) {
        std::vector<std::string> out;
        const auto seq = controller.currentSequence(); // held: the clip's stack lives in it
        for (const EffectInstance& e : seq->findClip(clip)->clip().effects())
            out.push_back(e.effect);
        return out;
    };

    // Nothing selected.
    controller.setSelection({});
    QVariantMap result = fx.applyToSelectedClips(QStringLiteral("wabcut.blur.gaussian"));
    CHECK_FALSE(result.value(QStringLiteral("ok")).toBool());
    CHECK_FALSE(result.value(QStringLiteral("error")).toString().isEmpty());
    CHECK(result.value(QStringLiteral("message")) == result.value(QStringLiteral("error")));

    // Both video clips: one instance each, one undo step.
    controller.setSelection({a, b});
    std::size_t before = controller.session().undoCount();
    result = fx.applyToSelectedClips(QStringLiteral("wabcut.blur.gaussian"));
    CHECK(result.value(QStringLiteral("ok")).toBool());
    CHECK(result.value(QStringLiteral("clips")).toInt() == 2);
    CHECK(result.value(QStringLiteral("message")).toString() == QStringLiteral("Added Gaussian Blur to 2 clips"));
    CHECK(controller.session().undoCount() == before + 1);
    CHECK(stackOf(a) == std::vector<std::string>{"wabcut.blur.gaussian"});
    CHECK(stackOf(b) == std::vector<std::string>{"wabcut.blur.gaussian"});
    CHECK(stackOf(aAudio).empty());

    // An audio effect with the video clips selected goes on their linked audio clips.
    before = controller.session().undoCount();
    result = fx.applyToSelectedClips(QStringLiteral("effect:wabcut.audio.gain"));
    CHECK(result.value(QStringLiteral("ok")).toBool());
    CHECK(result.value(QStringLiteral("clips")).toInt() == 2);
    CHECK(controller.session().undoCount() == before + 1);
    CHECK(stackOf(aAudio) == std::vector<std::string>{"wabcut.audio.gain"});
    CHECK(stackOf(bAudio) == std::vector<std::string>{"wabcut.audio.gain"});

    // A preset: on both, one step.
    before = controller.session().undoCount();
    result = fx.applyToSelectedClips(QStringLiteral("preset:Twirl In"));
    CHECK(result.value(QStringLiteral("ok")).toBool());
    CHECK(result.value(QStringLiteral("clips")).toInt() == 2);
    CHECK(controller.session().undoCount() == before + 1);
    CHECK(stackOf(a).size() == 2);
    CHECK(stackOf(b).size() == 2);

    // A transition: on the ends of each selected clip of its kind, one step.
    before = controller.session().undoCount();
    result = fx.applyToSelectedClips(QStringLiteral("crossDissolve"));
    CHECK(result.value(QStringLiteral("ok")).toBool());
    CHECK(result.value(QStringLiteral("clips")).toInt() == 2);
    CHECK(controller.session().undoCount() == before + 1);
    const auto withTransitions = controller.currentSequence();
    const auto& transitions = withTransitions->videoTracks[0]->transitions;
    CHECK_FALSE(transitions.empty());
    for (const Transition& t : transitions)
        CHECK(t.effect == "crossDissolve");
    CHECK(withTransitions->audioTracks[0]->transitions.empty());

    // Not an Effects panel item; a plug-in id is the plug-in bridge's.
    CHECK_FALSE(fx.applyToSelectedClips(QStringLiteral("wabcut.audio.essentialSound")).value(QStringLiteral("ok")).toBool());
    CHECK_FALSE(fx.applyToSelectedClips(QStringLiteral("nonsense")).value(QStringLiteral("ok")).toBool());
    CHECK_FALSE(fx.applyToSelectedClips(QStringLiteral("preset:No Such Preset")).value(QStringLiteral("ok")).toBool());

    // The tree's applyToSelection routes there too.
    EffectPresetsBridge presets;
    presets.setController(&controller);
    EffectsTreeModel tree;
    tree.setPersistent(false);
    tree.setSources(&controller, &presets, nullptr);
    controller.setSelection({b});
    result = tree.applyToSelection(QStringLiteral("effect:wabcut.blur.gaussian"));
    CHECK(result.value(QStringLiteral("ok")).toBool());
    CHECK(result.value(QStringLiteral("message")).toString() == QStringLiteral("Added Gaussian Blur"));
    CHECK(stackOf(b).size() == 3);
    CHECK_FALSE(tree.applyToSelection(QStringLiteral("plugin.clap.not.here")).value(QStringLiteral("ok")).toBool());
}

TEST_CASE("Effects tree: Premiere folders with nested presets bins updated in place", "[app][effects][presets]") {
    sharedEffectLibrary()->set({});
    Rig rig(kTwoClips);
    EffectPresetsBridge presets;
    presets.setController(&rig.controller);
    EffectsTreeModel tree;
    tree.setPersistent(false);
    tree.setSources(&rig.controller, &presets, nullptr);
    int resets = 0;
    QObject::connect(&tree, &QAbstractItemModel::modelReset, [&] { ++resets; });

    // At first every top-level folder is closed (Favorites would be open: there is none yet), in Premiere's order.
    CHECK(rowKeys(tree) == QStringList{QStringLiteral("presets"), QStringLiteral("lumetriPresets"), QStringLiteral("audio"),
                                       QStringLiteral("audioTransition"), QStringLiteral("video"), QStringLiteral("videoTransition")});
    CHECK(tree.rowCount() == 6);
    const QVariantMap presetsRow = tree.get(0);
    CHECK(presetsRow.value(QStringLiteral("rowType")).toString() == QStringLiteral("folder"));
    CHECK(presetsRow.value(QStringLiteral("title")).toString() == QStringLiteral("Presets"));
    CHECK(presetsRow.value(QStringLiteral("folderKind")).toString() == QStringLiteral("presets"));
    CHECK(presetsRow.value(QStringLiteral("depth")).toInt() == 0);
    CHECK_FALSE(presetsRow.value(QStringLiteral("expanded")).toBool());
    CHECK(presetsRow.value(QStringLiteral("canHoldPresets")).toBool());
    CHECK(presetsRow.value(QStringLiteral("count")).toInt() > 10);
    CHECK(tree.roleNames().value(EffectsTreeModel::TilesRole) == QByteArray("tiles"));

    // Presets bins split on "/": PiPs > 25%.
    tree.setExpanded(QStringLiteral("presets"), true);
    CHECK(tree.isExpanded(QStringLiteral("presets")));
    CHECK(tree.indexOf(QStringLiteral("presets/b:PiPs")) > 0);
    CHECK(tree.indexOf(QStringLiteral("presets/b:PiPs/25%")) < 0);
    CHECK(tree.reveal(QStringLiteral("presets/b:PiPs/25%")) == tree.indexOf(QStringLiteral("presets/b:PiPs")) + 1);
    CHECK(roleOf(tree, QStringLiteral("presets/b:PiPs/25%"), "title").toString() == QStringLiteral("25%"));
    CHECK(roleOf(tree, QStringLiteral("presets/b:PiPs/25%"), "depth").toInt() == 2);
    CHECK(roleOf(tree, QStringLiteral("presets/b:PiPs/25%"), "parentKey").toString() == QStringLiteral("presets/b:PiPs"));
    CHECK_FALSE(roleOf(tree, QStringLiteral("presets/b:PiPs/25%"), "renamable").toBool());
    CHECK(tree.parentIndex(tree.indexOf(QStringLiteral("presets/b:PiPs/25%"))) == tree.indexOf(QStringLiteral("presets/b:PiPs")));

    // Effect categories: one folder each, never split.
    tree.setExpanded(QStringLiteral("audio"), true);
    CHECK(roleOf(tree, QStringLiteral("audio/Noise Reduction/Restoration"), "title").toString() == QStringLiteral("Noise Reduction/Restoration"));
    CHECK(roleOf(tree, QStringLiteral("audio/Noise Reduction/Restoration"), "depth").toInt() == 1);
    CHECK(roleOf(tree, QStringLiteral("audio/Noise Reduction/Restoration"), "folderKind").toString() == QStringLiteral("category"));

    // Items: in their folder, with what the panel needs.
    tree.setExpanded(QStringLiteral("video"), true);
    tree.toggle(QStringLiteral("video/Blur & Sharpen"));
    const QString blurKey = QStringLiteral("video/Blur & Sharpen|wabcut.blur.gaussian");
    const int blurRow = tree.indexOf(blurKey);
    REQUIRE(blurRow > 0);
    const QVariantMap blur = tree.get(blurRow);
    CHECK(blur.value(QStringLiteral("rowType")).toString() == QStringLiteral("item"));
    CHECK(blur.value(QStringLiteral("itemId")).toString() == QStringLiteral("wabcut.blur.gaussian"));
    CHECK(blur.value(QStringLiteral("item")).toString() == QStringLiteral("effect:wabcut.blur.gaussian"));
    CHECK(blur.value(QStringLiteral("name")).toString() == QStringLiteral("Gaussian Blur"));
    CHECK(blur.value(QStringLiteral("kind")).toString() == QStringLiteral("video"));
    CHECK(blur.value(QStringLiteral("icon")).toString() == QStringLiteral("droplet"));
    CHECK(blur.value(QStringLiteral("home")).toBool());
    CHECK_FALSE(blur.value(QStringLiteral("favorite")).toBool());
    CHECK(blur.value(QStringLiteral("depth")).toInt() == 2);
    CHECK(tree.parentIndex(blurRow) == tree.indexOf(QStringLiteral("video/Blur & Sharpen")));
    CHECK(tree.indexOfItem(QStringLiteral("wabcut.blur.gaussian")) == blurRow);

    // Favorites: first, open, and the item marked everywhere; rows change in place.
    CHECK(presets.setFavorite(QStringLiteral("wabcut.blur.gaussian"), true).isEmpty());
    CHECK(rowKeys(tree).value(0) == QStringLiteral("favorites"));
    CHECK(tree.isExpanded(QStringLiteral("favorites")));
    CHECK(rowKeys(tree).value(1) == QStringLiteral("favorites|wabcut.blur.gaussian"));
    CHECK_FALSE(tree.get(1).value(QStringLiteral("home")).toBool());
    CHECK(tree.get(1).value(QStringLiteral("favorite")).toBool());
    CHECK(tree.get(1).value(QStringLiteral("icon")).toString() == QStringLiteral("droplet"));
    CHECK(roleOf(tree, blurKey, "favorite").toBool());
    CHECK(tree.indexOfItem(QStringLiteral("wabcut.blur.gaussian")) == tree.indexOf(blurKey)); // its own place first

    // Custom bins come last; their items are removable copies; a rename keeps the folder open.
    CHECK(presets.newCustomBin(QStringLiteral("preset:Twirl In")) == QStringLiteral("Custom Bin 01"));
    CHECK(rowKeys(tree).last() == QStringLiteral("custom:Custom Bin 01"));
    const QVariantMap binRow = tree.get(tree.indexOf(QStringLiteral("custom:Custom Bin 01")));
    CHECK(binRow.value(QStringLiteral("folderKind")).toString() == QStringLiteral("custom"));
    CHECK(binRow.value(QStringLiteral("customBin")).toString() == QStringLiteral("Custom Bin 01"));
    CHECK(binRow.value(QStringLiteral("canHoldItems")).toBool());
    CHECK(binRow.value(QStringLiteral("renamable")).toBool());
    CHECK(binRow.value(QStringLiteral("deletable")).toBool());
    CHECK(binRow.value(QStringLiteral("count")).toInt() == 1);
    tree.setExpanded(QStringLiteral("custom:Custom Bin 01"), true);
    CHECK(rowKeys(tree).last() == QStringLiteral("custom:Custom Bin 01|preset:Twirl In"));
    const QVariantMap copy = tree.get(tree.rowCount() - 1);
    CHECK(copy.value(QStringLiteral("customBin")).toString() == QStringLiteral("Custom Bin 01"));
    CHECK(copy.value(QStringLiteral("removable")).toBool());
    CHECK_FALSE(copy.value(QStringLiteral("home")).toBool());
    CHECK_FALSE(copy.value(QStringLiteral("deletable")).toBool()); // a built-in preset
    CHECK(copy.value(QStringLiteral("kind")).toString() == QStringLiteral("preset"));
    CHECK(presets.renameCustomBin(QStringLiteral("Custom Bin 01"), QStringLiteral("Gaming")).isEmpty());
    CHECK(tree.isExpanded(QStringLiteral("custom:Gaming")));
    CHECK(tree.indexOf(QStringLiteral("custom:Gaming|preset:Twirl In")) > 0);

    // The user's presets bins nest too and take presets; a rename keeps them open.
    CHECK(presets.newPresetsBin(QStringLiteral("Lower thirds/Blue")).isEmpty());
    const QString lower = QStringLiteral("presets/u:Lower thirds");
    REQUIRE(tree.indexOf(lower) > 0);
    CHECK(roleOf(tree, lower, "folderKind").toString() == QStringLiteral("presetsBin"));
    CHECK(roleOf(tree, lower, "presetsBin").toString() == QStringLiteral("Lower thirds"));
    CHECK(roleOf(tree, lower, "canHoldPresets").toBool());
    CHECK(roleOf(tree, lower, "renamable").toBool());
    CHECK(roleOf(tree, lower, "deletable").toBool());
    CHECK(roleOf(tree, lower, "userPresets").toInt() == 0);
    tree.setExpanded(lower, true);
    CHECK(roleOf(tree, QStringLiteral("presets/u:Lower thirds/Blue"), "presetsBin").toString() == QStringLiteral("Lower thirds/Blue"));
    CHECK(roleOf(tree, QStringLiteral("presets/u:Lower thirds/Blue"), "depth").toInt() == 2);
    CHECK(presets.renamePresetsBin(QStringLiteral("Lower thirds"), QStringLiteral("Titles")).isEmpty());
    CHECK(tree.isExpanded(QStringLiteral("presets/u:Titles")));
    CHECK(tree.indexOf(QStringLiteral("presets/u:Titles/Blue")) > 0);
    CHECK(tree.indexOf(lower) < 0);

    // Expand All / Collapse All of one folder, then of everything.
    tree.collapseAll(QStringLiteral("presets"));
    CHECK_FALSE(tree.isExpanded(QStringLiteral("presets")));
    CHECK_FALSE(tree.isExpanded(QStringLiteral("presets/u:Titles")));
    tree.expandAll(QStringLiteral("presets"));
    CHECK(tree.indexOf(QStringLiteral("presets/b:PiPs/40%")) > 0);
    tree.collapseAll();
    CHECK(tree.rowCount() == static_cast<int>(topKeys(tree).size()));
    CHECK(resets == 0);
}

TEST_CASE("Effects tree: a search opens every folder with a match and clearing it restores the user state",
          "[app][effects][presets]") {
    sharedEffectLibrary()->set({});
    Rig rig(kTwoClips);
    EffectPresetsBridge presets;
    presets.setController(&rig.controller);
    EffectsTreeModel tree;
    tree.setPersistent(false);
    tree.setSources(&rig.controller, &presets, nullptr);
    tree.setExpanded(QStringLiteral("video"), true);

    tree.setQuery(QStringLiteral("twirl"));
    CHECK(tree.searching());
    CHECK(tree.isExpanded(QStringLiteral("presets")));
    CHECK(tree.isExpanded(QStringLiteral("presets/b:Twirls")));
    CHECK(tree.indexOf(QStringLiteral("presets/b:Twirls|preset:Twirl In")) > 0);
    CHECK(tree.indexOf(QStringLiteral("video/Distort|wabcut.distort.twirl")) > 0);
    CHECK(tree.indexOf(QStringLiteral("audioTransition")) < 0); // nothing matches there: hidden
    // The match count: each item once, every folder with a match open.
    QSet<QString> shown;
    for (int i = 0; i < tree.rowCount(); ++i) {
        if (tree.get(i).value(QStringLiteral("rowType")).toString() == QStringLiteral("item"))
            shown.insert(tree.get(i).value(QStringLiteral("itemId")).toString());
    }
    CHECK(tree.matchCount() >= 3);
    CHECK(tree.matchCount() == shown.size());

    // Closing a folder during a search lasts until the query changes.
    tree.setExpanded(QStringLiteral("presets"), false);
    CHECK(tree.indexOf(QStringLiteral("presets/b:Twirls")) < 0);
    tree.setQuery(QStringLiteral("twirl i"));
    CHECK(tree.isExpanded(QStringLiteral("presets")));

    // Clearing it: the user's own state again (only Video Effects open).
    tree.setQuery({});
    CHECK_FALSE(tree.searching());
    CHECK_FALSE(tree.isExpanded(QStringLiteral("presets")));
    CHECK(tree.isExpanded(QStringLiteral("video")));
    CHECK_FALSE(tree.isExpanded(QStringLiteral("video/Distort")));
    CHECK(tree.indexOf(QStringLiteral("presets/b:Twirls")) < 0);
    CHECK(tree.indexOf(QStringLiteral("video/Distort")) > 0);

    // revealItem opens the folders around an item's own place (a preset just imported, a new effect...).
    const int row = tree.revealItem(QStringLiteral("preset:Twirl Out"));
    CHECK(row == tree.indexOf(QStringLiteral("presets/b:Twirls|preset:Twirl Out")));
    CHECK(row > 0);
    CHECK(tree.isExpanded(QStringLiteral("presets/b:Twirls")));
    CHECK(tree.revealItem(QStringLiteral("no.such.effect")) == -1);
}

TEST_CASE("Effects tree: filters and the Icons view tiles and the expanded state kept for the next start",
          "[app][effects][presets]") {
    QSettings().remove(QStringLiteral("effects"));
    sharedEffectLibrary()->set({});
    Rig rig(kTwoClips);
    EffectPresetsBridge presets;
    presets.setController(&rig.controller);
    {
        EffectsTreeModel tree;
        tree.setSources(&rig.controller, &presets, nullptr);
        // The first start: Favorites (when there are some) open, the rest closed.
        CHECK(tree.isExpanded(QStringLiteral("favorites")));
        CHECK_FALSE(tree.isExpanded(QStringLiteral("video")));
        tree.setExpanded(QStringLiteral("video"), true);
        tree.setExpanded(QStringLiteral("video/Blur & Sharpen"), true);
        tree.setExpanded(QStringLiteral("audio"), true); // the top-level folder opened last
        tree.setExpanded(QStringLiteral("favorites"), false);
    }
    EffectsTreeModel tree;
    tree.setSources(&rig.controller, &presets, nullptr);
    CHECK(tree.isExpanded(QStringLiteral("audio")));
    CHECK_FALSE(tree.isExpanded(QStringLiteral("video")));
    CHECK(tree.isExpanded(QStringLiteral("video/Blur & Sharpen")));
    CHECK_FALSE(tree.isExpanded(QStringLiteral("favorites")));

    // Filters: kinds of items in every folder, or one folder.
    tree.setFilter(QStringLiteral("transitions"));
    CHECK(topKeys(tree) == QStringList{QStringLiteral("audioTransition"), QStringLiteral("videoTransition")});
    tree.setFilter(QStringLiteral("presets"));
    CHECK(topKeys(tree) == QStringList{QStringLiteral("presets"), QStringLiteral("lumetriPresets")});
    tree.setFilter(QStringLiteral("video"));
    CHECK(topKeys(tree).contains(QStringLiteral("video")));
    CHECK(topKeys(tree).contains(QStringLiteral("videoTransition")));
    CHECK(topKeys(tree).contains(QStringLiteral("presets"))); // video presets (Twirl In)
    CHECK_FALSE(topKeys(tree).contains(QStringLiteral("audio")));
    CHECK_FALSE(topKeys(tree).contains(QStringLiteral("audioTransition")));
    tree.setFilter(QStringLiteral("favorites"));
    CHECK(tree.rowCount() == 0); // no favourites yet
    tree.setFilter(QStringLiteral("nonsense"));
    CHECK(tree.filter() == QStringLiteral("all"));
    CHECK(topKeys(tree).size() == 6);

    // The Icons view: a folder's items in tiles rows of up to `columns`.
    tree.setExpanded(QStringLiteral("video"), true);
    tree.setColumns(4);
    const int first = tree.indexOf(QStringLiteral("video/Blur & Sharpen#0"));
    REQUIRE(first > 0);
    const QVariantMap tiles = tree.get(first);
    CHECK(tiles.value(QStringLiteral("rowType")).toString() == QStringLiteral("tiles"));
    CHECK(tiles.value(QStringLiteral("depth")).toInt() == 2);
    const QVariantList items = tiles.value(QStringLiteral("tiles")).toList();
    CHECK_FALSE(items.isEmpty());
    CHECK(items.size() <= 4);
    CHECK(items.front().toMap().value(QStringLiteral("rowType")).toString() == QStringLiteral("item"));
    CHECK(tree.indexOf(QStringLiteral("video/Blur & Sharpen|wabcut.blur.gaussian")) >= first); // its tiles row
    CHECK(tree.indexOfItem(QStringLiteral("wabcut.blur.gaussian")) >= first);
    tree.setColumns(0);
    CHECK(tree.columns() == 1);
    CHECK(tree.indexOf(QStringLiteral("video/Blur & Sharpen#0")) < 0);
    CHECK(tree.indexOf(QStringLiteral("video/Blur & Sharpen|wabcut.blur.gaussian")) >= first);
}

/// A bridge call's outcome ("" or its error) as text, so a failing REQUIRE shows the error.
static std::string outcome(const QString& error) { return error.toStdString(); }

// ---- The panel in the shell (EffectsPanel.qml) --------------------------------------------------------------------------

namespace {

QQuickItem* named(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = named(child, name))
            return found;
    }
    return nullptr;
}

void sendKey(QQuickWindow* w, int key, Qt::KeyboardModifiers mods = Qt::NoModifier, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, key, mods, text);
    QCoreApplication::sendEvent(w, &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods, text);
    QCoreApplication::sendEvent(w, &release);
    spin(20);
}

/// A click at `local` in `item` (a pause first, so two clicks on one row are never a double-click). Through the window
/// system, as a real click: a QMouseEvent sent straight to the window has no press position, and a DragHandler then
/// takes the press for a drag already past its threshold.
void clickAt(QQuickWindow* w, QQuickItem* item, QPointF local, Qt::MouseButton button = Qt::LeftButton) {
    spin(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 30);
    const QPointF p = item->mapToScene(local);
    QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
        w, p, w->mapToGlobal(p), Qt::NoButton, Qt::NoButton, QEvent::MouseMove);
    QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
        w, p, w->mapToGlobal(p), button, button, QEvent::MouseButtonPress);
    QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
        w, p, w->mapToGlobal(p), Qt::NoButton, button, QEvent::MouseButtonRelease);
    // Away again, so no tooltip comes up in a later screenshot.
    const QPointF away(1, 1);
    QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
        w, away, w->mapToGlobal(away), Qt::NoButton, Qt::NoButton, QEvent::MouseMove);
    spin(30);
}

/// Drags `mime` onto `item` (at `local`) and drops it there; whether the drop target took the drag.
bool dropOn(QQuickWindow* w, QQuickItem* item, QPointF local, QMimeData* mime) {
    const QPoint at = item->mapToScene(local).toPoint();
    QDragEnterEvent enter(at, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &enter);
    QDragMoveEvent move(at, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &move);
    const bool accepted = move.isAccepted();
    QDropEvent drop(QPointF(at), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &drop);
    spin(30);
    return accepted;
}

/// The Effects panel in the browser slot of the shell, with its tree, list and helpers.
struct EffectsUi {
    Shell& shell;
    QQuickWindow* w = nullptr;
    QQuickItem* panel = nullptr;
    QQuickItem* list = nullptr;
    EffectsTreeModel* tree = nullptr;
    EffectPresetsBridge* presets = nullptr;
    ProjectController* controller = nullptr;

    explicit EffectsUi(Shell& s) : shell(s), w(s.window.get()) {
        presets = shell.engine.singletonInstance<EffectPresetsBridge*>("Wabcut", "EffectPresets");
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(presets);
        REQUIRE(controller);
    }
    void show() {
        REQUIRE(QMetaObject::invokeMethod(w, "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("effects"))));
        REQUIRE(waitFor([&] {
            panel = shell.item("effectsPanel");
            return panel && panel->isVisible() && panel->width() > 0;
        }));
        spin(100);
        list = named(panel, QStringLiteral("effectsList"));
        tree = panel->findChild<EffectsTreeModel*>(QStringLiteral("effectsTree"));
        REQUIRE(list);
        REQUIRE(tree);
    }
    /// A row of the list by its objectName. A window that is never shown is not polished: lay the list out first.
    QQuickItem* row(const QString& objectName) const {
        QMetaObject::invokeMethod(list, "forceLayout");
        return named(list, objectName);
    }
    QQuickItem* folder(const QString& key) const { return row(QStringLiteral("effectFolder_") + key); }
    /// Scrolls row `key` to the top of the list (clicks and drops land on what shows; a TapHandler also wants the point
    /// inside the window's screen geometry, which a window that is never shown keeps at its first size) and returns its
    /// delegate: the live one of that index (a delegate the view let go may linger, by its objectName, until deleted).
    QQuickItem* reveal(const QString& key) const {
        const int index = tree->indexOf(key);
        REQUIRE(index >= 0);
        REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, index), Q_ARG(int, 0))); // ListView.Beginning
        QMetaObject::invokeMethod(list, "forceLayout");
        spin(20);
        QQuickItem* item = nullptr;
        REQUIRE(QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, index)));
        return item;
    }
    std::string currentKey() const { return panel->property("currentKey").toString().toStdString(); }
    std::string status() const { return panel->property("status").toString().toStdString(); }
    void select(const QString& key) const {
        REQUIRE(QMetaObject::invokeMethod(panel, "select", Q_ARG(QVariant, key)));
        spin(30);
    }
    /// Clicks a folder's row (on its name: the click opens or closes it).
    void clickFolder(const QString& key) const {
        QQuickItem* f = reveal(key);
        REQUIRE(f);
        clickAt(w, f, QPointF(f->width() / 2, headerMiddle(f)));
    }
    static double headerMiddle(QQuickItem* f) { return std::min(f->height(), 28.0) / 2; }
    QObject* menuItem(const char* objectName) const { return panel->findChild<QObject*>(QLatin1String(objectName)); }
    /// Opens the menu of row `key` and clicks `item`; waits until the menu has closed (its action then runs).
    void runMenu(const QString& key, const char* item) const {
        QQuickItem* target = list;
        REQUIRE(QMetaObject::invokeMethod(panel, "openMenu", Q_ARG(QVariant, key), Q_ARG(QVariant, QVariant::fromValue(target)),
                                          Q_ARG(QVariant, 20.0), Q_ARG(QVariant, 10.0)));
        QObject* entry = menuItem(item);
        REQUIRE(entry);
        QObject* menu = entry->parent();
        while (menu && !menu->inherits("QQuickMenu"))
            menu = menu->parent();
        REQUIRE(menu);
        REQUIRE(waitFor([&] { return menu->property("opened").toBool(); }, 3000));
        CHECK(entry->property("visible").toBool());
        REQUIRE(entry->property("enabled").toBool());
        REQUIRE(QMetaObject::invokeMethod(entry, "click"));
        REQUIRE(waitFor([&] { return !menu->property("visible").toBool(); }, 3000));
        spin(60);
    }
    /// Whether `local` in `item` lies in the list's visible area (a click or a drop there reaches the row).
    bool inView(QQuickItem* item, QPointF local) const {
        const QPointF p = item->mapToScene(local);
        const QRectF view = list->mapRectToScene(QRectF(0, 0, list->width(), list->height()));
        INFO("point " << p.x() << "," << p.y() << " list " << view.x() << "," << view.y() << " " << view.width() << "x"
                      << view.height());
        CHECK(view.contains(p));
        return view.contains(p);
    }
    void saveCrop(const QString& file) const {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        const double dpr = w->effectiveDevicePixelRatio();
        const QRectF r = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
        REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(outputDir() + QLatin1Char('/') + file));
    }
};

} // namespace

TEST_CASE("Effects panel UI: Premiere folders in one list and the List Compact and Icons views", "[app][effects][qml][screenshot]") {
    resetSettings();
    clearMessages();
    sharedEffectLibrary()->set({});
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    EffectsUi ui(shell);
    // Favourites and a custom bin, made before the panel shows (a bin made from the panel is named in place).
    REQUIRE(outcome(ui.presets->setFavorite(QStringLiteral("wabcut.blur.gaussian"), true)) == "");
    REQUIRE(outcome(ui.presets->setFavorite(QStringLiteral("crossDissolve"), true)) == "");
    REQUIRE(ui.presets->newCustomBin(QStringLiteral("preset:Twirl In")) == QStringLiteral("Custom Bin 01"));
    REQUIRE(outcome(ui.presets->renameCustomBin(QStringLiteral("Custom Bin 01"), QStringLiteral("Gaming"))) == "");
    REQUIRE(outcome(ui.presets->addToCustomBin(QStringLiteral("Gaming"), QStringLiteral("wabcut.audio.gain"))) == "");
    ui.show();

    // Premiere's folders in Premiere's order, one list row each; Favorites open, the rest closed (the first start).
    const QStringList top = topKeys(*ui.tree);
    REQUIRE(top.size() >= 8);
    CHECK(top.mid(0, 7) == QStringList{QStringLiteral("favorites"), QStringLiteral("presets"), QStringLiteral("lumetriPresets"),
                                       QStringLiteral("audio"), QStringLiteral("audioTransition"), QStringLiteral("video"),
                                       QStringLiteral("videoTransition")});
    CHECK(top.last() == QStringLiteral("custom:Gaming"));
    CHECK(ui.tree->isExpanded(QStringLiteral("favorites")));
    CHECK_FALSE(ui.tree->isExpanded(QStringLiteral("video")));
    for (const QString& key : top)
        CHECK(ui.folder(key) != nullptr);
    // Favorites' copies say where they are; their own rows keep the plain names.
    CHECK(ui.row(QStringLiteral("effectItem_favorites_wabcut.blur.gaussian")) != nullptr);

    // A click opens a folder (and its open state is kept for the next start); items show in it.
    ui.clickFolder(QStringLiteral("video"));
    CHECK(ui.tree->isExpanded(QStringLiteral("video")));
    CHECK(QSettings().value(QStringLiteral("effects/lastOpenedFolder")).toString() == QStringLiteral("video"));
    ui.clickFolder(QStringLiteral("video/Blur & Sharpen"));
    ui.clickFolder(QStringLiteral("custom:Gaming"));
    ui.reveal(QStringLiteral("custom:Gaming|preset:Twirl In"));
    CHECK(ui.row(QStringLiteral("effectItem_custom:Gaming_preset:Twirl In")) != nullptr);
    QQuickItem* blur = ui.reveal(QStringLiteral("video/Blur & Sharpen|wabcut.blur.gaussian"));
    REQUIRE(blur);
    CHECK(blur->objectName() == QStringLiteral("effectItem_wabcut.blur.gaussian"));
    CHECK(blur->isVisible());
    REQUIRE(ui.inView(blur, QPointF(blur->width() / 2, blur->height() / 2)));
    clickAt(ui.w, blur, QPointF(blur->width() / 2, blur->height() / 2));
    CHECK(ui.currentKey() == "video/Blur & Sharpen|wabcut.blur.gaussian");
    CHECK(ui.presets->currentItem() == QStringLiteral("effect:wabcut.blur.gaussian"));
    const double listRow = blur->height();
    ui.select(QStringLiteral("favorites"));
    ui.saveCrop(QStringLiteral("effects-list.png"));

    // Compact: denser rows. The view is kept in Preferences (effects/view).
    QQuickItem* compact = named(ui.panel, QStringLiteral("effectsView_compact"));
    REQUIRE(compact);
    clickAt(ui.w, compact, QPointF(compact->width() / 2, compact->height() / 2));
    CHECK(shell.preferences->property("effectsView").toString() == QStringLiteral("compact"));
    CHECK(waitFor([] { return QSettings().value(QStringLiteral("effects/view")).toString() == QStringLiteral("compact"); }, 3000));
    blur = ui.row(QStringLiteral("effectItem_wabcut.blur.gaussian"));
    REQUIRE(blur);
    CHECK(blur->height() < listRow);
    ui.saveCrop(QStringLiteral("effects-compact.png"));

    // Icons: tiles in rows under each folder, the same names and behaviour.
    QQuickItem* icons = named(ui.panel, QStringLiteral("effectsView_icons"));
    REQUIRE(icons);
    clickAt(ui.w, icons, QPointF(icons->width() / 2, icons->height() / 2));
    CHECK(shell.preferences->property("effectsView").toString() == QStringLiteral("icons"));
    CHECK(ui.tree->columns() > 1);
    const int tilesRow = ui.tree->indexOf(QStringLiteral("video/Blur & Sharpen#0"));
    REQUIRE(tilesRow > 0);
    CHECK(ui.tree->get(tilesRow).value(QStringLiteral("rowType")).toString() == QStringLiteral("tiles"));
    QQuickItem* tile = ui.row(QStringLiteral("effectItem_wabcut.blur.gaussian"));
    REQUIRE(tile);
    CHECK(tile->width() > tile->height() * 0.8);
    CHECK(tile->width() <= ui.list->width());
    ui.select(QStringLiteral("video/Blur & Sharpen|wabcut.blur.gaussian"));
    ui.saveCrop(QStringLiteral("effects-icons.png"));

    // The search opens every folder with a match and says how many; Esc clears it and the user's folders come back.
    QQuickItem* listButton = named(ui.panel, QStringLiteral("effectsView_list"));
    REQUIRE(listButton);
    clickAt(ui.w, listButton, QPointF(listButton->width() / 2, listButton->height() / 2));
    CHECK(ui.tree->columns() == 1);
    QQuickItem* search = named(ui.panel, QStringLiteral("effectsSearch"));
    REQUIRE(search);
    search->forceActiveFocus();
    search->setProperty("text", QStringLiteral("blur"));
    spin(100);
    CHECK(ui.tree->searching());
    CHECK(ui.tree->indexOf(QStringLiteral("video/Blur & Sharpen|wabcut.blur.gaussian")) > 0);
    CHECK(ui.tree->indexOf(QStringLiteral("audioTransition")) < 0); // no match there
    QQuickItem* matches = named(ui.panel, QStringLiteral("effectsMatchCount"));
    REQUIRE(matches);
    CHECK(matches->isVisible());
    CHECK(matches->property("text").toString().startsWith(QString::number(ui.tree->matchCount())));
    ui.saveCrop(QStringLiteral("effects-search.png"));
    search->setProperty("text", QStringLiteral("zzzzq"));
    spin(50);
    QQuickItem* none = named(ui.panel, QStringLiteral("effectsNoMatch"));
    REQUIRE(none);
    CHECK(none->isVisible());
    sendKey(ui.w, Qt::Key_Escape);
    CHECK(search->property("text").toString().isEmpty());
    CHECK_FALSE(none->isVisible());
    CHECK(ui.tree->isExpanded(QStringLiteral("video")));
    CHECK_FALSE(ui.tree->isExpanded(QStringLiteral("presets")));

    // The filter menu: Transitions keeps the transition folders only.
    ui.panel->setProperty("filterKey", QStringLiteral("transitions"));
    CHECK(topKeys(*ui.tree) == QStringList{QStringLiteral("favorites"), QStringLiteral("audioTransition"), QStringLiteral("videoTransition")});
    ui.panel->setProperty("filterKey", QStringLiteral("all"));

    // The light theme.
    shell.setAppearance(false, "ultraviolet");
    spin(300);
    ui.saveCrop(QStringLiteral("effects-list-light.png"));
    shell.setAppearance(true, "ultraviolet");
    spin(100);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Effects panel UI: the keyboard walks the folders and applies to every selected clip", "[app][effects][qml]") {
    resetSettings();
    clearMessages();
    sharedEffectLibrary()->set({});
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    EffectsUi ui(shell);
    Timeline tl(kTwoClips);
    tl.linkMatching();
    ui.controller->resetProject(tl.state, tl.seq);
    const ClipId first = tl.clip("V1@0");
    const ClipId second = tl.clip("V1@42");
    ui.controller->setSelection({first, second});
    ui.show();
    const auto effectsOf = [&](ClipId clip) { return ui.controller->currentSequence()->findClip(clip)->clip().effects(); };
    const auto bothClips = [&] {
        return ui.controller->currentSequence()->findClip(first) && ui.controller->currentSequence()->findClip(second);
    };

    // Shift+7 (the panel's focus key) puts the keyboard in the search; Down goes to the first match.
    QQuickItem* search = named(ui.panel, QStringLiteral("effectsSearch"));
    REQUIRE(search);
    REQUIRE(waitFor([&] { return search->hasActiveFocus(); }, 3000));
    search->setProperty("text", QStringLiteral("gaussian blur"));
    spin(50);
    sendKey(ui.w, Qt::Key_Down);
    CHECK(ui.list->hasActiveFocus());
    CHECK(ui.currentKey().find('|') != std::string::npos); // an item (the Convolution Kernel preset "Gaussian Blur" first)
    ui.select(QStringLiteral("video/Blur & Sharpen|wabcut.blur.gaussian"));

    // Enter: on both selected clips, one undo step.
    const auto undo = ui.controller->session().undoCount();
    sendKey(ui.w, Qt::Key_Return);
    CHECK(ui.controller->session().undoCount() == undo + 1);
    REQUIRE_FALSE(effectsOf(first).empty());
    REQUIRE_FALSE(effectsOf(second).empty());
    CHECK(effectsOf(first).back().effect == "wabcut.blur.gaussian");
    CHECK(effectsOf(second).back().effect == "wabcut.blur.gaussian");
    CHECK(ui.status().find("2 clips") != std::string::npos);

    // Delete belongs to the panel here (not the keymap's Clear, which would delete the selected clips): a built-in
    // effect cannot be deleted, and it says so.
    sendKey(ui.w, Qt::Key_Delete);
    CHECK(bothClips());
    CHECK_FALSE(ui.status().empty());
    QObject* dialog = shell.window->findChild<QObject*>(QStringLiteral("effectsConfirmDialog"));
    REQUIRE(dialog);
    CHECK_FALSE(dialog->property("visible").toBool());

    // Up and Down walk the rows; Left goes to the folder, then closes it; Right opens it and goes in.
    search->setProperty("text", QString());
    spin(50);
    ui.list->forceActiveFocus();
    ui.select(QStringLiteral("presets"));
    sendKey(ui.w, Qt::Key_Right);
    CHECK(ui.tree->isExpanded(QStringLiteral("presets")));
    CHECK(ui.currentKey() == "presets");
    sendKey(ui.w, Qt::Key_Right);
    const std::string child = ui.currentKey();
    CHECK(child.rfind("presets/", 0) == 0);
    sendKey(ui.w, Qt::Key_Down);
    CHECK(ui.currentKey() != child);
    sendKey(ui.w, Qt::Key_Up);
    CHECK(ui.currentKey() == child);
    sendKey(ui.w, Qt::Key_Left);
    CHECK(ui.currentKey() == "presets");
    sendKey(ui.w, Qt::Key_Left);
    CHECK_FALSE(ui.tree->isExpanded(QStringLiteral("presets")));
    // Enter on a folder opens it; End goes to the last row; Home to the first, and Up from there to the search.
    sendKey(ui.w, Qt::Key_Return);
    CHECK(ui.tree->isExpanded(QStringLiteral("presets")));
    sendKey(ui.w, Qt::Key_End);
    CHECK(ui.currentKey() == ui.tree->get(ui.tree->rowCount() - 1).value(QStringLiteral("rowKey")).toString().toStdString());
    sendKey(ui.w, Qt::Key_Home);
    CHECK(ui.currentKey() == ui.tree->get(0).value(QStringLiteral("rowKey")).toString().toStdString());
    sendKey(ui.w, Qt::Key_Up);
    CHECK(search->hasActiveFocus());
    CHECK(bothClips());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Effects panel UI: deletes ask first and custom bins are named in place", "[app][effects][qml]") {
    resetSettings();
    clearMessages();
    sharedEffectLibrary()->set({});
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    EffectsUi ui(shell);
    REQUIRE(ui.presets->newCustomBin(QStringLiteral("wabcut.blur.gaussian")) == QStringLiteral("Custom Bin 01"));
    ui.show();
    ui.tree->setExpanded(QStringLiteral("custom:Custom Bin 01"), true);
    QObject* dialog = shell.window->findChild<QObject*>(QStringLiteral("effectsConfirmDialog"));
    REQUIRE(dialog);

    // F2 on a custom bin: its name in a field; Enter keeps the new name and the selection follows the bin.
    ui.list->forceActiveFocus();
    ui.select(QStringLiteral("custom:Custom Bin 01"));
    sendKey(ui.w, Qt::Key_F2);
    QQuickItem* field = ui.row(QStringLiteral("effectsRenameField"));
    REQUIRE(field);
    CHECK(field->hasActiveFocus());
    field->setProperty("text", QStringLiteral("Gaming"));
    sendKey(ui.w, Qt::Key_Return);
    CHECK(ui.presets->customBins() == QStringList{QStringLiteral("Gaming")});
    REQUIRE(waitFor([&] { return ui.currentKey() == "custom:Gaming"; }, 2000));
    CHECK(ui.row(QStringLiteral("effectsRenameField")) == nullptr);
    CHECK(ui.tree->isExpanded(QStringLiteral("custom:Gaming")));

    // Delete on an item in the bin asks first; Cancel keeps it, the confirm button takes it out of the bin.
    ui.select(QStringLiteral("custom:Gaming|wabcut.blur.gaussian"));
    CHECK(ui.presets->currentBin() == QStringLiteral("Gaming"));
    sendKey(ui.w, Qt::Key_Delete);
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 3000));
    CHECK(dialog->property("question").toString().contains(QStringLiteral("Gaussian Blur")));
    QObject* cancel = shell.window->findChild<QObject*>(QStringLiteral("effectsConfirmCancel"));
    REQUIRE(cancel);
    REQUIRE(QMetaObject::invokeMethod(cancel, "click"));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 3000));
    CHECK(ui.tree->indexOf(QStringLiteral("custom:Gaming|wabcut.blur.gaussian")) > 0);
    // Backspace is the keymap's Delete Custom Item: it asks through the panel too (Main.qml).
    ui.list->forceActiveFocus();
    ui.select(QStringLiteral("custom:Gaming|wabcut.blur.gaussian"));
    sendKey(ui.w, Qt::Key_Backspace);
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 3000));
    QObject* ok = shell.window->findChild<QObject*>(QStringLiteral("effectsConfirmOk"));
    REQUIRE(ok);
    CHECK(ok->property("text").toString() == QStringLiteral("Remove"));
    REQUIRE(QMetaObject::invokeMethod(ok, "click"));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 3000));
    CHECK(ui.tree->indexOf(QStringLiteral("custom:Gaming|wabcut.blur.gaussian")) < 0);
    CHECK(ui.status() == "Removed Gaussian Blur from Gaming");
    // The selection falls back on its folder.
    CHECK(waitFor([&] { return ui.currentKey() == "custom:Gaming"; }, 2000));

    // Delete on the bin: asked, then gone.
    ui.list->forceActiveFocus();
    sendKey(ui.w, Qt::Key_Delete);
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 3000));
    CHECK(dialog->property("question").toString().contains(QStringLiteral("Gaming")));
    REQUIRE(QMetaObject::invokeMethod(ok, "click"));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 3000));
    CHECK(ui.presets->customBins().isEmpty());

    // New Custom Bin (the toolbar's button): made, shown and named in place; Esc keeps the name it got.
    QQuickItem* newBin = named(ui.panel, QStringLiteral("effectsNewBinButton"));
    REQUIRE(newBin);
    clickAt(ui.w, newBin, QPointF(newBin->width() / 2, newBin->height() / 2));
    REQUIRE(waitFor([&] { return ui.row(QStringLiteral("effectsRenameField")) != nullptr; }, 2000));
    CHECK(ui.currentKey() == "custom:Custom Bin 01");
    CHECK(ui.row(QStringLiteral("effectsRenameField"))->hasActiveFocus());
    sendKey(ui.w, Qt::Key_Escape);
    CHECK(ui.row(QStringLiteral("effectsRenameField")) == nullptr);
    CHECK(ui.presets->customBins() == QStringList{QStringLiteral("Custom Bin 01")});
    CHECK(ui.list->hasActiveFocus());
    // A built-in folder cannot be renamed or deleted, and says so.
    ui.select(QStringLiteral("video"));
    sendKey(ui.w, Qt::Key_F2);
    CHECK(ui.row(QStringLiteral("effectsRenameField")) == nullptr);
    sendKey(ui.w, Qt::Key_Delete);
    CHECK_FALSE(dialog->property("visible").toBool());
    CHECK_FALSE(ui.status().empty());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// QA round fixes: Delete on the toolbar's buttons, the Icons view's selection after a delete, a rename left with a
// click, the item menu on a copy of your preset, the danger button of the confirmation and the tiles' tooltips.
TEST_CASE("Effects panel UI: Delete stays in the panel and the Icons view keeps the selection after a delete", "[app][effects][qml]") {
    resetSettings();
    clearMessages();
    sharedEffectLibrary()->set({});
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    EffectsUi ui(shell);
    // Two selected clips on the timeline: the keymap's Clear (Delete) would delete them.
    Timeline tl(kTwoClips);
    tl.linkMatching();
    ui.controller->resetProject(tl.state, tl.seq);
    const ClipId first = tl.clip("V1@0");
    const ClipId second = tl.clip("V1@42");
    ui.controller->setSelection({first, second});
    const auto bothClips = [&] {
        return ui.controller->currentSequence()->findClip(first) && ui.controller->currentSequence()->findClip(second);
    };
    // One of your presets with a long name and no description, and a custom bin of four items (tile rows of two).
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QUrl file = QUrl::fromLocalFile(dir.filePath(QStringLiteral("twirl.wabcut-presets.json")));
    REQUIRE(outcome(ui.presets->exportPresets(file, {QStringLiteral("Twirl In")})) == "");
    REQUIRE(outcome(ui.presets->newPresetsBin(QStringLiteral("Mine"))) == "");
    REQUIRE(outcome(ui.presets->importPresets(file, QStringLiteral("Mine"))) == "");
    const QString imported = ui.presets->lastImported().value(0);
    REQUIRE_FALSE(imported.isEmpty());
    const QString longName = QStringLiteral("Twirl in from the far left corner of the frame with a slow start and a soft stop");
    const QString type = ui.presets->preset(imported).value(QStringLiteral("type")).toString();
    REQUIRE(outcome(ui.presets->setPresetProperties(imported, longName, type, QString())) == "");
    REQUIRE(ui.presets->newCustomBin(QStringLiteral("wabcut.blur.gaussian")) == QStringLiteral("Custom Bin 01"));
    const QStringList more{QStringLiteral("wabcut.audio.gain"), QStringLiteral("crossDissolve"), QStringLiteral("preset:") + longName};
    for (const QString& item : more)
        REQUIRE(outcome(ui.presets->addToCustomBin(QStringLiteral("Custom Bin 01"), item)) == "");
    shell.preferences->setProperty("effectsView", QStringLiteral("icons"));
    ui.show();
    const QString bin = QStringLiteral("custom:Custom Bin 01");
    ui.tree->setExpanded(bin, true);
    ui.tree->setExpanded(QStringLiteral("presets"), true);
    ui.tree->setExpanded(QStringLiteral("presets/u:Mine"), true);
    spin(50);
    REQUIRE(ui.tree->columns() > 1);
    // The tiles of the bin's first tiles row (row keys).
    const auto firstRowTiles = [&] {
        QStringList keys;
        for (const QVariant& t : roleOf(*ui.tree, bin + QStringLiteral("#0"), "tiles").toList())
            keys << t.toMap().value(QStringLiteral("rowKey")).toString();
        return keys;
    };
    REQUIRE(firstRowTiles().size() >= 2);
    QObject* dialog = shell.window->findChild<QObject*>(QStringLiteral("effectsConfirmDialog"));
    QObject* ok = shell.window->findChild<QObject*>(QStringLiteral("effectsConfirmOk"));
    QObject* cancel = shell.window->findChild<QObject*>(QStringLiteral("effectsConfirmCancel"));
    REQUIRE(dialog);
    REQUIRE(ok);
    REQUIRE(cancel);

    // A name cut short in its tile shows whole in the tooltip, before the description (a preset without one lists its
    // effects); every tile's tooltip starts with its name.
    const QString longKey = QStringLiteral("presets/u:Mine|preset:") + longName;
    QQuickItem* longTile = named(ui.reveal(longKey), QStringLiteral("effectItem_preset:") + longName);
    REQUIRE(longTile);
    CHECK(longTile->property("nameElided").toBool());
    INFO(longTile->property("tooltipText").toString().toStdString());
    CHECK(longTile->property("tooltipText").toString().startsWith(longName + QStringLiteral(" "))); // then its effects
    QQuickItem* blurTile = named(ui.reveal(bin + QStringLiteral("|wabcut.blur.gaussian")),
                                 QStringLiteral("effectItem_custom:Custom Bin 01_wabcut.blur.gaussian"));
    REQUIRE(blurTile);
    CHECK(blurTile->property("tooltipText").toString().startsWith(QStringLiteral("Gaussian Blur ")));
    CHECK(blurTile->property("tooltipText").toString().size() > QStringLiteral("Gaussian Blur ").size());

    // The item menu of the copy of your preset in the bin: Remove from the bin, and no second Delete that does the
    // same; the preset's own tile keeps Delete.
    const auto entryShown = [&](const QString& key, const char* entry) {
        QQuickItem* target = ui.list;
        REQUIRE(QMetaObject::invokeMethod(ui.panel, "openMenu", Q_ARG(QVariant, key), Q_ARG(QVariant, QVariant::fromValue(target)),
                                          Q_ARG(QVariant, 20.0), Q_ARG(QVariant, 10.0)));
        QObject* item = ui.menuItem(entry);
        REQUIRE(item);
        QObject* menu = item->parent();
        while (menu && !menu->inherits("QQuickMenu"))
            menu = menu->parent();
        REQUIRE(menu);
        REQUIRE(waitFor([&] { return menu->property("opened").toBool(); }, 3000));
        const bool shown = item->property("visible").toBool();
        REQUIRE(QMetaObject::invokeMethod(menu, "close"));
        REQUIRE(waitFor([&] { return !menu->property("visible").toBool(); }, 3000));
        return shown;
    };
    const QString copyKey = bin + QStringLiteral("|preset:") + longName;
    CHECK(entryShown(copyKey, "removeFromCustomBin"));
    CHECK_FALSE(entryShown(copyKey, "deletePresetItem"));
    CHECK(entryShown(longKey, "deletePresetItem"));

    // Delete with the keyboard on a toolbar button (a click leaves it there) is still the panel's: it asks about the
    // selected item and the timeline's clips stay.
    const QString firstTile = firstRowTiles().value(0);
    ui.list->forceActiveFocus();
    ui.select(firstTile);
    QQuickItem* viewButton = named(ui.panel, QStringLiteral("effectsView_icons"));
    REQUIRE(viewButton);
    viewButton->forceActiveFocus(Qt::MouseFocusReason);
    REQUIRE(viewButton->hasActiveFocus());
    sendKey(ui.w, Qt::Key_Delete);
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 3000));
    CHECK(bothClips());
    CHECK(ok->property("text").toString() == QStringLiteral("Remove"));
    // The confirm button of a destructive question: Theme.danger's fill under black text.
    CHECK(ok->property("variant").toString() == QStringLiteral("danger"));
    auto* okBackground = ok->property("background").value<QQuickItem*>();
    REQUIRE(okBackground);
    CHECK(sameColor(okBackground->property("color").value<QColor>(), shell.themeColor("danger")));
    CHECK(sameColor(ok->property("foreground").value<QColor>(), shell.themeColor("clipInk"), 0));
    spin(100);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/effects-confirm-remove.png")));
    REQUIRE(QMetaObject::invokeMethod(cancel, "click"));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 3000));
    CHECK(bothClips());
    CHECK(ui.tree->indexOf(firstTile) > 0);

    // The Icons view: the deleted tile hands the selection to the tile now in its place, not to the top of the list.
    ui.list->forceActiveFocus();
    ui.select(firstTile);
    sendKey(ui.w, Qt::Key_Delete);
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 3000));
    REQUIRE(QMetaObject::invokeMethod(ok, "click"));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 3000));
    REQUIRE(waitFor([&] { return ui.tree->indexOf(firstTile) < 0; }, 2000));
    CHECK(bothClips());
    const QString inItsPlace = firstRowTiles().value(0);
    REQUIRE_FALSE(inItsPlace.isEmpty());
    CHECK(waitFor([&] { return ui.currentKey() == inItsPlace.toStdString(); }, 2000));
    // The menu's Remove from the bin (no dialog) does the same, and the arrows go on from there.
    ui.runMenu(inItsPlace, "removeFromCustomBin");
    REQUIRE(waitFor([&] { return ui.tree->indexOf(inItsPlace) < 0; }, 2000));
    const QStringList left = firstRowTiles();
    REQUIRE(left.size() >= 2);
    CHECK(waitFor([&] { return ui.currentKey() == left.value(0).toStdString(); }, 2000));
    ui.list->forceActiveFocus();
    sendKey(ui.w, Qt::Key_Right);
    CHECK(ui.currentKey() == left.value(1).toStdString());

    // A rename in place left with a click in the search: the name is kept and the search keeps the keyboard.
    ui.select(bin);
    sendKey(ui.w, Qt::Key_F2);
    QQuickItem* field = ui.row(QStringLiteral("effectsRenameField"));
    REQUIRE(field);
    REQUIRE(field->hasActiveFocus());
    field->setProperty("text", QStringLiteral("Gaming"));
    QQuickItem* search = named(ui.panel, QStringLiteral("effectsSearch"));
    REQUIRE(search);
    search->forceActiveFocus(Qt::MouseFocusReason);
    spin(50);
    CHECK(ui.presets->customBins() == QStringList{QStringLiteral("Gaming")});
    CHECK(ui.row(QStringLiteral("effectsRenameField")) == nullptr);
    CHECK(search->hasActiveFocus());
    CHECK_FALSE(ui.list->hasActiveFocus());
    CHECK(bothClips());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Effects panel UI: drops on a custom bin and a presets bin and presets files from Explorer", "[app][effects][qml]") {
    resetSettings();
    clearMessages();
    sharedEffectLibrary()->set({});
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    EffectsUi ui(shell);
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QUrl file = QUrl::fromLocalFile(dir.filePath(QStringLiteral("twirl.wabcut-presets.json")));
    REQUIRE(outcome(ui.presets->exportPresets(file, {QStringLiteral("Twirl In")})) == "");
    REQUIRE(ui.presets->newCustomBin(QString()) == QStringLiteral("Custom Bin 01"));
    REQUIRE(outcome(ui.presets->newPresetsBin(QStringLiteral("Mine"))) == "");
    REQUIRE(outcome(ui.presets->newPresetsBin(QStringLiteral("Other"))) == "");
    REQUIRE(outcome(ui.presets->importPresets(file, QStringLiteral("Mine"))) == "");
    const QString mine = ui.presets->lastImported().value(0);
    REQUIRE_FALSE(mine.isEmpty());
    ui.show();
    ui.tree->setExpanded(QStringLiteral("presets"), true);
    spin(50);
    // The drags' data lives as long as the test: Qt's drag state may still refer to the last one.
    QMimeData effect;
    QMimeData ownPreset;
    QMimeData builtInPreset;
    QMimeData files;

    // An effect dropped on a custom bin goes into it.
    QQuickItem* bin = ui.reveal(QStringLiteral("custom:Custom Bin 01"));
    REQUIRE(bin);
    REQUIRE(ui.inView(bin, QPointF(40, 10)));
    effect.setData(QStringLiteral("application/x-wabcut-effect"), QByteArrayLiteral("wabcut.blur.gaussian"));
    CHECK(dropOn(ui.w, bin, QPointF(40, 10), &effect));
    const QVariantList custom = ui.presets->customGroups({});
    REQUIRE(custom.size() == 1);
    CHECK(itemNames(custom[0].toMap(), QString()) == QStringList{QStringLiteral("Gaussian Blur")});
    CHECK(ui.status() == "Added Gaussian Blur to Custom Bin 01");

    // A preset of yours dropped on another presets bin moves there; a built-in one is refused.
    QQuickItem* other = ui.reveal(QStringLiteral("presets/u:Other"));
    REQUIRE(other);
    ownPreset.setData(QStringLiteral("application/x-wabcut-effect"), (QStringLiteral("preset:") + mine).toUtf8());
    CHECK(dropOn(ui.w, other, QPointF(40, 10), &ownPreset));
    CHECK(ui.presets->preset(mine).value(QStringLiteral("bin")).toString() == QStringLiteral("Other"));
    builtInPreset.setData(QStringLiteral("application/x-wabcut-effect"), QByteArrayLiteral("preset:Twirl Out"));
    CHECK_FALSE(dropOn(ui.w, ui.reveal(QStringLiteral("presets/u:Other")), QPointF(40, 10), &builtInPreset));
    CHECK(ui.presets->preset(QStringLiteral("Twirl Out")).value(QStringLiteral("bin")).toString() == QStringLiteral("Twirls"));

    // A presets file from Explorer dropped on a presets bin: imported into it, and the first one is shown.
    QQuickItem* mineFolder = ui.reveal(QStringLiteral("presets/u:Mine"));
    REQUIRE(mineFolder);
    files.setUrls({file});
    CHECK(dropOn(ui.w, mineFolder, QPointF(40, 10), &files));
    const QString imported = ui.presets->lastImported().value(0);
    REQUIRE_FALSE(imported.isEmpty());
    CHECK(ui.presets->preset(imported).value(QStringLiteral("bin")).toString() == QStringLiteral("Mine"));
    CHECK(ui.currentKey() == "presets/u:Mine|preset:" + imported.toStdString());
    CHECK(ui.status() == "Imported 1 preset into Mine");

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Effects panel UI: Import and Export Presets of a bin and the default transition from the menus", "[app][effects][qml]") {
    resetSettings();
    clearMessages();
    sharedEffectLibrary()->set({});
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    EffectsUi ui(shell);
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QUrl twirl = QUrl::fromLocalFile(dir.filePath(QStringLiteral("twirl.wabcut-presets.json")));
    REQUIRE(outcome(ui.presets->exportPresets(twirl, {QStringLiteral("Twirl In")})) == "");
    REQUIRE(outcome(ui.presets->newPresetsBin(QStringLiteral("Mine"))) == "");
    REQUIRE(outcome(ui.presets->newPresetsBin(QStringLiteral("Other"))) == "");
    REQUIRE(outcome(ui.presets->importPresets(twirl, QStringLiteral("Mine"))) == "");
    const QString mine = ui.presets->lastImported().value(0);
    ui.show();
    QObject* files = ui.panel->findChild<QObject*>(QStringLiteral("effectsPresetFiles"));
    REQUIRE(files);
    files->setProperty("dialogsEnabled", false); // the system dialogs are not shown in tests: their outcome is given
    ui.tree->setExpanded(QStringLiteral("presets"), true);
    spin(50);

    // Export Presets... of the bin Mine: its file is named after it, in the folder used last.
    ui.runMenu(QStringLiteral("presets/u:Mine"), "folderExportPresets");
    CHECK(files->property("exportBinPath").toString() == QStringLiteral("Mine"));
    const QUrl exported = ui.presets->presetsFileUrl(QStringLiteral("Mine"));
    CHECK(exported.fileName() == QStringLiteral("Mine.wabcut-presets.json"));
    REQUIRE(QMetaObject::invokeMethod(files, "acceptExport", Q_ARG(QUrl, exported)));
    CHECK(QFileInfo::exists(exported.toLocalFile()));
    CHECK(ui.status() == "Exported Mine");
    QFile written(exported.toLocalFile());
    REQUIRE(written.open(QIODevice::ReadOnly));
    CHECK(written.readAll().contains(mine.toUtf8()));

    // Import Presets... on the bin Other: the file's presets go there, and the first one is shown and selected.
    ui.runMenu(QStringLiteral("presets/u:Other"), "folderImportPresets");
    CHECK(files->property("importBin").toString() == QStringLiteral("Other"));
    REQUIRE(QMetaObject::invokeMethod(files, "acceptImport", Q_ARG(QVariant, QVariantList{exported})));
    const QString copy = ui.presets->lastImported().value(0);
    REQUIRE_FALSE(copy.isEmpty());
    CHECK(ui.presets->preset(copy).value(QStringLiteral("bin")).toString() == QStringLiteral("Other"));
    CHECK(ui.currentKey() == "presets/u:Other|preset:" + copy.toStdString());
    CHECK(ui.status() == "Imported 1 preset into Other");

    // The panel menu's Export Presets... is there while you have presets; New Presets Bin names the bin in place.
    QObject* exportAll = ui.menuItem("exportPresetsItem");
    REQUIRE(exportAll);
    CHECK(exportAll->property("enabled").toBool());
    ui.runMenu(QStringLiteral("presets/u:Other"), "folderNewPresetsBin");
    REQUIRE(waitFor([&] { return ui.row(QStringLiteral("effectsRenameField")) != nullptr; }, 2000));
    CHECK(ui.currentKey() == "presets/u:Other/Presets Bin");
    QQuickItem* field = ui.row(QStringLiteral("effectsRenameField"));
    field->setProperty("text", QStringLiteral("Lower thirds"));
    sendKey(ui.w, Qt::Key_Return);
    CHECK(ui.presets->presetBins().contains(QStringLiteral("Other/Lower thirds")));
    REQUIRE(waitFor([&] { return ui.currentKey() == "presets/u:Other/Lower thirds"; }, 2000));

    // Set as Default Transition (stream T's setting) from a transition's menu; DEFAULT shows on its row.
    ui.tree->setExpanded(QStringLiteral("videoTransition"), true);
    ui.tree->setExpanded(QStringLiteral("videoTransition/Dissolve"), true);
    const QString additive = QStringLiteral("videoTransition/Dissolve|additiveDissolve");
    REQUIRE(ui.tree->indexOf(additive) > 0);
    ui.select(additive);
    ui.runMenu(additive, "defaultTransitionItem");
    QObject* editing = shell.engine.singletonInstance<QObject*>("Wabcut", "Editing");
    REQUIRE(editing);
    CHECK(editing->property("defaultVideoTransition").toString() == QStringLiteral("additiveDissolve"));
    QObject* item = ui.menuItem("defaultTransitionItem");
    REQUIRE(item);
    CHECK_FALSE(item->property("enabled").toBool()); // it is the default now
    editing->setProperty("defaultVideoTransition", QStringLiteral("crossDissolve"));

    // Add to Favorites from the item menu; the row is marked.
    ui.runMenu(additive, "favoriteItem");
    CHECK(ui.presets->isFavorite(QStringLiteral("additiveDissolve")));
    CHECK(ui.tree->get(ui.tree->indexOf(additive)).value(QStringLiteral("favorite")).toBool());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Effects panel UI: every control has a name and Tab reaches the search the tools and the tree", "[app][effects][qml][a11y]") {
    resetSettings();
    clearMessages();
    sharedEffectLibrary()->set({});
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    EffectsUi ui(shell);
    REQUIRE(ui.presets->newCustomBin(QStringLiteral("crossDissolve")) == QStringLiteral("Custom Bin 01"));
    REQUIRE(outcome(ui.presets->setFavorite(QStringLiteral("wabcut.blur.gaussian"), true)) == "");
    ui.show();
    ui.tree->expandAll(QStringLiteral("video"));
    ui.tree->setExpanded(QStringLiteral("custom:Custom Bin 01"), true);
    QMetaObject::invokeMethod(ui.list, "forceLayout");
    spin(50);

    const QStringList issues = auditAccessibility(ui.panel);
    INFO(join(issues));
    CHECK(issues.isEmpty());
    INFO("chain: " << describeTabChain(ui.panel).toStdString());
    for (const char* name : {"effectsSearch", "effectsView_list", "effectsView_compact", "effectsView_icons",
                             "effectsFilterButton", "effectsNewBinButton", "effectsPanelMenuButton", "effectsList"})
        CHECK(tabReaches(ui.panel, QString::fromLatin1(name)));
    // The tree names itself, and its rows say what they are.
    QQuickItem* folder = ui.reveal(QStringLiteral("video"));
    REQUIRE(folder);
    QAccessibleInterface* folderIface = QAccessible::queryAccessibleInterface(folder);
    REQUIRE(folderIface);
    CHECK(folderIface->role() == QAccessible::TreeItem);
    CHECK(folderIface->text(QAccessible::Name) == QStringLiteral("Video Effects"));
    CHECK(folderIface->text(QAccessible::Description).startsWith(QStringLiteral("Folder, open")));
    QAccessibleInterface* listIface = QAccessible::queryAccessibleInterface(ui.list);
    REQUIRE(listIface);
    CHECK(listIface->text(QAccessible::Name) == QStringLiteral("Effects"));

    // The list keeps the keyboard and the selected row is the accessible focus: screen readers follow it as the
    // arrows move it (folders, items, and the Icons view's tiles), and nothing is focused once the list lets go.
    ui.list->forceActiveFocus();
    ui.select(QStringLiteral("video"));
    folder = ui.reveal(QStringLiteral("video"));
    folderIface = QAccessible::queryAccessibleInterface(folder);
    REQUIRE(folderIface);
    CHECK(folderIface->state().focusable);
    CHECK(folderIface->state().focused);
    CHECK(folderIface->state().selected);
    const QString blurKey = QStringLiteral("video/Blur & Sharpen|wabcut.blur.gaussian");
    ui.select(blurKey);
    QQuickItem* blur = ui.reveal(blurKey);
    REQUIRE(blur);
    QAccessibleInterface* blurIface = QAccessible::queryAccessibleInterface(blur);
    REQUIRE(blurIface);
    CHECK(blurIface->role() == QAccessible::TreeItem);
    CHECK(blurIface->state().focused);
    folderIface = QAccessible::queryAccessibleInterface(ui.reveal(QStringLiteral("video")));
    REQUIRE(folderIface);
    CHECK_FALSE(folderIface->state().focused);
    shell.preferences->setProperty("effectsView", QStringLiteral("icons"));
    spin(50);
    QQuickItem* tile = named(ui.reveal(blurKey), QStringLiteral("effectItem_wabcut.blur.gaussian"));
    REQUIRE(tile);
    QAccessibleInterface* tileIface = QAccessible::queryAccessibleInterface(tile);
    REQUIRE(tileIface);
    CHECK(tileIface->state().focusable);
    CHECK(tileIface->state().focused);
    QQuickItem* search = named(ui.panel, QStringLiteral("effectsSearch"));
    REQUIRE(search);
    search->forceActiveFocus();
    CHECK_FALSE(QAccessible::queryAccessibleInterface(tile)->state().focused);
    shell.preferences->setProperty("effectsView", QStringLiteral("list"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
