#include <wabcut/audio/edit/VoiceOver.h>

#include <wabcut/model/ProjectOps.h>

#include <algorithm>
#include <memory>
#include <string_view>

namespace wabcut::audio::edit {

namespace {

Error invalid(std::string message) { return Error(ErrorCode::InvalidArgument, std::move(message)); }

/// The OS's device name made safe to keep: valid UTF-8 (invalid bytes dropped), at most the provenance's bound.
std::string boundedDeviceName(std::string_view text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        const std::size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        bool ok = len > 0 && i + len <= text.size();
        for (std::size_t k = 1; ok && k < len; ++k) {
            ok = (static_cast<unsigned char>(text[i + k]) & 0xC0) == 0x80;
        }
        if (!ok) {
            ++i;
            continue;
        }
        if (out.size() + len > RecordingProvenance::kMaxDeviceBytes) {
            break;
        }
        out.append(text.substr(i, len));
        i += len;
    }
    return out;
}

std::string trackLabelOf(const Sequence& seq, TrackId track) {
    for (std::size_t i = 0; i < seq.audioTracks.size(); ++i) {
        if (seq.audioTracks[i]->id == track) {
            return "A" + std::to_string(i + 1);
        }
    }
    return "A?";
}

bool overlaps(const TimeRange& a, const TimeRange& b) { return a.start < b.end() && b.start < a.end(); }

/// The recorded media of a slot: the same sequence and track, a range overlapping `range`.
std::vector<const MediaItem*> slotTakes(const ProjectState& state, SequenceId sequence, TrackId track, const TimeRange& range) {
    std::vector<const MediaItem*> out;
    for (const MediaPtr& m : state.media) {
        if (m->recording && m->recording->sequence == sequence && m->recording->track == track && overlaps(m->recording->range, range)) {
            out.push_back(m.get());
        }
    }
    return out;
}

const char* codecOf(int bits) { return bits == 16 ? "pcm_s16le" : bits == 32 ? "pcm_f32le" : "pcm_s24le"; }

} // namespace

std::string voiceOverTakeName(const std::string& trackLabel, std::int32_t take) {
    return "Voice-over " + trackLabel + " Take " + std::to_string(take);
}

wabcut::edit::EditOutcome placeVoiceOverTake(const ProjectState& state, SequenceId sequence, TrackId track, const RecordedTake& take) {
    const Sequence* seq = state.findSequence(sequence);
    if (seq == nullptr) {
        return std::unexpected(Error(ErrorCode::NotFound, "sequence " + std::to_string(sequence.value) + " does not exist"));
    }
    const Track* target = seq->findTrack(track);
    if (target == nullptr || target->kind != TrackKind::Audio) {
        return std::unexpected(invalid("a take goes on an audio track"));
    }
    if (take.duration.value <= 0 || take.path.empty()) {
        return std::unexpected(invalid("the take is empty"));
    }
    const TimeRange range{take.start, take.duration};
    std::int32_t number = 1;
    for (const MediaItem* m : slotTakes(state, sequence, track, range)) {
        number = std::max(number, m->recording->take + 1);
    }
    ProjectState next = state;
    // The Voice-over bin (at the top level), made when missing.
    BinId bin;
    for (const Bin& b : next.bins) {
        if (b.name == kVoiceOverBin && !b.parent.isValid()) {
            bin = b.id;
            break;
        }
    }
    if (!bin.isValid()) {
        auto made = wabcut::edit::addBin(next, std::string(kVoiceOverBin));
        if (!made) {
            return std::unexpected(made.error());
        }
        next = std::move(made->state);
        bin = made->id;
    }
    MediaItem item;
    item.bin = bin;
    item.name = voiceOverTakeName(trackLabelOf(*seq, track), number);
    item.locator.path = take.path;
    item.locator.fingerprint = take.fingerprint;
    item.info.path = take.path;
    item.info.container = "wav";
    item.info.duration = take.duration;
    item.info.fileSize = std::max<std::int64_t>(0, take.fingerprint.size);
    AudioStreamInfo stream;
    stream.index = 0;
    stream.codec = codecOf(take.bitsPerSample);
    stream.sampleRate = Rate{take.sampleRate, 1};
    stream.channels = take.channels;
    stream.channelLayout = take.channels == 1 ? "mono" : "stereo";
    stream.duration = take.duration;
    item.info.audio.push_back(stream);
    RecordingProvenance recording;
    recording.sequence = sequence;
    recording.track = track;
    recording.range = range;
    recording.take = number;
    recording.device = boundedDeviceName(take.device);
    recording.latencyCompensation = take.latencyCompensation;
    recording.createdAt = take.createdAt;
    item.recording = std::move(recording);
    auto added = wabcut::edit::addMedia(next, std::move(item));
    if (!added) {
        return std::unexpected(added.error());
    }
    const MediaId media = added->id;
    wabcut::edit::SourceClip source;
    source.media = media;
    source.sourceRange = TimeRange{Ticks{}, take.duration};
    source.patches = {wabcut::edit::SourcePatch{StreamRef{StreamKind::Audio, 0}, track}};
    auto placed = wabcut::edit::overwriteEdit(added->state, sequence, source, take.start);
    if (!placed) {
        return std::unexpected(placed.error());
    }
    placed->createdMedia = media;
    return placed;
}

