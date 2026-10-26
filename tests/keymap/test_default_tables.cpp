// The built-in Premiere tables: embedding, validity, conflicts and the facts verified against Adobe's page
// (https://helpx.adobe.com/premiere/desktop/get-started/keyboard-shortcuts/default-keyboard-shortcuts.html,
// last updated 2026-01-07). See docs/keymap.md.

#include <wabcut/keymap/DefaultKeymaps.h>
#include <wabcut/keymap/KeymapManager.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

using namespace wabcut::keymap;

namespace {

const KeymapManager& keymap(Platform p) {
    static const KeymapManager windows = *KeymapManager::withDefaults(Platform::Windows);
    static const KeymapManager macos = *KeymapManager::withDefaults(Platform::MacOS);
    return p == Platform::MacOS ? macos : windows;
}

KeyChord chord(std::string_view text, Platform p) {
    auto c = parseKeyChord(text, p);
    INFO(text);
    REQUIRE(c.has_value());
    return *c;
}

/// The command `text` triggers with `scope` focused.
std::string resolved(Platform p, Scope scope, std::string_view text) {
    return keymap(p).resolve(scope, chord(text, p)).value_or("<none>");
}

/// The command's chords as canonical text.
std::vector<std::string> keysOf(Platform p, std::string_view command) {
    std::vector<std::string> out;
    for (const KeyChord& c : keymap(p).chordsFor(command)) {
        out.push_back(formatKeyChord(c, p));
    }
    return out;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

using Keys = std::vector<std::string>;

} // namespace

TEST_CASE("Embedded tables are byte-identical to the JSON sources", "[keymap][tables]") {
    const std::filesystem::path dir = WABCUT_KEYMAP_TABLE_DIR;
    CHECK(defaultKeymapJson(Platform::Windows) == readFile(dir / "premiere-windows.json"));
    CHECK(defaultKeymapJson(Platform::MacOS) == readFile(dir / "premiere-macos.json"));
    CHECK(defaultKeymapJson(Platform::Linux) == defaultKeymapJson(Platform::Windows)); // Linux uses Windows'
}

TEST_CASE("Both default tables load and every chord string is canonical", "[keymap][tables]") {
    const Platform p = GENERATE(Platform::Windows, Platform::MacOS);
    const auto doc = nlohmann::json::parse(defaultKeymapJson(p));
    CHECK(doc["platform"] == (p == Platform::MacOS ? "macos" : "windows"));
    std::size_t count = 0;
    for (const auto& b : doc["bindings"]) {
        INFO(b.dump());
        CHECK(doc["sources"].contains(b["source"].get<std::string>()));
        for (const auto& k : b["keys"]) {
            const std::string text = k.get<std::string>();
            CHECK(formatKeyChord(chord(text, p), p) == text);
            ++count;
        }
    }
    CHECK(count >= 200);
    CHECK(keymap(p).platform() == p);
    CHECK(keymap(p).bindings().size() == count);
    CHECK_FALSE(keymap(p).hasOverrides());
}

TEST_CASE("Linux uses the Windows table", "[keymap][tables]") {
    const auto linuxKeymap = KeymapManager::withDefaults(Platform::Linux);
    REQUIRE(linuxKeymap.has_value());
    CHECK(linuxKeymap->platform() == Platform::Linux);
    CHECK(linuxKeymap->resolve(Scope::Application, {Key_K, Modifier::Control}) == "sequence.addEdit");
    CHECK(linuxKeymap->bindings() == keymap(Platform::Windows).bindings());
}

TEST_CASE("Default tables have no conflicts", "[keymap][tables]") {
    CHECK(keymap(Platform::Windows).conflicts().empty());
    CHECK(keymap(Platform::MacOS).conflicts().empty());
    const auto doc = nlohmann::json::parse(defaultKeymapJson(Platform::MacOS));
    CHECK(doc["acknowledgedConflicts"].empty());
    // Adobe's page put a second command on macOS Cmd+T and Cmd+U; the shipped key set does not.
    CHECK(resolved(Platform::MacOS, Scope::Application, "Cmd+T") == "graphics.newTextLayer");
    CHECK(resolved(Platform::MacOS, Scope::Application, "Cmd+U") == "clip.makeSubclip");
}

TEST_CASE("Verified: I O X M , . ; ' F and Enter are identical on both OSes", "[keymap][tables][verified]") {
    const Platform p = GENERATE(Platform::Windows, Platform::MacOS);
    const std::pair<const char*, const char*> facts[] = {
        {"I", "marker.markIn"},       {"O", "marker.markOut"},
        {"X", "marker.markClip"},     {"M", "marker.add"},
        {",", "clip.insert"},         {".", "clip.overwrite"},
        {";", "sequence.lift"},       {"'", "sequence.extract"},
        {"F", "sequence.matchFrame"}, {"Enter", "sequence.renderInToOut"},
    };
    for (const auto& [key, command] : facts) {
        INFO(key << " -> " << command);
        CHECK(keysOf(p, command) == Keys{formatKeyChord(chord(key, Platform::Windows), p)});
        CHECK(resolved(p, Scope::Application, key) == command);
        CHECK(resolved(p, Scope::Timeline, key) == command); // Application shortcuts work in every panel
    }
    CHECK(keymap(Platform::Windows).chordsFor("sequence.renderInToOut") ==
          keymap(Platform::MacOS).chordsFor("sequence.renderInToOut"));
}

TEST_CASE("Verified: Clear In/Out/In-and-Out are Ctrl+Shift on Windows but Opt on macOS",
          "[keymap][tables][verified]") {
    CHECK(keysOf(Platform::Windows, "marker.clearIn") == Keys{"Ctrl+Shift+I"});
    CHECK(keysOf(Platform::Windows, "marker.clearOut") == Keys{"Ctrl+Shift+O"});
    CHECK(keysOf(Platform::Windows, "marker.clearInAndOut") == Keys{"Ctrl+Shift+X"});
    CHECK(keysOf(Platform::MacOS, "marker.clearIn") == Keys{"Opt+I"});
    CHECK(keysOf(Platform::MacOS, "marker.clearOut") == Keys{"Opt+O"});
    CHECK(keysOf(Platform::MacOS, "marker.clearInAndOut") == Keys{"Opt+X"});
    // Not a Ctrl<->Cmd swap: Cmd+Shift+I is not Clear In on macOS.
    CHECK(resolved(Platform::MacOS, Scope::Application, "Shift+Cmd+I") == "<none>");
    CHECK(resolved(Platform::MacOS, Scope::Application, "Opt+I") == "marker.clearIn");
}

TEST_CASE("Trim Edit and Make Subsequence follow the shipped key sets; Previous in Sequence differs per OS",
          "[keymap][tables][verified]") {
    // Lead decision (task K.1): follow Premiere's shipped default key sets, not the help page, which says
    // Cmd+T / Cmd+U on macOS (both already taken there by New Layer > Text and Make Subclip).
    for (const Platform p : {Platform::Windows, Platform::MacOS}) {
        INFO(platformId(p));
        CHECK(keysOf(p, "sequence.trimEdit") == Keys{"Shift+T"});
        CHECK(keysOf(p, "sequence.makeSubsequence") == Keys{"Shift+U"});
        CHECK(resolved(p, Scope::Application, "Shift+T") == "sequence.trimEdit");
        CHECK(resolved(p, Scope::Application, "Shift+U") == "sequence.makeSubsequence");
    }
    CHECK(keysOf(Platform::Windows, "sequence.previousInSequence") == Keys{"Ctrl+Shift+;"});
    CHECK(keysOf(Platform::MacOS, "sequence.previousInSequence") == Keys{"Opt+;"});
    CHECK(resolved(Platform::Windows, Scope::Application, "Ctrl+Shift+;") == "sequence.previousInSequence");
    CHECK(resolved(Platform::MacOS, Scope::Application, "Opt+;") == "sequence.previousInSequence");
}

TEST_CASE("Verified: Ctrl/Cmd+L is Link, but Loop in the Audio Track Mixer, Metadata and Effect Controls",
          "[keymap][tables][verified]") {
    const Platform p = GENERATE(Platform::Windows, Platform::MacOS);
    const char* key = p == Platform::MacOS ? "Cmd+L" : "Ctrl+L";
    CHECK(resolved(p, Scope::Application, key) == "clip.link");
    CHECK(resolved(p, Scope::Timeline, key) == "clip.link");
    CHECK(resolved(p, Scope::AudioMixer, key) == "audioMixer.loop");
    CHECK(resolved(p, Scope::Metadata, key) == "metadata.loop");
    CHECK(resolved(p, Scope::EffectControls, key) == "effectControls.loopAudioPlayback");
}

TEST_CASE("Verified: Ctrl/Cmd+R is Speed/Duration, but Show Rulers in the Program Monitor",
          "[keymap][tables][verified]") {
    const Platform p = GENERATE(Platform::Windows, Platform::MacOS);
    const char* key = p == Platform::MacOS ? "Cmd+R" : "Ctrl+R";
    CHECK(resolved(p, Scope::Application, key) == "clip.speedDuration");
    CHECK(resolved(p, Scope::Timeline, key) == "clip.speedDuration");
    CHECK(resolved(p, Scope::ProgramMonitor, key) == "programMonitor.showRulers");
}

TEST_CASE("Verified: Ripple Delete is Shift+Delete in the Edit menu, Alt/Opt+Backspace in the Timeline",
          "[keymap][tables][verified]") {
    // Edit menu: Shift + Delete (Windows) / Shift + Forward Delete (macOS) — both Qt::Key_Delete.
    CHECK(keysOf(Platform::Windows, "edit.rippleDelete") == Keys{"Shift+Delete"});
    CHECK(keysOf(Platform::MacOS, "edit.rippleDelete") == Keys{"Shift+Delete"});
    // Timeline panel: Alt + Backspace (Windows) / Opt + Delete (macOS's Delete key is Qt::Key_Backspace).
    CHECK(keysOf(Platform::Windows, "timeline.rippleDelete") == Keys{"Alt+Backspace"});
    CHECK(keysOf(Platform::MacOS, "timeline.rippleDelete") == Keys{"Opt+Backspace"});
    const Platform p = GENERATE(Platform::Windows, Platform::MacOS);
    const char* timelineKey = p == Platform::MacOS ? "Opt+Backspace" : "Alt+Backspace";
    CHECK(resolved(p, Scope::Timeline, timelineKey) == "timeline.rippleDelete");
    CHECK(resolved(p, Scope::Application, timelineKey) == "<none>");
    CHECK(resolved(p, Scope::Timeline, "Shift+Delete") == "edit.rippleDelete");
    CHECK(resolved(p, Scope::Project, "Shift+Delete") == "edit.rippleDelete");
}

TEST_CASE("Checked against Adobe: J K L C V Q W B N", "[keymap][tables][verified]") {
    // Not on the 2026 page; J/K/L/Q/W are on Adobe's 2024 page, C/V/B/N on Adobe's 2017 print pages, and all
    // nine match Premiere's default .kys files for both OSes (docs/keymap.md).
    const Platform p = GENERATE(Platform::Windows, Platform::MacOS);
    const std::pair<const char*, const char*> rows[] = {
        {"J", "transport.shuttleBackward"},
        {"K", "transport.shuttleStop"},
        {"L", "transport.shuttleForward"},
        {"C", "tool.razor"},
        {"V", "tool.selection"},
        {"Q", "sequence.rippleTrimPreviousToPlayhead"},
        {"W", "sequence.rippleTrimNextToPlayhead"},
        {"B", "tool.rippleEdit"},
        {"N", "tool.rollingEdit"},
    };
    for (const auto& [key, command] : rows) {
        INFO(key << " -> " << command);
        CHECK(keysOf(p, command) == Keys{key});
        CHECK(resolved(p, Scope::Application, key) == command);
    }
}

TEST_CASE("MVP shortcuts from the brief resolve on both OSes", "[keymap][tables]") {
    const Platform p = GENERATE(Platform::Windows, Platform::MacOS);
    const bool mac = p == Platform::MacOS;
    const std::pair<std::string, const char*> rows[] = {
        {"A", "tool.trackSelectForward"},
        {"Shift+A", "tool.trackSelectBackward"},
        {"R", "tool.rateStretch"},
        {"Y", "tool.slip"},
        {"U", "tool.slide"},
        {"P", "tool.pen"},
        {"H", "tool.hand"},
        {"Z", "tool.zoom"},
        {"T", "tool.type"},
        {"Space", "transport.playStop"},
        {"Left", "transport.stepBackward"},
        {"Right", "transport.stepForward"},
        {"Shift+Left", "transport.stepBackwardFive"},
        {"Shift+Right", "transport.stepForwardFive"},
        {"Up", "transport.goToPreviousEdit"},
        {"Down", "transport.goToNextEdit"},
        {"Home", "transport.goToStart"},
        {"End", "transport.goToEnd"},
        {"Shift+I", "marker.goToIn"},
        {"Shift+O", "marker.goToOut"},
        {"E", "sequence.extendEditToPlayhead"},
        {"D", "sequence.selectClipAtPlayhead"},
        {"S", "sequence.snap"},
        {"=", "sequence.zoomIn"},
        {"-", "sequence.zoomOut"},
        {"\\", "sequence.zoomToSequence"},
        {"Shift+D", "sequence.applyDefaultTransitions"},
        {"Shift+1", "window.projects"},
        {"Shift+2", "window.sourceMonitor"},
        {"Shift+3", "window.timeline"},
        {"Shift+4", "window.programMonitor"},
        {"Shift+5", "window.effectControls"},
        {"Shift+6", "window.audioTrackMixer"},
        {"Shift+7", "window.effects"},
        {"Shift+8", "window.mediaBrowser"},
        {"Shift+9", "window.audioClipMixer"},
        {mac ? "Cmd+K" : "Ctrl+K", "sequence.addEdit"},
        {mac ? "Shift+Cmd+K" : "Ctrl+Shift+K", "sequence.addEditToAllTracks"},
        {mac ? "Cmd+G" : "Ctrl+G", "clip.group"},
        {mac ? "Cmd+D" : "Ctrl+D", "sequence.applyVideoTransition"},
        {mac ? "Shift+Cmd+D" : "Ctrl+Shift+D", "sequence.applyAudioTransition"},
        {mac ? "Cmd+N" : "Ctrl+N", "file.newSequence"},
        {mac ? "Cmd+I" : "Ctrl+I", "file.import"},
        {mac ? "Cmd+M" : "Ctrl+M", "file.exportMedia"},
        {mac ? "Cmd+S" : "Ctrl+S", "file.save"},
        {mac ? "Shift+Cmd+S" : "Ctrl+Shift+S", "file.saveAs"},
        {mac ? "Cmd+Z" : "Ctrl+Z", "edit.undo"},
        {mac ? "Shift+Cmd+Z" : "Ctrl+Shift+Z", "edit.redo"},
        {mac ? "Cmd+X" : "Ctrl+X", "edit.cut"},
        {mac ? "Cmd+C" : "Ctrl+C", "edit.copy"},
        {mac ? "Cmd+V" : "Ctrl+V", "edit.paste"},
        {mac ? "Shift+Cmd+V" : "Ctrl+Shift+V", "edit.pasteInsert"},
        {mac ? "Cmd+A" : "Ctrl+A", "edit.selectAll"},
        {mac ? "Shift+Cmd+A" : "Ctrl+Shift+A", "edit.deselectAll"},
    };
    for (const auto& [key, command] : rows) {
        INFO(key << " -> " << command);
        CHECK(resolved(p, Scope::Application, key) == command);
    }
    // Nudge clip selection (Timeline panel): Alt+arrows on Windows, Cmd+arrows on macOS per Adobe's page.
    CHECK(resolved(p, Scope::Timeline, mac ? "Cmd+Left" : "Alt+Left") == "timeline.nudgeClipLeft");
    CHECK(resolved(p, Scope::Timeline, mac ? "Cmd+Right" : "Alt+Right") == "timeline.nudgeClipRight");
    CHECK(resolved(p, Scope::Timeline, mac ? "Shift+Cmd+Left" : "Alt+Shift+Left") ==
          "timeline.nudgeClipLeftFive");
}

TEST_CASE("macOS physical-Control shortcuts use MetaModifier", "[keymap][tables]") {
    // Audio Track Mixer > Meter Input(s) Only is "Ctrl + Shift + I" in Adobe's macOS column: the ⌃ key.
    CHECK(keymap(Platform::MacOS).chordsFor("audioMixer.meterInputsOnly") ==
          std::vector<KeyChord>{{Key_I, Modifier::Meta | Modifier::Shift}});
    CHECK(keymap(Platform::Windows).chordsFor("audioMixer.meterInputsOnly") ==
          std::vector<KeyChord>{{Key_I, Modifier::Control | Modifier::Shift}});
    CHECK(keymap(Platform::MacOS).shortcutText("audioMixer.meterInputsOnly") == "⌃⇧I");
    CHECK(keymap(Platform::MacOS).shortcutText("marker.clearIn") == "⌥I");
    CHECK(keymap(Platform::Windows).shortcutText("marker.clearIn") == "Ctrl+Shift+I");
}

TEST_CASE("Verified (M5j): trim and all-track height rows follow the shipped key sets; sequence tabs are Wabcut's",
          "[keymap][tables][verified]") {
    const Platform p = GENERATE(Platform::Windows, Platform::MacOS);
    const bool mac = p == Platform::MacOS;
    INFO((mac ? "macOS" : "Windows"));
    // The shipped key sets put these in context.global: Toggle Trim Type is Ctrl+Shift+T on Windows and Control+T on
    // macOS, so Shift+T stays Trim Edit everywhere, the Timeline included.
    CHECK(keysOf(p, "sequence.toggleTrimType") == Keys{mac ? "Ctrl+T" : "Ctrl+Shift+T"});
    CHECK(resolved(p, Scope::Application, mac ? "Ctrl+T" : "Ctrl+Shift+T") == "sequence.toggleTrimType");
    CHECK(resolved(p, Scope::Timeline, "Shift+T") == "sequence.trimEdit");
    // Trim Backward/Forward (Many): Ctrl+arrows on Windows, Opt+arrows on macOS, from any panel.
    CHECK(resolved(p, Scope::Application, mac ? "Opt+Left" : "Ctrl+Left") == "sequence.trimBackward");
    CHECK(resolved(p, Scope::Timeline, mac ? "Opt+Right" : "Ctrl+Right") == "sequence.trimForward");
    CHECK(resolved(p, Scope::Timeline, mac ? "Opt+Shift+Left" : "Ctrl+Shift+Left") == "sequence.trimBackwardMany");
    CHECK(resolved(p, Scope::Timeline, mac ? "Opt+Shift+Right" : "Ctrl+Shift+Right") == "sequence.trimForwardMany");
    // Slip keeps Ctrl+Alt / Opt+Cmd.
    CHECK(resolved(p, Scope::Timeline, mac ? "Opt+Cmd+Left" : "Ctrl+Alt+Left") == "timeline.slipClipLeft");
    // Expand All Tracks and Minimize All Tracks (context.global); the 2026 track height rows stay in the Timeline.
    CHECK(resolved(p, Scope::Application, "Shift+=") == "timeline.expandAllTracks");
    CHECK(resolved(p, Scope::Timeline, "Shift+-") == "timeline.minimizeAllTracks");
    CHECK(resolved(p, Scope::Timeline, mac ? "Cmd+=" : "Ctrl+=") == "timeline.increaseVideoTrackHeight");
    CHECK(resolved(p, Scope::Timeline, mac ? "Opt+-" : "Alt+-") == "timeline.decreaseAudioTrackHeight");
    // Wabcut: sequence tabs cycle with the physical Control key on both OSes (Cmd+Tab is the macOS app switcher).
    CHECK(resolved(p, Scope::Timeline, "Ctrl+Tab") == "timeline.nextSequenceTab");
    CHECK(resolved(p, Scope::Timeline, "Ctrl+Shift+Tab") == "timeline.previousSequenceTab");
    // Qt reports Ctrl+Shift+Tab as Ctrl+Shift+Backtab; the normalized chord resolves.
    const KeyChord backtab{Key_Backtab, Modifier::Shift | (mac ? Modifier::Meta : Modifier::Control)};
    CHECK(keymap(p).resolve(Scope::Timeline, backtab.normalized()).value_or("<none>") == "timeline.previousSequenceTab");
}

TEST_CASE("Verified (M5j): the trim and all-track height rows carry their .kys identities", "[keymap][registry][verified]") {
    // From the shipped key sets (docs/keymap.md, footnotes 13 and 15): every one is in context.global.
    const CommandRegistry& registry = CommandRegistry::builtin();
    const std::pair<const char*, const char*> rows[] = {
        {"cmd.sequence.toggletrimtype", "sequence.toggleTrimType"},
        {"cmd.sequence.trimbackward", "sequence.trimBackward"},
        {"cmd.sequence.trimforward", "sequence.trimForward"},
        {"cmd.sequence.trimbackwardmany", "sequence.trimBackwardMany"},
        {"cmd.sequence.trimforwardmany", "sequence.trimForwardMany"},
        {"cmd.timeline.expand.all.tracks", "timeline.expandAllTracks"},
        {"cmd.timeline.minimize.all.tracks", "timeline.minimizeAllTracks"},
    };
    for (const auto& [kys, id] : rows) {
        INFO(kys);
        const CommandInfo* c = registry.findKys("context.global", kys);
        REQUIRE(c != nullptr);
        CHECK(c->id == id);
        CHECK(c->scope == Scope::Application);
    }
    // Wabcut's own commands have no .kys identity.
    CHECK(registry.find("timeline.nextSequenceTab")->kysName.empty());
    CHECK(registry.find("timeline.previousSequenceTab")->kysName.empty());
}

TEST_CASE("Verified (M5l): the multi-camera keys follow the shipped key sets", "[keymap][tables][verified]") {
    const Platform p = GENERATE(Platform::Windows, Platform::MacOS);
    const bool mac = p == Platform::MacOS;
    INFO((mac ? "macOS" : "Windows"));
    // Premiere_default_keys.kys / Default Adobe.kys (context.global): Select Camera N is N, Cut to Camera N is
    // Ctrl+N on Windows (1-8: the Windows set has no Ctrl+9) and Control+N on macOS (1-9; "Ctrl" is the physical
    // Control key there), Toggle Multi-Camera View is Shift+0. The 2026 page lists none of them.
    for (int n = 1; n <= 9; ++n) {
        const std::string digit = std::to_string(n);
        INFO(n);
        CHECK(resolved(p, Scope::Application, digit) == "multicam.selectCamera" + digit);
        CHECK(resolved(p, Scope::Timeline, digit) == "multicam.selectCamera" + digit); // from any panel
        if (mac || n < 9)
            CHECK(keysOf(p, "multicam.cutToCamera" + digit) == Keys{"Ctrl+" + digit});
        else
            CHECK(keysOf(p, "multicam.cutToCamera9").empty());
    }
    CHECK(resolved(p, Scope::ProgramMonitor, "Shift+0") == "multicam.toggleView");
    // Their .kys identities, so a user's .kys import carries custom camera keys over.
    const CommandRegistry& registry = CommandRegistry::builtin();
    CHECK(registry.findKys("context.global", "cmd.multicam.choosenocut.camera3")->id == "multicam.selectCamera3");
    CHECK(registry.findKys("context.global", "cmd.multicam.choose.camera.7")->id == "multicam.cutToCamera7");
    CHECK(registry.findKys("context.global", "cmd.multicam.toggle.multicam.view")->id == "multicam.toggleView");
}

TEST_CASE("Verified (M5m): Remove Attributes has no default key, as in Premiere", "[keymap][tables][verified]") {
    const Platform p = GENERATE(Platform::Windows, Platform::MacOS);
    INFO((p == Platform::MacOS ? "macOS" : "Windows"));
    // Neither shipped key set lists an item for it (docs/keymap.md, footnote 19); Paste Attributes keeps its key.
    CHECK(keysOf(p, "edit.removeAttributes").empty());
    CHECK(keysOf(p, "edit.pasteAttributes") == Keys{p == Platform::MacOS ? "Opt+Cmd+V" : "Ctrl+Alt+V"});
    const CommandRegistry& registry = CommandRegistry::builtin();
    REQUIRE(registry.find("edit.removeAttributes") != nullptr);
    CHECK(registry.find("edit.removeAttributes")->kysName.empty());
}

TEST_CASE("Verified (UX rework): Settings is Cmd+Comma in the shipped macOS key set and Ctrl+Comma on Windows",
          "[keymap][tables][verified]") {
    // Default Adobe.kys (context.global) binds Preferences > General to Cmd+, (virtual key 0x8000002C with Command);
    // Premiere_default_keys.kys binds nothing there on Windows, so Ctrl+, is Wabcut's own (source "wabcut").
    CHECK(keysOf(Platform::Windows, "edit.preferences") == Keys{"Ctrl+,"});
    CHECK(keysOf(Platform::MacOS, "edit.preferences") == Keys{"Cmd+,"});
    for (const Platform p : {Platform::Windows, Platform::MacOS}) {
        INFO(platformId(p));
        const std::string key = p == Platform::MacOS ? "Cmd+," : "Ctrl+,";
        CHECK(resolved(p, Scope::Application, key) == "edit.preferences");
        CHECK(resolved(p, Scope::Timeline, key) == "edit.preferences"); // from any panel
        CHECK(resolved(p, Scope::Project, key) == "edit.preferences");
        CHECK(resolved(p, Scope::Application, ",") == "clip.insert"); // the plain key keeps Insert
    }
    const CommandRegistry& registry = CommandRegistry::builtin();
    const CommandInfo* settings = registry.find("edit.preferences");
    REQUIRE(settings != nullptr);
    CHECK(settings->scope == Scope::Application);
    CHECK(settings->group == "Edit");
    CHECK(registry.findKys("context.global", "cmd.edit.preferences.general") == settings);
}

TEST_CASE("Verified (0.4): new keys follow the shipped key sets", "[keymap][tables][verified]") {
    // Premiere_default_keys.kys (Windows) and Default Adobe.kys (macOS): Nudge Clip Selection Up / Down is Alt+Up /
    // Alt+Down in context.timeline (Opt on macOS, although Left / Right are Cmd+arrows there), the clip volume keys are
    // ] [ and Shift+] Shift+[ in context.global, Play from Playhead to Out is Control+Space (the physical Control key on
    // macOS too), and Ripple Sequence Markers has an identity but no key.
    struct Row {
        const char* id;
        const char* kysContext;
        const char* kysName;
        const char* windows; // "" = no default key
        const char* macos;
        Scope scope;
    };
    const Row rows[] = {
        {"timeline.nudgeClipUp", "context.timeline", "cmd.timeline.nudge.up", "Alt+Up", "Opt+Up", Scope::Timeline},
        {"timeline.nudgeClipDown", "context.timeline", "cmd.timeline.nudge.down", "Alt+Down", "Opt+Down", Scope::Timeline},
        {"clip.increaseVolume", "context.global", "cmd.sequence.increaseclipvolume", "]", "]", Scope::Application},
        {"clip.decreaseVolume", "context.global", "cmd.sequence.decreaseclipvolume", "[", "[", Scope::Application},
        {"clip.increaseVolumeMany", "context.global", "cmd.sequence.increaseclipvolumemany", "Shift+]", "Shift+]",
         Scope::Application},
        {"clip.decreaseVolumeMany", "context.global", "cmd.sequence.decreaseclipvolumemany", "Shift+[", "Shift+[",
         Scope::Application},
        {"transport.playFromPlayheadToOut", "context.global", "cmd.transport.play.ctitoout", "Ctrl+Space", "Ctrl+Space",
         Scope::Application},
        {"marker.toggleRipple", "context.global", "cmd.marker.style.ripplesequencemarkers", "", "", Scope::Application},
    };
    const CommandRegistry& registry = CommandRegistry::builtin();
    for (const Row& row : rows) {
        INFO(row.id);
        const CommandInfo* c = registry.find(row.id);
        REQUIRE(c != nullptr);
        CHECK(c->scope == row.scope);
        CHECK(registry.findKys(row.kysContext, row.kysName) == c);
        for (const Platform p : {Platform::Windows, Platform::MacOS}) {
            INFO(platformId(p));
            const std::string key = p == Platform::MacOS ? row.macos : row.windows;
            CHECK(keysOf(p, row.id) == (key.empty() ? Keys{} : Keys{key}));
            if (!key.empty()) {
                CHECK(resolved(p, row.scope, key) == row.id);
            }
        }
    }
    // macOS: Control, not Command (MetaModifier; Cmd+Space is Spotlight).
    CHECK(keymap(Platform::MacOS).chordsFor("transport.playFromPlayheadToOut") ==
          std::vector<KeyChord>{{Key_Space, Modifier::Meta}});
    // Wabcut's own commands for other streams have no identity and no key.
    for (const char* id : {"file.importFolder", "window.backgroundTasks"}) {
        INFO(id);
        REQUIRE(registry.find(id) != nullptr);
        CHECK(registry.find(id)->kysName.empty());
        CHECK(keysOf(Platform::Windows, id).empty());
        CHECK(keysOf(Platform::MacOS, id).empty());
    }
    // The slow shuttle, Play Around and screen paging keep the keys they had (handled from 0.4).
    CHECK(keysOf(Platform::Windows, "transport.shuttleSlowForward") == Keys{"Shift+L"});
    CHECK(keysOf(Platform::Windows, "transport.playAround") == Keys{"Shift+K"});
    CHECK(keysOf(Platform::Windows, "timeline.showNextScreen") == Keys{"PageDown"});
}

TEST_CASE("Bracket keys change clip volume while the Project panel keeps Shift+] and Shift+[", "[keymap][tables]") {
    for (const Platform p : {Platform::Windows, Platform::MacOS}) {
        INFO(platformId(p));
        CHECK(resolved(p, Scope::Timeline, "]") == "clip.increaseVolume");
        CHECK(resolved(p, Scope::Timeline, "[") == "clip.decreaseVolume");
        CHECK(resolved(p, Scope::Timeline, "Shift+]") == "clip.increaseVolumeMany");
        CHECK(resolved(p, Scope::ProgramMonitor, "Shift+[") == "clip.decreaseVolumeMany");
        // The Library (Project panel) shadows the Application keys with Premiere's thumbnail size keys.
        CHECK(resolved(p, Scope::Project, "Shift+]") == "project.thumbnailSizeNext");
        CHECK(resolved(p, Scope::Project, "Shift+[") == "project.thumbnailSizePrevious");
        CHECK(resolved(p, Scope::Project, "]") == "clip.increaseVolume");
        // Alt/Opt+Up next to them in the Timeline is Nudge Clip Selection Up.
        CHECK(resolved(p, Scope::Timeline, p == Platform::MacOS ? "Opt+Up" : "Alt+Up") == "timeline.nudgeClipUp");
    }
    CHECK(keymap(Platform::Windows).conflicts().empty());
    CHECK(keymap(Platform::MacOS).conflicts().empty());
}

TEST_CASE("Verified (0.5): Toggle Audio Scrubbing is Shift+S and Audio Meters has no key", "[keymap][tables][verified]") {
    // Premiere_default_keys.kys and Default Adobe.kys: cmd.toggle.audio.scrubbing is Shift+S (virtual key 0x80000053
    // with Shift) in context.global on both OSes; uif.window.Audio Meter is listed (macOS) with no key.
    const CommandRegistry& registry = CommandRegistry::builtin();
    const CommandInfo* scrub = registry.find("transport.toggleAudioScrubbing");
    const CommandInfo* meters = registry.find("window.audioMeters");
    REQUIRE(scrub != nullptr);
    REQUIRE(meters != nullptr);
    CHECK(scrub->scope == Scope::Application);
    CHECK(meters->scope == Scope::Application);
    CHECK(registry.findKys("context.global", "cmd.toggle.audio.scrubbing") == scrub);
    CHECK(registry.findKys("context.global", "uif.window.Audio Meter") == meters);
    for (const Platform p : {Platform::Windows, Platform::MacOS}) {
        INFO(platformId(p));
        CHECK(keysOf(p, "transport.toggleAudioScrubbing") == Keys{"Shift+S"});
        CHECK(resolved(p, Scope::Application, "Shift+S") == "transport.toggleAudioScrubbing");
        CHECK(resolved(p, Scope::Timeline, "Shift+S") == "transport.toggleAudioScrubbing"); // from any panel
        CHECK(resolved(p, Scope::Application, "S") == "sequence.snap");                     // S alone stays Snap
        CHECK(keysOf(p, "window.audioMeters").empty());
    }
}
