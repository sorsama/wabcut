// 0.2: the Effects panel's presets, custom bins and favourites (EffectPresetsBridge), Save Preset from Effect Controls,
// and presets dropped on the timeline or Effect Controls. Offscreen; no window is shown.

#include "TimelineHarness.h"

#include "EffectControls.h"
#include "EffectPresetsBridge.h"
#include "ProjectController.h"
#include "TimelineView.h"

#include <wabcut/command/ExecutionContext.h>

#include <catch2/catch_test_macros.hpp>

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
