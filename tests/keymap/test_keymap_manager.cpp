#include <wabcut/keymap/KeymapManager.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <string>

using namespace wabcut;
using namespace wabcut::keymap;

namespace {

constexpr Modifiers kCtrl = Modifier::Control;
constexpr Modifiers kAlt = Modifier::Alt;
constexpr Modifiers kShift = Modifier::Shift;

// A small custom registry keeps these tests independent of the shipped tables.
constexpr CommandInfo kTestCommands[] = {
    {"clip.link", Scope::Application, "Clip", "Link", "Link", "context.global", "cmd.clip.linkaudioandvideo",
     false},
    {"audioMixer.loop", Scope::AudioMixer, "Audio Track Mixer", "Loop", "Loop", "", "", false},
    {"edit.undo", Scope::Application, "Edit", "Undo", "Undo", "", "", true},
    {"edit.redo", Scope::Application, "Edit", "Redo", "Redo", "", "", true},
    {"tool.razor", Scope::Application, "Tools", "Razor Tool", "Razor Tool", "", "", false},
    {"marker.markClip", Scope::Application, "Markers", "Mark Clip", "Mark Clip", "", "", false},
    {"marker.markIn", Scope::Application, "Markers", "Mark In", "Mark In", "", "", false},
    {"sequence.lift", Scope::Application, "Sequence", "Lift", "Lift", "", "", false},
    {"sequence.nextInSequence", Scope::Application, "Sequence", "Next in Sequence", "Next in Sequence", "",
     "", false},
    {"window.projects", Scope::Application, "Window", "Projects", "Projects", "", "", false},
    {"project.previousColumnField", Scope::Project, "Project", "Previous Column Field", "", "", "", false},
    {"sequence.renderInToOut", Scope::Application, "Sequence", "Render", "", "", "", false},
    {"timeline.clearSelection", Scope::Timeline, "Timeline", "Clear Selection", "", "", "", false},
    {"timeline.rippleDelete", Scope::Timeline, "Timeline", "Ripple Delete", "", "", "", false},
    {"marker.add", Scope::Application, "Markers", "Add Marker", "", "", "", false},
    {"graphics.newTextLayer", Scope::Application, "Graphics", "Text", "", "", "", false},
    {"sequence.trimEdit", Scope::Application, "Sequence", "Trim Edit", "", "", "", false},
    {"transport.stepForward", Scope::Application, "Playback", "Step Forward", "", "", "", true},
};

const CommandRegistry& testRegistry() {
    static const CommandRegistry registry{kTestCommands};
    return registry;
}

constexpr std::string_view kWindowsTable = R"({
  "schemaVersion": 1,
  "name": "test table",
  "platform": "windows",
  "bindings": [
    {"scope": "application", "command": "clip.link", "keys": ["Ctrl+L"]},
    {"scope": "audioMixer", "command": "audioMixer.loop", "keys": ["Ctrl+L"]},
    {"scope": "application", "command": "edit.undo", "keys": ["Ctrl+Z"]},
    {"scope": "application", "command": "edit.redo", "keys": ["Ctrl+Shift+Z", "Ctrl+Y"]},
    {"scope": "application", "command": "tool.razor", "keys": ["C"]},
    {"scope": "application", "command": "marker.markClip", "keys": ["X"]},
    {"scope": "application", "command": "marker.markIn", "keys": ["I"]},
    {"scope": "application", "command": "sequence.lift", "keys": [";"]},
    {"scope": "application", "command": "sequence.nextInSequence", "keys": ["Shift+;"]},
    {"scope": "application", "command": "window.projects", "keys": ["Shift+1"]},
    {"scope": "project", "command": "project.previousColumnField", "keys": ["Shift+Tab"]},
    {"scope": "application", "command": "sequence.renderInToOut", "keys": ["Enter"]},
    {"scope": "timeline", "command": "timeline.clearSelection", "keys": ["Backspace"]},
    {"scope": "timeline", "command": "timeline.rippleDelete", "keys": ["Alt+Backspace"]},
    {"scope": "application", "command": "marker.add", "keys": ["M"]},
    {"scope": "application", "command": "graphics.newTextLayer", "keys": ["Ctrl+T"]},
    {"scope": "application", "command": "sequence.trimEdit", "keys": ["Ctrl+T"]},
    {"scope": "application", "command": "transport.stepForward", "keys": ["Right"]}
  ],
  "acknowledgedConflicts": [
    {"scope": "application", "keys": "Ctrl+T", "commands": ["sequence.trimEdit", "graphics.newTextLayer"],
     "note": "winner listed first, against table order"}
  ]
})";

