#pragma once

// 0.2: nested and multicam clips for the tests of the analyses that read through them (highlights, dead time, Auto
// Reframe, audio sync, ...): a multicam source of fixture media edited into a sequence, and a Nest of a sequence's clips.

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Nesting.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace wabcut::test {

struct PlacedMulticam {
    ProjectState state;
    SequenceId multicam;
    ClipId video; ///< the multicam clip on V1 (its picture)
    ClipId audio; ///< the linked one on A1 (its sound)
};

/// A multicam source of `angles` (manual sync at `offsets`) overwritten onto V1 / A1 of `seq` at `at`, playing `source`
/// (the source's time) with angle `videoAngle` in the picture and `audioAngle` in the sound (0-based).
inline PlacedMulticam placeMulticam(const ProjectState& s, SequenceId seq, std::vector<MediaId> angles, std::vector<Ticks> offsets,
                                    TimeRange source, Ticks at, std::size_t videoAngle = 0, std::size_t audioAngle = 0) {
    edit::MulticamSpec spec;
    spec.angles = std::move(angles);
    spec.sync = MulticamSync::Manual;
    spec.offsets = std::move(offsets);
    auto mc = edit::createMulticam(s, spec);
    REQUIRE(mc.has_value());
    edit::SourceClip src;
    src.sequence = mc->id;
    src.sourceRange = source;
    src.patches = edit::defaultSourcePatches(*mc->state.findSequence(seq), *mc->state.findSequence(mc->id));
    src.videoAngle = AngleSet::of(videoAngle);
    src.audioAngles = AngleSet::of(audioAngle);
    auto placed = edit::overwriteEdit(mc->state, seq, src, at);
    REQUIRE(placed.has_value());
    PlacedMulticam out{std::move(placed->state), mc->id, {}, {}};
    const Sequence& parent = *out.state.findSequence(seq);
    for (const auto* tracks : {&parent.videoTracks, &parent.audioTracks}) {
        for (const TrackPtr& t : *tracks) {
            for (const Clip& c : t->clips) {
                if (c.sequence == mc->id && c.start() == at) {
                    (t->kind == TrackKind::Video ? out.video : out.audio) = c.id;
                }
            }
        }
    }
    REQUIRE(out.video.isValid());
    requireValid(out.state);
    return out;
}

/// Premiere's Nest of every clip of `seq` (with their links): one nested clip per kind replaces them.
inline edit::NestResult nestEverything(const ProjectState& s, SequenceId seq) {
    std::vector<ClipId> ids;
    const Sequence& parent = *s.findSequence(seq);
    for (const auto* tracks : {&parent.videoTracks, &parent.audioTracks}) {
        for (const TrackPtr& t : *tracks) {
            for (const Clip& c : t->clips) {
                ids.push_back(c.id);
            }
        }
    }
    auto nested = edit::nestClips(s, seq, ids, "Nest");
    REQUIRE(nested.has_value());
    requireValid(nested->edit.state);
    return std::move(*nested);
}

} // namespace wabcut::test
