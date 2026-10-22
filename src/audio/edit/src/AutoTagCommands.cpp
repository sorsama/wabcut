// p02 audio (docs/audio.md "Auto-tagging"): Essential Sound's automatic tagging, Premiere's auto-tag of clips as
// Dialogue, Music, SFX or Ambience: audio.autoTag listens to each clip through the host's SoundTagger (a local,
// deterministic classifier, SoundClassifier.h) and writes the type into the clip's Essential Sound record, the tag the
// panel shows and its presets follow. dryRun previews the proposals; applying them is one undo step.

#include "AudioCommandHelpers.h"

#include <wabcut/audio/edit/EssentialSound.h>
#include <wabcut/audio/edit/SoundClassifier.h>

namespace wabcut::audio::edit {

namespace {

using namespace detail;

void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
}

/// The clips to tag: "clips", or every audio clip of "tracks", or every audio clip of the sequence.
std::vector<ClipId> clipsToTag(ExecutionContext& c) {
    if (c.has("clips")) {
        return audioClipsOf(c, c.clips("clips"));
    }
    std::vector<ClipId> out;
    const Sequence& seq = c.sequence();
    const std::vector<TrackId> tracks = c.has("tracks") ? c.tracks("tracks") : std::vector<TrackId>{};
    for (const TrackPtr& t : seq.audioTracks) {
        if (!tracks.empty() && std::ranges::find(tracks, t->id) == tracks.end()) {
            continue;
        }
        for (const Clip& clip : t->clips) {
            out.push_back(clip.id);
        }
    }
    return out;
}

/// The clip's stack with the Essential Sound record's type set to `type` (the record made when there is none).
ProjectState withType(ExecutionContext& c, ProjectState state, ClipId id, SoundType type) {
    const SequenceId seq = c.sequenceId();
    const Clip& clip = sequenceIn(state, seq).findClip(id)->clip();
    const EffectInstance* record = findEffect(clip, kEssentialSoundEffect);
    wabcut::edit::EffectChange change;
    if (record != nullptr) {
        change.data = {EffectData{"type", std::string(toString(type))}};
        return c.take(wabcut::edit::setEffect(state, seq, id, record->id, change)).state;
    }
    auto added = c.take(wabcut::edit::addEffect(state, seq, std::span<const ClipId>(&id, 1), std::string(kEssentialSoundEffect), {}, 0));
    const EffectInstanceId created = added.createdEffects.front();
    change.data = essentialSoundRecord(type, "", "{}");
    return c.take(wabcut::edit::setEffect(added.state, seq, id, created, change)).state;
}

CommandOutput autoTag(ExecutionContext& c) {
    AudioServices& services = servicesOf(c, false, false);
    if (!services.tagger) {
        fail(ErrorCode::InvalidRequest, "no auto-tagging here: the host provides no sound classifier");
    }
    const bool overwrite = c.boolean("overwrite", false);
    const double minConfidence = c.has("minConfidence") ? c.number("minConfidence") : 0.0;
    const std::vector<ClipId> clips = clipsToTag(c);
    if (clips.empty()) {
        fail(ErrorCode::InvalidParams, "no audio clips to tag", Json{{"path", "/clips"}});
    }
    ProjectState state = c.state();
    const SequenceId seq = c.sequenceId();
    Json report = Json::array();
    int tagged = 0;
    for (const ClipId id : clips) {
        const auto at = sequenceIn(state, seq).findClip(id);
        const Clip& clip = at->clip();
        Json row{{"clip", id.value}, {"track", c.trackLabel(at->track->id)}};
        const MediaItem* item = clip.isNested() || clip.hasUnknownSource() ? nullptr : state.findMedia(clip.media);
        if (item == nullptr || clip.stream.kind != StreamKind::Audio) {
            row["skipped"] = "not an audio clip of a media item";
            report.push_back(std::move(row));
            continue;
        }
        const auto guess = services.tagger->classify(state, *item, clip.stream.index,
                                                     TimeRange{clip.sourceIn, clip.duration()}, c.control());
        if (!guess) {
            failWith(guess.error(), "auto-tagging clip " + std::to_string(id.value));
        }
        if (guess->silent) {
            row["skipped"] = "silent";
            report.push_back(std::move(row));
            continue;
        }
        row["type"] = std::string(toString(guess->type));
        row["confidence"] = round3(guess->confidence);
        row["scores"] = Json{{"dialogue", round3(guess->scores[0])},
                             {"music", round3(guess->scores[1])},
                             {"sfx", round3(guess->scores[2])},
                             {"ambience", round3(guess->scores[3])}};
        const std::optional<SoundType> existing = recordedSoundType(clip.effects());
        if (existing && !overwrite) {
            row["kept"] = std::string(toString(*existing)); // a type set by hand (or earlier) stays
        } else if (guess->confidence < minConfidence) {
            row["skipped"] = "below minConfidence";
        } else if (existing != guess->type) {
            state = withType(c, std::move(state), id, guess->type);
            row["applied"] = true;
            ++tagged;
        }
        report.push_back(std::move(row));
        c.checkpoint("auto-tagging");
    }
    CommandOutput out;
    out.state = std::move(state);
    out.data = Json{{"clips", std::move(report)}, {"tagged", tagged}};
    return out;
}

} // namespace

void registerAutoTagCommands(command::CommandRegistry& registry) {
    add(registry,
        {"audio.autoTag", 1, "Auto-tag Audio",
         "Essential Sound's automatic tagging: listens to each audio clip (up to 30 s from its middle) and tags it Dialogue, "
         "Music, SFX or Ambience, with a local, deterministic classifier (syllable-rate modulation, pitch glides and held "
         "notes, beat regularity, pauses, spectral flatness, crest; nothing leaves the computer). The tag is the clip's "
         "Essential Sound record: the panel shows the type and offers its presets; no effect is added. Clips that already "
         "have a type keep it unless \"overwrite\". Without \"clips\" or \"tracks\": every audio clip of the sequence. "
         "dryRun previews the proposals with their confidence; applying them is one undo step.",
         Capability::EditSequence,
         withSequence({{"clips", schema::array(Json{{"type", Json::array({"integer", "object"})}}, "The audio clips (video clips stand for their linked audio).")},
                       {"tracks", schema::array(trackParam("An audio track."), "Every clip of these audio tracks.")},
                       {"overwrite", schema::boolean("Retag clips that already have a type (default false).")},
                       {"minConfidence", schema::number("Tag only above this confidence, 0..1 (default 0).", 0.0, 1.0)}}),
         Json{{"tracks", Json::array({"A2"})}}, {}, autoTag});
}

} // namespace wabcut::audio::edit