KeymapManager makeKeymap() {
    KeymapManager km(Platform::Windows, testRegistry());
    const auto loaded = km.load(kWindowsTable, Platform::Windows);
    INFO((loaded ? std::string() : loaded.error().message));
    REQUIRE(loaded.has_value());
    return km;
}

std::string resolveText(const KeymapManager& km, Scope scope, KeyChord chord) {
    return km.resolve(scope, chord).value_or("<none>");
}

/// Replaces `from` with `to` once in the test table.
std::string tableWith(std::string_view from, std::string_view to) {
    std::string s(kWindowsTable);
    const auto pos = s.find(from);
    REQUIRE(pos != std::string::npos);
    s.replace(pos, from.size(), to);
    return s;
}

} // namespace

TEST_CASE("The focused panel scope wins, then Application", "[keymap][manager]") {
    const KeymapManager km = makeKeymap();
    const KeyChord ctrlL{Key_L, kCtrl};
    CHECK(resolveText(km, Scope::AudioMixer, ctrlL) == "audioMixer.loop");
    CHECK(resolveText(km, Scope::Application, ctrlL) == "clip.link");
    CHECK(resolveText(km, Scope::Timeline, ctrlL) == "clip.link"); // Timeline has no Ctrl+L of its own
    CHECK(resolveText(km, Scope::AudioMixer, {Key_Z, kCtrl}) == "edit.undo"); // falls through to Application
    // Panel shortcuts never fire outside their panel.
    CHECK(resolveText(km, Scope::Application, {Key_Backspace, kAlt}) == "<none>");
    CHECK(resolveText(km, Scope::Project, {Key_Backspace, kAlt}) == "<none>");
    CHECK(resolveText(km, Scope::Timeline, {Key_Backspace, kAlt}) == "timeline.rippleDelete");
    CHECK(resolveText(km, Scope::Timeline, {Key_Backspace}) == "timeline.clearSelection");
    // Unbound chords and bare modifier keys resolve to nothing.
    CHECK(resolveText(km, Scope::Timeline, {Key_Q}) == "<none>");
    CHECK(resolveText(km, Scope::Timeline, {Key_Shift, kShift}) == "<none>");
    CHECK(km.resolveCommand(Scope::AudioMixer, ctrlL)->displayName == "Loop");
}

TEST_CASE("Chords straight from Qt key events resolve", "[keymap][manager]") {
    const KeymapManager km = makeKeymap();
    // Shift+; and Shift+1 arrive as the shifted symbol.
    CHECK(resolveText(km, Scope::Timeline, KeyChord::fromQt(Key_Colon, 0x02000000)) ==
          "sequence.nextInSequence");
    CHECK(resolveText(km, Scope::Timeline, KeyChord::fromQt(Key_Exclam, 0x02000000)) == "window.projects");
    CHECK(resolveText(km, Scope::Timeline, KeyChord::fromQt(Key_Semicolon, 0)) == "sequence.lift");
    // Shift+Tab arrives as Backtab.
    CHECK(resolveText(km, Scope::Project, KeyChord::fromQt(Key_Backtab, 0x02000000)) ==
          "project.previousColumnField");
    // macOS sets KeypadModifier on arrow keys; keypad digits/Enter fall back to the main-keyboard binding.
    CHECK(resolveText(km, Scope::Timeline, KeyChord::fromQt(Key_Right, 0x20000000)) ==
          "transport.stepForward");
    CHECK(resolveText(km, Scope::Timeline, KeyChord::fromQt(Key_Enter, 0x20000000)) ==
          "sequence.renderInToOut");
    CHECK(resolveText(km, Scope::Timeline, KeyChord::fromQt(Key_Return, 0)) == "sequence.renderInToOut");
    // GroupSwitch (AltGr on X11) is ignored.
    CHECK(resolveText(km, Scope::Timeline, KeyChord::fromQt(Key_M, 0x40000000)) == "marker.add");
}

