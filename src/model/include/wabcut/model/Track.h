#pragma once

// A sequence track: an ordered, non-overlapping list of clips. Gaps are implicit (Premiere does the same).
// Transitions are edge objects between neighbouring clips; they never make clips overlap.

#include <wabcut/model/AudioChannels.h>
#include <wabcut/model/Clip.h>
#include <wabcut/model/ClipList.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wabcut {

enum class TrackKind : std::uint8_t { Video, Audio };

constexpr StreamKind streamKindOf(TrackKind kind) {
    return kind == TrackKind::Video ? StreamKind::Video : StreamKind::Audio;
}

/// Where a transition sits relative to its cut (Premiere's alignment). Stored on the transition (schema 8);
/// operations keep `before` / `after` matching it.
enum class TransitionAlignment : std::uint8_t {
    Center,     ///< half before, half after the cut: whole frames, an odd frame after it (within a frame)
    StartAtCut, ///< all after the cut (before == 0): the outgoing clip plays on into its handle
    EndAtCut,   ///< all before the cut (after == 0): the incoming clip starts early from its handle
    Custom,     ///< the cut lies `before` into the transition: an explicit offset, kept when the length changes
};

std::string_view toString(TransitionAlignment alignment);
std::optional<TransitionAlignment> transitionAlignmentFromString(std::string_view text);

/// Whether `before` / `after` place a transition as `alignment` says (Center: they differ by at most `frame`;
/// Custom: always).
bool alignmentFits(TransitionAlignment alignment, Ticks before, Ticks after, Ticks frame);
/// The alignment `before` / `after` describe: StartAtCut when nothing is before the cut, EndAtCut when nothing
/// is after it, Center when the sides differ by at most `frame`, otherwise Custom. Used for files from before
/// schema 8, imported transitions and sides set explicitly.
TransitionAlignment alignmentOf(Ticks before, Ticks after, Ticks frame);
/// Splits a length into (before, after) by alignment: Center puts the whole frames of half the length before
/// the cut, Custom keeps `offset` before it (at most the length).
std::pair<Ticks, Ticks> splitTransition(Ticks duration, TransitionAlignment alignment, Ticks offset, Rate rate);
/// What an audio track carries (Premiere's Essential Sound types), for the dialogue-first tools: auto-ducking
/// lowers Music under Dialogue, loudness presets and Enhance Speech look for Dialogue. Video tracks are always None.
enum class TrackRole : std::uint8_t { None, Dialogue, Music, Effects };

/// "none", "dialogue", "music", "effects" (the serialized and command form).
constexpr std::string_view toString(TrackRole role) {
    switch (role) {
    case TrackRole::Dialogue:
        return "dialogue";
    case TrackRole::Music:
        return "music";
    case TrackRole::Effects:
        return "effects";
    case TrackRole::None:
        break;
    }
    return "none";
}

constexpr std::optional<TrackRole> trackRoleFromString(std::string_view text) {
    for (const TrackRole role : {TrackRole::None, TrackRole::Dialogue, TrackRole::Music, TrackRole::Effects}) {
        if (text == toString(role)) {
            return role;
        }
    }
    return std::nullopt;
}

/// A transition on an edit point. Two-sided transitions sit on the cut between clip A (`outgoing`, which
/// ends at the cut) and clip B (`incoming`, which starts there) and cover [cut - before, cut + after):
/// A keeps playing `after` past its Out point and B starts `before` ahead of its In point, so both need
/// that much spare source media (handles). A one-sided transition has only one clip and stays inside
/// it: at A's tail ("to black", after == 0) or at B's head ("from black", before == 0).
/// The renderer evaluates the two source layers itself; clips on a track never overlap.
struct Transition {
    TransitionId id;
    ClipId outgoing; ///< clip A; invalid for a transition at B's head
    ClipId incoming; ///< clip B; invalid for a transition at A's tail
    Ticks before;    ///< length before the cut
    Ticks after;     ///< length after the cut
    std::string effect = "crossDissolve"; ///< stable transition id (EffectRegistry, kind *Transition)
    /// Constant parameters by key, sorted (e.g. a wipe's "angle"); unknown keys are kept.
    std::vector<EffectParam> params;
    /// How the sides were placed (one-sided: EndAtCut at A's tail, StartAtCut at B's head).
    TransitionAlignment alignment = TransitionAlignment::Center;

    /// Source media each clip needs beyond its own range.
    struct Handles {
        Ticks outgoingTail; ///< past A's Out point
        Ticks incomingHead; ///< before B's In point
    };
    Handles handles() const {
        if (outgoing.isValid() && incoming.isValid()) {
            return {after, before};
        }
        return {};
    }
    Ticks duration() const { return before + after; }

    bool operator==(const Transition&) const = default;
};

/// p02 audio: the most sends an audio track has (Premiere's Audio Track Mixer sends A to E).
inline constexpr std::size_t kMaxTrackSends = 5;