std::vector<VoiceOverTake> voiceOverTakesOf(const ProjectState& state, SequenceId sequence, ClipId clip) {
    std::vector<VoiceOverTake> out;
    const Sequence* seq = state.findSequence(sequence);
    const auto at = seq != nullptr ? seq->findClip(clip) : std::nullopt;
    if (!at) {
        return out;
    }
    const MediaItem* playing = state.findMedia(at->clip().media);
    if (playing == nullptr || !playing->recording) {
        return out;
    }
    const RecordingProvenance& slot = *playing->recording;
    for (const MediaItem* m : slotTakes(state, slot.sequence, slot.track, slot.range)) {
        out.push_back(VoiceOverTake{m->id, m->recording->take, m->recording->range, m->name, m->recording->device, m->recording->createdAt,
                                    m->id == playing->id});
    }
    std::sort(out.begin(), out.end(), [](const VoiceOverTake& a, const VoiceOverTake& b) { return a.take > b.take; });
    return out;
}

wabcut::edit::EditOutcome useVoiceOverTake(const ProjectState& state, SequenceId sequence, ClipId clip, std::int32_t take) {
    const Sequence* seq = state.findSequence(sequence);
    const auto at = seq != nullptr ? seq->findClip(clip) : std::nullopt;
    if (!at) {
        return std::unexpected(Error(ErrorCode::NotFound, "clip " + std::to_string(clip.value) + " is not in the sequence"));
    }
    const Clip old = at->clip();
    const TrackId track = at->track->id;
    const std::vector<VoiceOverTake> takes = voiceOverTakesOf(state, sequence, clip);
    if (takes.empty()) {
        return std::unexpected(invalid("clip " + std::to_string(clip.value) + " is not a recorded voice-over take"));
    }
    const auto chosen = std::find_if(takes.begin(), takes.end(), [&](const VoiceOverTake& t) { return t.take == take; });
    if (chosen == takes.end()) {
        return std::unexpected(invalid("the slot has no take " + std::to_string(take)));
    }
    wabcut::edit::SourceClip source;
    source.media = chosen->media;
    source.sourceRange = TimeRange{Ticks{}, chosen->range.duration};
    source.patches = {wabcut::edit::SourcePatch{StreamRef{StreamKind::Audio, 0}, track}};
    auto placed = wabcut::edit::overwriteEdit(state, sequence, source, chosen->range.start);
    if (!placed || placed->placed.empty()) {
        return placed;
    }
    // The new clip keeps the old one's level, pan and effects (when the overwrite replaced it whole: its effect
    // instances move with it), under the take's name.
    const bool replaced = std::find(placed->removed.begin(), placed->removed.end(), clip) != placed->removed.end();
    Sequence next = *placed->state.findSequence(sequence);
    for (TrackPtr& t : next.audioTracks) {
        if (t->id != track) {
            continue;
        }
        Track copy = *t;
        for (std::size_t i = 0; i < copy.clips.size(); ++i) {
            if (copy.clips[i].id != placed->placed.front()) {
                continue;
            }
            ClipContent content = copy.clips[i].content();
            content.name = chosen->name;
            if (replaced) {
                content.properties = old.properties();
                content.effects = old.effects();
            }
            copy.clips.mutableAt(i).setContent(std::move(content));
        }
        t = std::make_shared<const Track>(std::move(copy));
    }
    placed->state = placed->state.withSequence(std::make_shared<const Sequence>(std::move(next)));
    return placed;
}

} // namespace wabcut::audio::edit