TEST_CASE("Keypad-specific bindings beat the main-keyboard fallback", "[keymap][manager]") {
    KeymapManager km = makeKeymap();
    REQUIRE(km.setChords("marker.add", {KeyChord{Key_M}, KeyChord{Key_5, Modifier::Keypad}}));
    REQUIRE(km.setChords("marker.markIn", {KeyChord{Key_5}}));
    CHECK(resolveText(km, Scope::Timeline, {Key_5, Modifier::Keypad}) == "marker.add");
    CHECK(resolveText(km, Scope::Timeline, {Key_5}) == "marker.markIn");
}

TEST_CASE("Conflicts are detected per scope, and acknowledged ones pick their winner", "[keymap][manager]") {
    KeymapManager km = makeKeymap();
    auto conflicts = km.conflicts();
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].scope == Scope::Application);
    CHECK(conflicts[0].chord == KeyChord{Key_T, kCtrl});
    CHECK(conflicts[0].acknowledged);
    CHECK(conflicts[0].commands == std::vector<CommandId>{"sequence.trimEdit", "graphics.newTextLayer"});
    CHECK(resolveText(km, Scope::Application, {Key_T, kCtrl}) == "sequence.trimEdit");

    // Ctrl+L in two different scopes is shadowing, not a conflict.
    for (const Conflict& c : conflicts) {
        CHECK(c.chord != KeyChord{Key_L, kCtrl});
    }

    // A user override that collides with a default is an unacknowledged conflict, and the user's binding
    // wins.
    REQUIRE(km.setChords("tool.razor", {KeyChord{Key_X}}));
    conflicts = km.conflicts();
    REQUIRE(conflicts.size() == 2);
    const Conflict& x = conflicts[0].chord == KeyChord{Key_X} ? conflicts[0] : conflicts[1];
    CHECK_FALSE(x.acknowledged);
    CHECK(x.commands == std::vector<CommandId>{"tool.razor", "marker.markClip"});
    CHECK(resolveText(km, Scope::Application, {Key_X}) == "tool.razor");

    // Overriding one side of an acknowledged conflict changes the command set: no longer acknowledged.
    REQUIRE(km.setChords("tool.razor", {KeyChord{Key_C}}));
    REQUIRE(km.setChords("marker.add", {KeyChord{Key_T, kCtrl}}));
    conflicts = km.conflicts();
    REQUIRE(conflicts.size() == 1);
    CHECK_FALSE(conflicts[0].acknowledged);
    CHECK(conflicts[0].commands.front() == "marker.add");
}

