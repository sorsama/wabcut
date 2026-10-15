// A.8 (docs/ai.md "Voice-over recording"): track.setRecordArm, record.takes and record.useTake. Recording itself
// (record.start / record.stop, Premiere's Voice-over Record) is the app's and is not a command: the microphone is
// opened only on the user's own action in the app, never by a command an agent, a script or an MCP client sends.

#include "AudioCommandHelpers.h"

#include <wabcut/audio/edit/VoiceOver.h>

namespace wabcut::audio::edit {

namespace {

using namespace detail;

void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
}

CommandOutput setRecordArm(ExecutionContext& c) {
    wabcut::edit::TrackStateChange change;
    change.recordArmed = c.boolean("armed", true);
    const TrackId track = c.track("track");
    return edited(c.take(wabcut::edit::setTrackState(c.state(), c.sequenceId(), track, change)),
                  Json{{"track", c.trackLabel(track)}, {"armed", *change.recordArmed}});
}

/// A clip that plays a recorded take; NotFound / InvalidParams otherwise.
std::vector<VoiceOverTake> takesOrFail(ExecutionContext& c, ClipId clip) {
    if (!c.sequence().findClip(clip)) {
        fail(ErrorCode::NotFound, "no clip " + std::to_string(clip.value) + " in the sequence", Json{{"path", "/clip"}});
    }
    std::vector<VoiceOverTake> takes = voiceOverTakesOf(c.state(), c.sequenceId(), clip);
    if (takes.empty()) {
        fail(ErrorCode::InvalidParams, "clip " + std::to_string(clip.value) + " is not a recorded voice-over take", Json{{"path", "/clip"}});
    }
    return takes;
}

Json takeJson(ExecutionContext& c, const VoiceOverTake& t) {
    return Json{{"take", t.take},
                {"media", t.media.value},
                {"name", t.name},
                {"start", c.timecode(t.range.start)},
                {"seconds", round3(t.range.duration.toSeconds())},
                {"device", t.device}, // the OS's name for the input (data, bounded)
                {"createdAt", t.createdAt},
                {"active", t.active}};
}

CommandOutput takes(ExecutionContext& c) {
    const ClipId clip = c.clip("clip");
    Json list = Json::array();
    for (const VoiceOverTake& t : takesOrFail(c, clip)) {
        list.push_back(takeJson(c, t));
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"clip", clip.value}, {"takes", std::move(list)}};
    return out;
}

CommandOutput useTake(ExecutionContext& c) {
    const ClipId clip = c.clip("clip");
    const std::int64_t take = c.integer("take");
    const std::vector<VoiceOverTake> takes = takesOrFail(c, clip);
    if (std::ranges::none_of(takes, [&](const VoiceOverTake& t) { return t.take == take; })) {
        fail(ErrorCode::InvalidParams, "clip " + std::to_string(clip.value) + "'s slot has no take " + std::to_string(take),
             Json{{"path", "/take"}});
    }
    wabcut::edit::EditResult result = c.take(useVoiceOverTake(c.state(), c.sequenceId(), clip, static_cast<std::int32_t>(take)));
    Json data{{"take", take}};
    if (!result.placed.empty()) {
        data["clip"] = result.placed.front().value;
    }
    CommandOutput out = edited(std::move(result), std::move(data));
    return out;
}

} // namespace

void registerVoiceOverCommands(command::CommandRegistry& registry) {
    add(registry,
        {"track.setRecordArm", 1, "Set Track Record Arm",
         "Arms an audio track for voice-over recording or disarms it (the R button of Premiere's track headers and Audio Track "
         "Mixer). The app's Record (record.start) records the microphone onto the armed track (else the targeted one) at the "
         "playhead or between the sequence In and Out; recording is the user's action in the app, never a command. Allowed on "
         "locked tracks; video tracks are refused. One undo step.",
         Capability::EditSequence,
         withSequence({{"track", trackParam("The audio track.")}, {"armed", schema::boolean("Arm (default true) or disarm.")}}, {"track"}),
         Json{{"track", "A2"}, {"armed", true}}, {}, setRecordArm});
    add(registry,
        {"record.takes", 1, "Voice-over Takes",
         "The takes of a recorded voice-over slot (Premiere's repeated voice-over passes over one range): each pass over the same "
         "track and range made a media item in the Voice-over bin, and the clip plays the newest unless record.useTake chose "
         "another. Lists them newest first with their number, media, start, length, input device, time and which one the clip "
         "plays.",
         Capability::Read, withSequence({{"clip", schema::integer("A clip that plays a recorded take.", 1)}}, {"clip"}), Json{{"clip", 42}},
         {}, takes});
    add(registry,
        {"record.useTake", 1, "Use Voice-over Take",
         "Plays another take of a recorded voice-over slot (record.takes): that take's media goes over its recorded range on the "
         "clip's track as an overwrite, with the clip's level, pan and effects. The takes stay in the Voice-over bin. One undo "
         "step; dryRun previews it.",
         Capability::EditSequence,
         withSequence({{"clip", schema::integer("A clip that plays a recorded take.", 1)}, {"take", schema::integer("The take number.", 1)}},
                      {"clip", "take"}),
         Json{{"clip", 42}, {"take", 2}}, {}, useTake});
}

} // namespace wabcut::audio::edit
