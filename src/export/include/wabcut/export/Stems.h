#pragma once

// 0.6 (#52): dialogue / music / effects stems with the master (Premiere's multichannel stems, Resolve's Deliver stems;
// a DME or M&E delivery). "By track role" splits the sequence's audio by the role of its audio tracks (Track::role,
// the Essential Sound types): one audio-only WAV per role that has an audible track, next to the main export and named
// after it ("<output stem>_DX.wav", "_MX", "_FX", "_OTHER" for tracks without a role).
//
// A stem is an ordinary export of a copy of the project in which that role's tracks are soloed and every other track
// is not: the same mix path, range and sample rate as the master, so the stems line up with it sample for sample (and
// add up to it when the master bus has no effects). Only tracks the master itself plays go into a stem (muted tracks
// and, when some tracks are soloed, the unsoloed ones stay out). Stems never get loudness normalisation; the master
// bus's effects stay on unless the request asks for stems without them.
//
// The export requests carry the choice in their overrides (ExportController and the export.* commands):
//   "stems": "role"             on the main export: queue its stems after it ("off" or absent: none)
//   "stemsMasterEffects": false the stems skip the master bus's effects (default true)
//   "stemOnly": "dialogue"      this request IS the stem of that role ("dialogue", "music", "effects", "other")

#include <wabcut/export/ExportSettings.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Track.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::exporter {

/// One stem of a sequence.
struct StemRole {
    TrackRole role = TrackRole::None;
    std::string key;    ///< "dialogue", "music", "effects", "other" (the "stemOnly" value)
    std::string suffix; ///< "DX", "MX", "FX", "OTHER"
    std::string name;   ///< English name: "Dialogue", "Music", "Effects", "Other"
    int tracks = 0;     ///< the sequence's audio tracks that go into it
};

/// The role's key, suffix and name (tracks 0).
StemRole stemRoleInfo(TrackRole role);
/// The role of a "stemOnly" key, or nullopt.
std::optional<TrackRole> stemRoleFromKey(std::string_view key);

/// The stems a "By track role" export of `sequence` writes, in delivery order: Dialogue, Music, Effects, then Other
/// (tracks without a role). A role is written when at least one of its audio tracks is played by the master (enabled,
/// not muted, soloed when any track is) and has a clip. Empty for multicam source sequences (their angles are not
/// tracks a mix plays).
std::vector<StemRole> stemRoles(const Sequence& sequence);

/// The settings of a stem of an export with `master`: audio only, WAV, 24-bit PCM, the master's sample rate (48 kHz
/// when the master has no audio), the master's channel format (5.1 stays 5.1), the same range and start timecode; no
/// loudness normalisation, captions or XMP.
ExportSettings stemSettings(const ExportSettings& master);

/// `state` with `sequence`'s audio tracks of `role` that the master plays soloed and every other track (submixes too)
/// unsoloed; `masterEffects` false also empties the master bus's effect stack (the master fader stays).
ProjectState stemProject(const ProjectState& state, SequenceId sequence, TrackRole role, bool masterEffects);

/// "<folder>/<master output's stem>_<suffix>.wav".
std::filesystem::path stemOutput(const std::filesystem::path& masterOutput, TrackRole role);

} // namespace wabcut::exporter