TEST_CASE("User overrides layer over the defaults", "[keymap][manager]") {
    KeymapManager km = makeKeymap();
    CHECK(km.chordsFor("edit.redo") == std::vector<KeyChord>{{Key_Z, kCtrl | kShift}, {Key_Y, kCtrl}});
    CHECK(km.shortcutText("edit.redo") == "Ctrl+Shift+Z");

    // Replace.
    REQUIRE(km.setChords("tool.razor", {KeyChord{Key_R, kAlt}}));
    CHECK(km.isOverridden("tool.razor"));
    CHECK(resolveText(km, Scope::Application, {Key_R, kAlt}) == "tool.razor");
    CHECK(resolveText(km, Scope::Application, {Key_C}) == "<none>");
    CHECK(km.defaultChordsFor("tool.razor") == std::vector<KeyChord>{KeyChord{Key_C}});

    // Unbind.
    REQUIRE(km.setChords("marker.markIn", {}));
    CHECK(km.chordsFor("marker.markIn").empty());
    CHECK(km.shortcutText("marker.markIn").empty());
    CHECK(resolveText(km, Scope::Application, {Key_I}) == "<none>");

    // Setting the defaults back removes the override, keeping the diff minimal.
    REQUIRE(km.setChords("tool.razor", {KeyChord{Key_C}}));
    CHECK_FALSE(km.isOverridden("tool.razor"));

    // Reset.
    km.resetToDefault("marker.markIn");
    CHECK(resolveText(km, Scope::Application, {Key_I}) == "marker.markIn");
    REQUIRE(km.setChords("edit.undo", {KeyChord{Key_U, kCtrl}}));
    km.resetAllToDefaults();
    CHECK_FALSE(km.hasOverrides());
    CHECK(resolveText(km, Scope::Application, {Key_Z, kCtrl}) == "edit.undo");

    // Errors leave everything unchanged.
    CHECK(km.setChords("no.such", {KeyChord{Key_Q}}).error().code == ErrorCode::NotFound);
    CHECK(km.setChords("edit.undo", {KeyChord{Key_Shift}}).error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(km.hasOverrides());
}

TEST_CASE("assign() takes the chord from other commands in the same scope, like Premiere",
          "[keymap][manager]") {
    KeymapManager km = makeKeymap();
    const auto lost = km.assign("tool.razor", KeyChord{Key_X});
    REQUIRE(lost.has_value());
    CHECK(*lost == std::vector<CommandId>{"marker.markClip"});
    CHECK(km.chordsFor("tool.razor") == std::vector<KeyChord>{KeyChord{Key_C}, KeyChord{Key_X}});
    CHECK(km.chordsFor("marker.markClip").empty());
    CHECK(km.conflicts().size() == 1); // only the acknowledged Ctrl+T one remains
    CHECK(resolveText(km, Scope::Application, {Key_X}) == "tool.razor");

    // Other scopes keep their bindings: Ctrl+L assigned in Application leaves the mixer's Loop alone.
    REQUIRE(km.assign("edit.undo", KeyChord{Key_L, kCtrl}));
    CHECK(km.chordsFor("clip.link").empty());
    CHECK(resolveText(km, Scope::AudioMixer, {Key_L, kCtrl}) == "audioMixer.loop");

    REQUIRE(km.unassign("tool.razor", KeyChord{Key_C}));
    CHECK(km.chordsFor("tool.razor") == std::vector<KeyChord>{KeyChord{Key_X}});
    REQUIRE(km.unassign("tool.razor", KeyChord{Key_F1})); // not bound: no-op
    CHECK(km.assign("no.such", KeyChord{Key_Q}).error().code == ErrorCode::NotFound);
}

TEST_CASE("Overrides save as a small JSON diff and load back", "[keymap][manager]") {
    KeymapManager km = makeKeymap();
    REQUIRE(km.setChords("tool.razor", {KeyChord{Key_X, kAlt}, KeyChord{Key_Plus, kCtrl}}));
    REQUIRE(km.setChords("marker.markIn", {}));
    const std::string saved = km.saveOverrides();
    CHECK(saved.find("\"tool.razor\"") != std::string::npos);
    CHECK(saved.find("\"Alt+X\"") != std::string::npos);
    CHECK(saved.find("\"Ctrl++\"") != std::string::npos);
    CHECK(saved.find("edit.undo") == std::string::npos); // untouched commands are not written

    KeymapManager other = makeKeymap();
    const auto report = other.loadOverrides(saved);
    REQUIRE(report.has_value());
    CHECK(report->warnings.empty());
    CHECK(other.overrides() == km.overrides());
    CHECK(other.bindings() == km.bindings());
    CHECK(other.saveOverrides() == saved);

    // Loading replaces existing overrides.
    REQUIRE(other.loadOverrides(R"({"schemaVersion":1,"platform":"windows","overrides":{}})"));
    CHECK_FALSE(other.hasOverrides());

    // Linux and Windows share chord text; macOS does not.
    KeymapManager linuxKeymap(Platform::Linux, testRegistry());
    REQUIRE(linuxKeymap.load(kWindowsTable, Platform::Linux));
    CHECK(linuxKeymap.loadOverrides(saved).has_value());
    KeymapManager mac(Platform::MacOS, testRegistry());
    CHECK(mac.loadOverrides(saved).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Loading overrides skips unknown commands and rejects malformed files", "[keymap][manager]") {
    KeymapManager km = makeKeymap();
    REQUIRE(km.setChords("edit.undo", {KeyChord{Key_U, kCtrl}}));
    const auto before = km.overrides();

    const auto report = km.loadOverrides(
        R"({"schemaVersion":1,"platform":"windows","overrides":{"from.future":["F5"],"tool.razor":["Shift+C"]}})");
    REQUIRE(report.has_value());
    REQUIRE(report->warnings.size() == 1);
    CHECK(report->warnings[0].find("from.future") != std::string::npos);
    CHECK(km.chordsFor("tool.razor") == std::vector<KeyChord>{{Key_C, kShift}});
    CHECK_FALSE(km.isOverridden("edit.undo"));

    REQUIRE(km.setChords("edit.undo", {KeyChord{Key_U, kCtrl}}));
    const auto snapshot = km.overrides();
    for (std::string_view bad : {
             R"(not json)",
             R"([1,2])",
             R"({"platform":"windows","overrides":{}})",
             R"({"schemaVersion":2,"platform":"windows","overrides":{}})",
             R"({"schemaVersion":1,"overrides":{}})",
             R"({"schemaVersion":1,"platform":"windows","overrides":[]})",
             R"({"schemaVersion":1,"platform":"windows","overrides":{"tool.razor":"C"}})",
             R"({"schemaVersion":1,"platform":"windows","overrides":{"tool.razor":[42]}})",
             R"({"schemaVersion":1,"platform":"windows","overrides":{"tool.razor":["Cmd+C"]}})",
         }) {
        INFO(bad);
        CHECK_FALSE(km.loadOverrides(bad).has_value());
        CHECK(km.overrides() == snapshot); // untouched
    }
    CHECK(km.loadOverrides(R"({"schemaVersion":2,"platform":"windows","overrides":{}})").error().code ==
          ErrorCode::VersionMismatch);
}

TEST_CASE("applyOverrides is atomic", "[keymap][manager]") {
    KeymapManager km = makeKeymap();
    const std::vector<CommandOverride> good{{"tool.razor", {KeyChord{Key_R}}}, {"marker.markIn", {}}};
    REQUIRE(km.applyOverrides(good));
    CHECK(km.overrides().size() == 2);
    const std::vector<CommandOverride> bad{{"edit.undo", {KeyChord{Key_U}}}, {"no.such", {}}};
    CHECK(km.applyOverrides(bad).error().code == ErrorCode::NotFound);
    CHECK_FALSE(km.isOverridden("edit.undo"));
}

TEST_CASE("Default tables are validated strictly and load atomically", "[keymap][manager]") {
    KeymapManager km = makeKeymap();
    REQUIRE(km.setChords("tool.razor", {KeyChord{Key_R}}));
    const auto bindingsBefore = km.bindings();
    const std::pair<std::string, ErrorCode> cases[] = {
        {"{", ErrorCode::ParseError},
        {tableWith(R"("schemaVersion": 1)", R"("schemaVersion": 7)"), ErrorCode::VersionMismatch},
        {tableWith(R"("platform": "windows")", R"("platform": "amiga")"), ErrorCode::ParseError},
        {tableWith(R"("command": "tool.razor")", R"("command": "tool.typo")"), ErrorCode::NotFound},
        {tableWith(R"("scope": "audioMixer")", R"("scope": "application")"), ErrorCode::InvalidArgument},
        {tableWith(R"("scope": "audioMixer")", R"("scope": "mixer")"), ErrorCode::ParseError},
        {tableWith(R"("keys": ["C"])", R"("keys": ["Cmd+C"])"), ErrorCode::ParseError},
        {tableWith(R"("keys": ["C"])", R"("keys": "C")"), ErrorCode::ParseError},
        {tableWith(R"("command": "marker.markClip")", R"("command": "tool.razor")"),
         ErrorCode::InvalidArgument},
        // Acknowledgements must match a real conflict.
        {tableWith(R"("keys": "Ctrl+T")", R"("keys": "Ctrl+Y")"), ErrorCode::InvalidArgument},
        {tableWith(R"(["sequence.trimEdit", "graphics.newTextLayer"])", R"(["sequence.trimEdit"])"),
         ErrorCode::InvalidArgument},
    };
    for (const auto& [json, code] : cases) {
        INFO(json.substr(0, 120));
        const auto r = km.load(json, Platform::Windows);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == code);
        CHECK(km.bindings() == bindingsBefore);
    }
    // A Windows table cannot be loaded as a macOS keymap.
    CHECK(km.load(kWindowsTable, Platform::MacOS).error().code == ErrorCode::InvalidArgument);
    // load(json) adopts the table's platform.
    KeymapManager adopt(Platform::MacOS, testRegistry());
    REQUIRE(adopt.load(kWindowsTable));
    CHECK(adopt.platform() == Platform::Windows);
    CHECK(adopt.tableName() == "test table");
}

TEST_CASE("bindings() lists effective bindings in registry order", "[keymap][manager]") {
    KeymapManager km = makeKeymap();
    REQUIRE(km.setChords("edit.undo", {KeyChord{Key_U, kCtrl}}));
    const auto all = km.bindings();
    REQUIRE_FALSE(all.empty());
    CHECK(all.front().command == "clip.link");
    const auto undo = std::ranges::find(all, CommandId("edit.undo"), &Binding::command);
    REQUIRE(undo != all.end());
    CHECK(undo->overridden);
    CHECK(undo->chord == KeyChord{Key_U, kCtrl});
    CHECK(std::ranges::count(all, CommandId("edit.redo"), &Binding::command) == 2);
}

TEST_CASE("Overriding one side of an acknowledged conflict keeps the table's winner", "[keymap][manager]") {
    KeymapManager km = makeKeymap();
    // The loser gains a second chord: it is overridden, but Ctrl+T is still one of its defaults.
    REQUIRE(km.assign("sequence.trimEdit", KeyChord{Key_T, kShift}));
    REQUIRE(km.isOverridden("sequence.trimEdit"));
    CHECK(resolveText(km, Scope::Application, {Key_T, kCtrl}) == "sequence.trimEdit"); // table winner kept
    auto conflicts = km.conflicts();
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].acknowledged);
    // The winner overridden the same way: still the same order.
    REQUIRE(km.assign("graphics.newTextLayer", KeyChord{Key_N, kCtrl | kShift}));
    CHECK(resolveText(km, Scope::Application, {Key_T, kCtrl}) == "sequence.trimEdit");
    CHECK(km.conflicts()[0].acknowledged);
}

