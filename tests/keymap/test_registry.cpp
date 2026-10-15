#include <wabcut/keymap/CommandRegistry.h>

#include <catch2/catch_test_macros.hpp>
#include <set>
#include <string>

using namespace wabcut::keymap;

TEST_CASE("Built-in command ids are unique, dotted and indexed", "[keymap][registry]") {
    const CommandRegistry& registry = CommandRegistry::builtin();
    REQUIRE(registry.size() >= 200);
    std::set<std::string_view> ids;
    for (const CommandInfo& c : registry.all()) {
        INFO(c.id);
        CHECK(ids.insert(c.id).second);
        const auto dot = c.id.find('.');
        CHECK(dot != std::string_view::npos);
        CHECK(dot > 0);
        CHECK(dot + 1 < c.id.size());
        CHECK_FALSE(c.displayName.empty());
        CHECK_FALSE(c.group.empty());
        CHECK(registry.find(c.id) == &c);
        CHECK(c.kysContext.empty() == c.kysName.empty());
    }
    CHECK(registry.find("no.such.command") == nullptr);
}

TEST_CASE("Premiere .kys identities map to exactly one command", "[keymap][registry]") {
    const CommandRegistry& registry = CommandRegistry::builtin();
    std::set<std::pair<std::string_view, std::string_view>> seen;
    for (const CommandInfo& c : registry.all()) {
        if (c.kysName.empty()) {
            continue;
        }
        INFO(c.id);
        CHECK(c.kysContext.starts_with("context."));
        CHECK(seen.insert({c.kysContext, c.kysName}).second);
        CHECK(registry.findKys(c.kysContext, c.kysName) == &c);
    }
    // The same Premiere command name means different commands in different panels.
    CHECK(registry.findKys("context.global", "cmd.edit.clear")->id == "edit.clear");
    CHECK(registry.findKys("context.timeline", "cmd.edit.clear")->id == "timeline.clearSelection");
    CHECK(registry.findKys("context.project", "cmd.edit.clear")->id == "project.delete");
    CHECK(registry.findKys("context.global", "cmd.edit.rippledelete")->id == "edit.rippleDelete");
    CHECK(registry.findKys("context.timeline", "cmd.timeline.ripple.delete")->id == "timeline.rippleDelete");
    CHECK(registry.findKys("context.global", "cmd.export.frame") == nullptr);
}

TEST_CASE("The MVP command set is registered in the right scopes", "[keymap][registry]") {
    const CommandRegistry& registry = CommandRegistry::builtin();
    const auto expectScope = [&](std::string_view id, Scope scope) {
        INFO(id);
        const CommandInfo* c = registry.find(id);
        REQUIRE(c != nullptr);
        CHECK(c->scope == scope);
    };
    for (std::string_view id : {"tool.selection",
                                "tool.trackSelectForward",
                                "tool.trackSelectBackward",
                                "tool.rippleEdit",
                                "tool.rollingEdit",
                                "tool.rateStretch",
                                "tool.razor",
                                "tool.slip",
                                "tool.slide",
                                "tool.pen",
                                "tool.hand",
                                "tool.zoom",
                                "tool.type",
                                "transport.playStop",
                                "transport.shuttleBackward",
                                "transport.shuttleStop",
                                "transport.shuttleForward",
                                "transport.stepBackward",
                                "transport.stepForward",
                                "transport.stepBackwardFive",
                                "transport.stepForwardFive",
                                "transport.goToPreviousEdit",
                                "transport.goToNextEdit",
                                "transport.goToStart",
                                "transport.goToEnd",
                                "marker.goToIn",
                                "marker.goToOut",
                                "marker.markIn",
                                "marker.markOut",
                                "marker.markClip",
                                "marker.add",
                                "marker.clearIn",
                                "marker.clearOut",
                                "marker.clearInAndOut",
                                "clip.insert",
                                "clip.overwrite",
                                "sequence.lift",
                                "sequence.extract",
                                "sequence.addEdit",
                                "sequence.addEditToAllTracks",
                                "edit.rippleDelete",
                                "sequence.rippleTrimPreviousToPlayhead",
                                "sequence.rippleTrimNextToPlayhead",
                                "sequence.extendEditToPlayhead",
                                "sequence.selectClipAtPlayhead",
                                "sequence.matchFrame",
                                "sequence.snap",
                                "clip.link",
                                "clip.group",
                                "clip.speedDuration",
                                "sequence.applyVideoTransition",
                                "sequence.applyAudioTransition",
                                "sequence.applyDefaultTransitions",
                                "sequence.zoomIn",
                                "sequence.zoomOut",
                                "sequence.zoomToSequence",
                                "window.projects",
                                "window.sourceMonitor",
                                "window.timeline",
                                "window.programMonitor",
                                "window.effectControls",
                                "window.audioTrackMixer",
                                "window.effects",
                                "window.mediaBrowser",
                                "window.audioClipMixer",
                                "file.newSequence",
                                "file.import",
                                "file.exportMedia",
                                "file.save",
                                "file.saveAs",
                                "edit.undo",
                                "edit.redo",
                                "edit.cut",
                                "edit.copy",
                                "edit.paste",
                                "edit.pasteInsert",
                                "edit.selectAll",
                                "edit.deselectAll",
                                "sequence.renderInToOut"}) {
        expectScope(id, Scope::Application);
    }
    expectScope("timeline.rippleDelete", Scope::Timeline);
    expectScope("timeline.nudgeClipLeft", Scope::Timeline);
    expectScope("timeline.nudgeClipRight", Scope::Timeline);
    expectScope("audioMixer.loop", Scope::AudioMixer);
    expectScope("metadata.loop", Scope::Metadata);
    expectScope("effectControls.loopAudioPlayback", Scope::EffectControls);
    expectScope("programMonitor.showRulers", Scope::ProgramMonitor);
    // A.8: voice-over recording, app-wide, with no default key (Premiere's key sets bind none). A custom .kys binding of
    // Premiere's Voice-over Record (macOS key set: context.timeline / cmd.timeline.voiceover.track.record) maps to
    // record.toggle.
    for (std::string_view id : {"record.toggle", "record.start", "record.stop"}) {
        expectScope(id, Scope::Application);
    }
    CHECK(registry.findKys("context.timeline", "cmd.timeline.voiceover.track.record")->id == "record.toggle");
    CHECK(registry.find("record.start")->kysName.empty());
}

TEST_CASE("Only stepping-style commands auto-repeat", "[keymap][registry]") {
    const CommandRegistry& registry = CommandRegistry::builtin();
    CHECK(registry.find("transport.stepForward")->autoRepeat);
    CHECK(registry.find("timeline.nudgeClipLeft")->autoRepeat);
    CHECK(registry.find("sequence.zoomIn")->autoRepeat);
    CHECK(registry.find("edit.undo")->autoRepeat);
    CHECK_FALSE(registry.find("transport.playStop")->autoRepeat);
    CHECK_FALSE(registry.find("marker.add")->autoRepeat);
    CHECK_FALSE(registry.find("sequence.addEdit")->autoRepeat);
}

TEST_CASE("Scope ids round-trip", "[keymap][registry]") {
    for (Scope s : kAllScopes) {
        INFO(scopeId(s));
        CHECK(scopeFromId(scopeId(s)) == s);
        CHECK_FALSE(scopeDisplayName(s).empty());
    }
    CHECK_FALSE(scopeFromId("Timeline"));
    CHECK(scopeDisplayName(Scope::AudioMixer) == "Audio Track Mixer");
}