/// p02 audio: a send from an audio track to a submix track (Premiere's Audio Track Mixer sends): a copy of the track's
/// signal, taken before its fader (pre-fader) or after it (post-fader, Premiere's default), at the send's own level and
/// pan, mixed into the submix next to whatever the submix gets from the tracks assigned to it. A muted or soloed-away
/// track is silent in its sends too.
struct TrackSend {
    TrackId submix;        ///< a submix track of the same sequence (Sequence::submixTracks)
    bool preFader = false; ///< Pre-Fader: the send ignores the track's fader and panner
    bool muted = false;    ///< the send's mute
    /// Send volume, dB (ParamId::VolumeDb's range, -144..+24; 0 = unity): a constant or keyframes in timeline time.
    ParamValue level;
    /// Send pan (ParamId::Pan, -1..1): a balance on a stereo mix, the pan of a mono submix's input; timeline time.
    ParamValue pan;

    bool operator==(const TrackSend&) const = default;
};

struct Track {
    TrackId id;
    TrackKind kind = TrackKind::Video;
    /// User-visible custom name; empty means the default ("Video 1", "Audio 2", …).
    std::string name;
    /// Track output on (video "eye" / audio track enabled).
    bool enabled = true;
    /// Track lock: no edit may change this track, and ripples do not shift it.
    bool locked = false;
    /// Sync lock (Premiere, on by default): ripple edits on other tracks (insert, extract, ripple
    /// delete/trim) shift this track too, cutting through clips, so it stays in sync.
    bool syncLocked = true;
    bool muted = false;
    bool solo = false;
    /// Track targeting: the tracks lift/extract, mark clip and Q/W act on.
    bool targeted = false;
    /// Audio tracks: what the track carries (dialogue, music, effects). Always None on video tracks.
    TrackRole role = TrackRole::None;
    /// A.9, audio tracks: the track's channel format (Premiere's mono, stereo, 5.1 and adaptive tracks): clips are mixed
    /// to it, and it to the master's format. Stereo on video tracks. Saved only when not stereo.
    ChannelFormat format = ChannelFormat::Stereo;
    /// A.9, mono and stereo audio tracks on a 5.1 master: the 5.1 panner (puck and LFE send). Saved only when changed.
    SurroundPan surround;
    /// A.8, audio tracks: armed for voice-over recording (Premiere's track R button); a recording pass records onto
    /// the armed tracks. Always false on video tracks. Saved only when set.
    bool recordArmed = false;
    /// Sorted by start; clips never overlap and have positive duration. Chunked and shared between
    /// revisions (see ClipList).
    ClipList clips;
    /// Sorted by edit point; at most one per edit point and never overlapping.
    std::vector<Transition> transitions;
    /// Track effect stack, applied to the composited track (video) or the mixed track (audio, before the track
    /// fader: the audio engine evaluates it). Edited with the target-taking effect operations (FinishingOps.h);
    /// keyframe times on track effects are timeline times.
    EffectStack effects;
    /// A.3, audio tracks: the track fader and panner (Premiere's track Volume and Panner), applied to the mixed track
    /// after its effect stack. They are the intrinsic wabcut.volume / wabcut.panner parameters (ParamId::VolumeDb in
    /// dB, 0 = unity; ParamId::Pan, -1..1 balance), constants or curves in timeline time, edited with the
    /// target-taking parameter and keyframe operations (FinishingOps.h: EffectTarget::ofTrack with
    /// ParamRef::of(ParamId::VolumeDb / ParamId::Pan)). Default (0 dB, centre) on video tracks.
    ParamValue volume;
    ParamValue pan;
    /// p02 audio, audio and submix tracks: Premiere's track output assignment: TrackId{} = the Mix (the master), or a
    /// submix track of the sequence (never one that outputs back to this track: no cycles). Saved only when set.
    TrackId output;
    /// p02 audio, audio tracks: the track's sends to submixes, at most kMaxTrackSends. Saved only when there are some.
    std::vector<TrackSend> sends;

    /// End of the last clip (0 for an empty track).
    Ticks end() const { return clips.empty() ? Ticks{} : clips.back().end(); }

    /// Index of the first clip that starts at or after `t`.
    std::size_t firstClipAtOrAfter(Ticks t) const { return clips.lowerBoundStart(t); }

    /// Index of the clip whose range contains `t`, if any.
    std::optional<std::size_t> clipIndexAt(Ticks t) const {
        const std::size_t after = clips.upperBoundStart(t);
        if (after == 0) {
            return std::nullopt;
        }
        const std::size_t index = after - 1;
        if (clips[index].timelineRange.contains(t)) {
            return index;
        }
        return std::nullopt;
    }

    /// Linear in the number of clips.
    std::optional<std::size_t> indexOf(ClipId clipId) const {
        std::size_t i = 0;
        for (const Clip& c : clips) {
            if (c.id == clipId) {
                return i;
            }
            ++i;
        }
        return std::nullopt;
    }

    const Clip* findClip(ClipId clipId) const {
        const auto index = indexOf(clipId);
        return index ? &clips[*index] : nullptr;
    }

    /// Whether name or header flags differ (not clips or transitions).
    bool sameHeader(const Track& o) const {
        return id == o.id && kind == o.kind && name == o.name && enabled == o.enabled && locked == o.locked &&
               syncLocked == o.syncLocked && muted == o.muted && solo == o.solo && targeted == o.targeted &&
               role == o.role && recordArmed == o.recordArmed && format == o.format && surround == o.surround &&
               effects == o.effects && volume == o.volume && pan == o.pan && output == o.output && sends == o.sends;
    }

    bool operator==(const Track&) const = default;
};

} // namespace wabcut