TEST_CASE("Chords captured from Qt are stored in their canonical form", "[keymap][manager]") {
    KeymapManager km = makeKeymap();
    // A shortcut editor captures Shift+; as Shift+Key_Colon.
    const auto lost = km.assign("tool.razor", KeyChord::fromQt(Key_Colon, 0x02000000));
    REQUIRE(lost.has_value());
    CHECK(*lost == std::vector<CommandId>{"sequence.nextInSequence"});
    CHECK(km.chordsFor("tool.razor") ==
          std::vector<KeyChord>{KeyChord{Key_C}, KeyChord{Key_Semicolon, kShift}});
    CHECK(resolveText(km, Scope::Timeline, KeyChord::fromQt(Key_Colon, 0x02000000)) == "tool.razor");
    CHECK(km.saveOverrides().find("\"Shift+;\"") != std::string::npos);
    REQUIRE(km.setChords("marker.add", {KeyChord::fromQt(Key_Backtab, 0x02000000)}));
    CHECK(km.chordsFor("marker.add") == std::vector<KeyChord>{{Key_Tab, kShift}});
    REQUIRE(km.unassign("tool.razor", KeyChord::fromQt(Key_Colon, 0x02000000)));
    CHECK(km.chordsFor("tool.razor") == std::vector<KeyChord>{KeyChord{Key_C}});
}

