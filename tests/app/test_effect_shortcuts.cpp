// 0.7 (card #82): keyboard shortcuts for individual effects and presets. Every effect and preset is a run-time command of
// the keymap ("fx:<effect id>", "preset:<name>"; groups "Apply Effect" and "Apply Preset"), listed by Keyboard Shortcuts,
// bound like any command, applied to the selected clips by a key (one undo step, a toast message), kept in the user's keys
// when the preset is deleted ("(unavailable)") and across restarts, and living beside the extensions' commands. Offscreen;
// no window is shown.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "EffectControls.h"
#include "EffectPresetsBridge.h"
#include "EffectShortcutsBridge.h"
#include "KeymapDispatcher.h"
#include "ProjectController.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/keymap/Key.h>
#include <wabcut/keymap/Scope.h>

#include <catch2/catch_test_macros.hpp>

#include <QTemporaryDir>

#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace km = wabcut::keymap;

namespace {

constexpr const char* kPicture = "V1 |Aaaaaaaaaaaaaaaaaaaa..Bbbbbbbbbbbbbbbbbbbb|\n"
                                 "A1 |Aaaaaaaaaaaaaaaaaaaa..Bbbbbbbbbbbbbbbbbbbb|";

std::string resolved(const KeymapDispatcher& keys, const char* chord) {
    const auto parsed = km::parseKeyChord(chord, km::Platform::Windows);
    REQUIRE(parsed);
    return keys.keymap().resolve(km::Scope::Application, *parsed).value_or(std::string());
}

/// What a bridge reported with `applied`, in order.
struct Applied {
    QObject context;
    std::vector<std::pair<QString, bool>> calls;
    explicit Applied(EffectShortcutsBridge& bridge) {
        QObject::connect(&bridge, &EffectShortcutsBridge::applied, &context,
                         [this](const QString& message, bool ok) { calls.emplace_back(message, ok); });
    }
    std::size_t count() const { return calls.size(); }
    std::pair<QString, bool> take() {
        auto first = calls.front();
        calls.erase(calls.begin());
        return first;
    }
};

const km::CommandInfo* commandOf(const KeymapDispatcher& keys, std::string_view id) { return keys.keymap().registry().find(id); }

struct Fixture {
    QTemporaryDir config;
    KeymapDispatcher keys;
    Timeline tl{kPicture};
    ProjectController controller;
    EffectPresetsBridge presets;
    EffectShortcutsBridge shortcuts;
    ClipId a;
    ClipId b;

    Fixture() {
        resetSettings();
        sharedEffectLibrary()->set({});
        keys.setOverridesPath(config.filePath(QStringLiteral("keymap/overrides.json")));
        keys.setPlatform(QStringLiteral("windows"));
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        presets.setController(&controller);
        presets.setPersistent(false);
        shortcuts.setController(&controller);
        shortcuts.setPresets(&presets);
        shortcuts.setDispatcher(&keys);
        a = clipAt(*controller.snapshot(), tl.seq, "V1@0");
        b = clipAt(*controller.snapshot(), tl.seq, "V1@22");
    }
    std::vector<std::string> stackOf(ClipId clip) {
        std::vector<std::string> out;
        const auto seq = controller.currentSequence();
        for (const EffectInstance& e : seq->findClip(clip)->clip().effects())
            out.push_back(e.effect);
        return out;
    }
    std::size_t undoCount() { return controller.session().undoCount(); }
};

} // namespace

