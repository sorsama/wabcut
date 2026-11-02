#include <wabcut/export/Stems.h>

#include <wabcut/export/ExportPresets.h>

#include <algorithm>
#include <memory>

namespace wabcut::exporter {

namespace {

constexpr TrackRole kOrder[] = {TrackRole::Dialogue, TrackRole::Music, TrackRole::Effects, TrackRole::None};

/// Whether the master export plays `track` (the mix's mute and solo rules).
bool playedByMaster(const Track& track, bool anySolo) {
    return track.enabled && !track.muted && (!anySolo || track.solo);
}

bool anyTrackSoloed(const Sequence& sequence) {
    return std::any_of(sequence.audioTracks.begin(), sequence.audioTracks.end(),
                       [](const TrackPtr& t) { return t && t->solo; });
}

} // namespace

StemRole stemRoleInfo(TrackRole role) {
    switch (role) {
    case TrackRole::Dialogue:
        return {role, "dialogue", "DX", "Dialogue", 0};
    case TrackRole::Music:
        return {role, "music", "MX", "Music", 0};
    case TrackRole::Effects:
        return {role, "effects", "FX", "Effects", 0};
    case TrackRole::None:
        break;
    }
    return {TrackRole::None, "other", "OTHER", "Other", 0};
}

std::optional<TrackRole> stemRoleFromKey(std::string_view key) {
    for (const TrackRole role : kOrder) {
        if (stemRoleInfo(role).key == key) {
            return role;
        }
    }
    return std::nullopt;
}

std::vector<StemRole> stemRoles(const Sequence& sequence) {
    std::vector<StemRole> out;
    if (sequence.multicam) {
        return out;
    }
    const bool anySolo = anyTrackSoloed(sequence);
    for (const TrackRole role : kOrder) {
        StemRole stem = stemRoleInfo(role);
        for (const TrackPtr& t : sequence.audioTracks) {
            if (t && t->role == role && playedByMaster(*t, anySolo) && !t->clips.empty()) {
                ++stem.tracks;
            }
        }
        if (stem.tracks > 0) {
            out.push_back(std::move(stem));
        }
    }
    return out;
}

ExportSettings stemSettings(const ExportSettings& master) {
    ExportSettings s;
    if (const auto wav = findPreset("audio-wav")) {
        s = wav->settings;
    }
    s.presetId = "audio-wav";
    s.container = Container::Wav;
    s.video.enabled = false;
    s.audio.enabled = true;
    s.audio.codec = AudioCodec::Pcm24;
    s.audio.channels = 0; // the master's channel format
    s.audio.sampleRate = master.audio.enabled ? master.audio.sampleRate : 48000;
    s.range = master.range;
    s.customRange = master.customRange;
    s.captions = {};
    s.subtitles = {};
    s.effects = {}; // no loudness normalisation (and nothing for the picture)
    s.metadata = master.metadata; // the Broadcast WAV time reference lines the stems up in a DAW
    s.metadata.xmp = XmpMode::None;
    s.metadata.chapters = false;
    return s;
}

ProjectState stemProject(const ProjectState& state, SequenceId sequenceId, TrackRole role, bool masterEffects) {
    const Sequence* source = state.findSequence(sequenceId);
    if (!source) {
        return state;
    }
    auto seq = std::make_shared<Sequence>(*source);
    const bool anySolo = anyTrackSoloed(*source);
    for (TrackPtr& t : seq->audioTracks) {
        if (!t) {
            continue;
        }
        const bool solo = t->role == role && playedByMaster(*t, anySolo);
        if (t->solo != solo) {
            auto copy = std::make_shared<Track>(*t);
            copy->solo = solo;
            t = std::move(copy);
        }
    }
    for (TrackPtr& t : seq->submixTracks) { // a submix sounds with the soloed tracks that feed it
        if (t && t->solo) {
            auto copy = std::make_shared<Track>(*t);
            copy->solo = false;
            t = std::move(copy);
        }
    }
    if (!masterEffects) {
        seq->audioEffects = {};
    }
    return state.withSequence(std::move(seq));
}

std::filesystem::path stemOutput(const std::filesystem::path& masterOutput, TrackRole role) {
    std::filesystem::path stem = masterOutput.stem();
    stem += "_" + stemRoleInfo(role).suffix + ".wav";
    return masterOutput.parent_path() / stem;
}

} // namespace wabcut::exporter