TEST_CASE("An exact chord in Application beats a fallback in the focused panel", "[keymap][manager]") {
    KeymapManager km = makeKeymap();
    REQUIRE(km.setChords("marker.add", {KeyChord{Key_1, Modifier::Keypad}})); // Application, keypad 1
    REQUIRE(km.setChords("project.previousColumnField", {KeyChord{Key_1}}));  // Project, main-row 1
    CHECK(resolveText(km, Scope::Project, {Key_1, Modifier::Keypad}) == "marker.add");
    CHECK(resolveText(km, Scope::Project, {Key_1}) == "project.previousColumnField");
    // Keypad digits never fall back to the main row (Premiere uses them for timecode entry)...
    REQUIRE(km.setChords("marker.add", {KeyChord{Key_M}}));
    CHECK(resolveText(km, Scope::Project, {Key_1, Modifier::Keypad}) == "<none>");
    CHECK(resolveText(km, Scope::Timeline, {Key_Plus, Modifier::Keypad}) == "<none>");
    // ...but keypad navigation keys do, and an exact keypad binding still wins across scopes.
    REQUIRE(km.setChords("marker.add", {KeyChord{Key_Home, Modifier::Keypad}}));
    REQUIRE(km.setChords("project.previousColumnField", {KeyChord{Key_Home}}));
    CHECK(resolveText(km, Scope::Project, {Key_Home, Modifier::Keypad}) == "marker.add");
    REQUIRE(km.setChords("marker.add", {KeyChord{Key_M}}));
    CHECK(resolveText(km, Scope::Project, {Key_Home, Modifier::Keypad}) == "project.previousColumnField");
    CHECK(resolveText(km, Scope::Timeline, {Key_Delete, Modifier::Keypad}) == "<none>");
}