TEST_CASE("Effect shortcuts: every effect and preset is a keymap command in its own group", "[app][effects][shortcuts][keymap]") {
    Fixture f;
    CHECK(f.shortcuts.count() > 100);
    const km::CommandInfo* blur = commandOf(f.keys, "fx:wabcut.blur.gaussian");
    REQUIRE(blur != nullptr);
    CHECK(blur->scope == km::Scope::Application);
    CHECK(blur->group == "Apply Effect");
    CHECK(blur->displayName == "Apply Gaussian Blur");
    const km::CommandInfo* gain = commandOf(f.keys, "fx:wabcut.audio.gain");
    REQUIRE(gain != nullptr);
    CHECK(gain->group == "Apply Effect");
    CHECK(std::string(gain->displayName).ends_with("(audio)"));
    const km::CommandInfo* twirl = commandOf(f.keys, "preset:Twirl In");
    REQUIRE(twirl != nullptr);
    CHECK(twirl->group == "Apply Preset");
    CHECK(twirl->displayName == "Apply Preset: Twirl In");
    // Transitions, audio plug-ins and what the Effects panel does not list are not commands.
    CHECK(commandOf(f.keys, "fx:crossDissolve") == nullptr);
    CHECK(commandOf(f.keys, "fx:wabcut.audio.essentialSound") == nullptr);
    CHECK(f.shortcuts.commandFor(QStringLiteral("crossDissolve")).isEmpty());
    CHECK(f.shortcuts.commandFor(QStringLiteral("plugin.clap.something")).isEmpty());
    CHECK(f.shortcuts.commandFor(QStringLiteral("nonsense")).isEmpty());
    CHECK(f.shortcuts.commandFor(QStringLiteral("preset:")).isEmpty());
    // The Effects panel's item ids map to them.
    CHECK(f.shortcuts.commandFor(QStringLiteral("wabcut.blur.gaussian")) == QStringLiteral("fx:wabcut.blur.gaussian"));
    CHECK(f.shortcuts.commandFor(QStringLiteral("effect:wabcut.blur.gaussian")) == QStringLiteral("fx:wabcut.blur.gaussian"));
    CHECK(f.shortcuts.commandFor(QStringLiteral("preset:Twirl In")) == QStringLiteral("preset:Twirl In"));
    // No default key: nothing is bound until the user binds one.
    CHECK(f.keys.shortcutText(QStringLiteral("fx:wabcut.blur.gaussian")).isEmpty());
    CHECK(f.shortcuts.shortcutOf(QStringLiteral("wabcut.blur.gaussian")).isEmpty());
    // A user preset joins when the library changes (Save Preset), and leaves the commands when it is deleted.
    CHECK(commandOf(f.keys, "preset:Mine") == nullptr);
    f.controller.selectClipById(f.a, ProjectController::Replace);
    CHECK(f.controller.effectControls()->applyToSelectedClips(QStringLiteral("preset:Twirl In")).value(QStringLiteral("ok")).toBool());
    const std::string section = "e" + std::to_string(f.controller.currentSequence()->findClip(f.a)->clip().effects()[0].id.value);
    REQUIRE(f.presets
                .savePreset({QString::fromStdString(section), QStringLiteral("motion")}, QStringLiteral("Mine"),
                            QStringLiteral("anchorToInPoint"), QString(), QString())
                .isEmpty());
    const km::CommandInfo* mine = commandOf(f.keys, "preset:Mine");
    REQUIRE(mine != nullptr);
    CHECK(mine->displayName == "Apply Preset: Mine");
}

TEST_CASE("Effect shortcuts: a bound key applies the effect or preset to the selected clips in one undo step",
          "[app][effects][shortcuts]") {
    Fixture f;
    Applied applied(f.shortcuts);
    // Bind keys (the Keyboard Shortcuts editor's own assign), then the keys resolve to the commands.
    REQUIRE(f.keys.assignShortcut(QStringLiteral("fx:wabcut.blur.gaussian"), QStringLiteral("Ctrl+Alt+Shift+G")).value("ok").toBool());
    REQUIRE(f.keys.assignShortcut(QStringLiteral("preset:Twirl In"), QStringLiteral("Ctrl+Alt+Shift+T")).value("ok").toBool());
    CHECK(resolved(f.keys, "Ctrl+Alt+Shift+G") == "fx:wabcut.blur.gaussian");
    CHECK(resolved(f.keys, "Ctrl+Alt+Shift+T") == "preset:Twirl In");
    CHECK(f.shortcuts.shortcutOf(QStringLiteral("wabcut.blur.gaussian")) == QStringLiteral("Ctrl+Alt+Shift+G"));
    CHECK(f.shortcuts.shortcutOf(QStringLiteral("preset:Twirl In")) == QStringLiteral("Ctrl+Alt+Shift+T"));

    // The effect: both selected video clips get it, one undo step, and the toast says so.
    f.controller.setSelection({f.a, f.b});
    std::size_t before = f.undoCount();
    CHECK(f.keys.trigger(QStringLiteral("fx:wabcut.blur.gaussian"))); // what the key does
    REQUIRE(applied.count() == 1);
    CHECK(applied.take().first == QStringLiteral("Added Gaussian Blur to 2 clips"));
    CHECK(f.undoCount() == before + 1);
    CHECK(f.stackOf(f.a) == std::vector<std::string>{"wabcut.blur.gaussian"});
    CHECK(f.stackOf(f.b) == std::vector<std::string>{"wabcut.blur.gaussian"});
    f.controller.undo();
    CHECK(f.stackOf(f.a).empty());
    CHECK(f.stackOf(f.b).empty());

    // An audio effect with video clips selected goes on their linked audio clips (the Effects panel's rule).
    before = f.undoCount();
    REQUIRE(f.keys.assignShortcut(QStringLiteral("fx:wabcut.audio.gain"), QStringLiteral("Ctrl+Alt+Shift+A")).value("ok").toBool());
    CHECK(f.keys.trigger(QStringLiteral("fx:wabcut.audio.gain")));
    CHECK(applied.take().second);
    CHECK(f.undoCount() == before + 1);

    // The preset: on the selected clips, one step.
    before = f.undoCount();
    CHECK(f.keys.trigger(QStringLiteral("preset:Twirl In")));
    REQUIRE(applied.count() == 1);
    CHECK(applied.take().second);
    CHECK(f.undoCount() == before + 1);
    CHECK(f.stackOf(f.a).size() == 1);
    CHECK(f.stackOf(f.b).size() == 1);

    // Nothing selected: nothing changes, the toast says why (a warning).
    f.controller.setSelection({});
    before = f.undoCount();
    CHECK(f.keys.trigger(QStringLiteral("fx:wabcut.blur.gaussian")));
    REQUIRE(applied.count() == 1);
    const auto failed = applied.take();
    CHECK_FALSE(failed.second);
    CHECK_FALSE(failed.first.isEmpty());
    CHECK(f.undoCount() == before);
    // A command of another prefix is not this bridge's.
    CHECK(f.shortcuts.run(QStringLiteral("edit.undo")).isEmpty());
    CHECK(applied.count() == 0);
}