TEST_CASE("Overrides saved under the renamed trim command ids still load (review P2-45)", "[keymap][manager]") {
    auto km = KeymapManager::withDefaults(Platform::Windows);
    REQUIRE(km.has_value());
    const auto report = km->loadOverrides(R"({"schemaVersion":1,"platform":"windows","overrides":{
        "timeline.toggleTrimType":["Ctrl+Alt+Shift+F7"], "timeline.trimBackward":["Ctrl+Alt+Shift+F8"],
        "timeline.trimForward":["Ctrl+Alt+Shift+F9"], "timeline.trimBackwardMany":["Ctrl+Alt+Shift+F10"],
        "timeline.trimForwardMany":["Ctrl+Alt+Shift+F11"]}})");
    REQUIRE(report.has_value());
    CHECK(report->warnings.empty());
    for (const char* id : {"sequence.toggleTrimType", "sequence.trimBackward", "sequence.trimForward",
                           "sequence.trimBackwardMany", "sequence.trimForwardMany"}) {
        INFO(id);
        CHECK(km->isOverridden(id));
        CHECK(km->chordsFor(id).size() == 1);
    }
    // Saved again under the new ids.
    const std::string saved = km->saveOverrides();
    CHECK(saved.find("sequence.toggleTrimType") != std::string::npos);
    CHECK(saved.find("timeline.toggleTrimType") == std::string::npos);
}