TEST_CASE("Effect shortcuts: a key of a deleted preset is kept as unavailable, and keys survive a restart",
          "[app][effects][shortcuts][keymap]") {
    Fixture f;
    // A user preset with a key.
    f.controller.selectClipById(f.a, ProjectController::Replace);
    REQUIRE(f.controller.effectControls()->applyToSelectedClips(QStringLiteral("preset:Twirl In")).value(QStringLiteral("ok")).toBool());
    const std::string section = "e" + std::to_string(f.controller.currentSequence()->findClip(f.a)->clip().effects()[0].id.value);
    REQUIRE(f.presets
                .savePreset({QString::fromStdString(section), QStringLiteral("motion")}, QStringLiteral("Mine"),
                            QStringLiteral("anchorToInPoint"), QString(), QString())
                .isEmpty());
    REQUIRE(commandOf(f.keys, "preset:Mine") != nullptr);
    REQUIRE(f.keys.assignShortcut(QStringLiteral("preset:Mine"), QStringLiteral("Ctrl+Alt+Shift+M")).value("ok").toBool());
    REQUIRE(f.keys.assignShortcut(QStringLiteral("fx:wabcut.blur.gaussian"), QStringLiteral("Ctrl+Alt+Shift+G")).value("ok").toBool());

    // Deleted: the key stays, the command is listed as unavailable and does nothing.
    REQUIRE(f.presets.deleteCustomItem(QVariantMap{{QStringLiteral("preset"), QStringLiteral("Mine")}}).isEmpty());
    const km::CommandInfo* gone = commandOf(f.keys, "preset:Mine");
    REQUIRE(gone != nullptr);
    CHECK(std::string(gone->displayName).find("(unavailable)") != std::string::npos);
    CHECK(gone->group == "Apply Preset");
    CHECK_FALSE(f.keys.keymap().chordsFor("preset:Mine").empty());
    CHECK(f.keys.shortcutText(QStringLiteral("preset:Mine")) == QStringLiteral("Ctrl+Alt+Shift+M"));
    f.controller.setSelection({f.b});
    const std::size_t before = f.undoCount();
    Applied applied(f.shortcuts);
    CHECK(f.keys.trigger(QStringLiteral("preset:Mine")));
    REQUIRE(applied.count() == 1);
    CHECK_FALSE(applied.take().second);
    CHECK(f.undoCount() == before);

    // A restart: a new dispatcher over the same overrides keeps both keys before anything registers them, then the
    // bridge gives the effect its real title back.
    KeymapDispatcher again;
    again.setOverridesPath(f.config.filePath(QStringLiteral("keymap/overrides.json")));
    again.setPlatform(QStringLiteral("windows"));
    CHECK(again.shortcutText(QStringLiteral("fx:wabcut.blur.gaussian")) == QStringLiteral("Ctrl+Alt+Shift+G"));
    REQUIRE(commandOf(again, "fx:wabcut.blur.gaussian") != nullptr);
    CHECK(std::string(commandOf(again, "fx:wabcut.blur.gaussian")->displayName).find("(unavailable)") != std::string::npos);
    EffectShortcutsBridge second;
    second.setController(&f.controller);
    second.setPresets(&f.presets);
    second.setDispatcher(&again);
    CHECK(commandOf(again, "fx:wabcut.blur.gaussian")->displayName == "Apply Gaussian Blur");
    CHECK(again.shortcutText(QStringLiteral("fx:wabcut.blur.gaussian")) == QStringLiteral("Ctrl+Alt+Shift+G"));
    CHECK(resolved(again, "Ctrl+Alt+Shift+G") == "fx:wabcut.blur.gaussian");
}

TEST_CASE("Effect shortcuts: the commands live beside the extensions' commands", "[app][effects][shortcuts][keymap]") {
    Fixture f;
    const int count = f.shortcuts.count();
    f.keys.setExtraCommands({KeymapDispatcher::ExtraCommand{QStringLiteral("ext:demo/hello"), QStringLiteral("Demo: Hello")}});
    // Both sources are listed; neither replaced the other.
    REQUIRE(commandOf(f.keys, "ext:demo/hello") != nullptr);
    CHECK(commandOf(f.keys, "ext:demo/hello")->group == "Extensions");
    CHECK(commandOf(f.keys, "fx:wabcut.blur.gaussian") != nullptr);
    CHECK(f.shortcuts.count() == count);
    f.shortcuts.refresh(); // the same list again changes nothing
    CHECK(commandOf(f.keys, "ext:demo/hello") != nullptr);
    f.keys.setExtraCommands({});
    CHECK(commandOf(f.keys, "ext:demo/hello") == nullptr);
    CHECK(commandOf(f.keys, "fx:wabcut.blur.gaussian") != nullptr);
    CHECK(commandOf(f.keys, "preset:Twirl In") != nullptr);
}
