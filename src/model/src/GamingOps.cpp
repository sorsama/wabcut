// Gaming edit operations (C.16, GamingOps.h): instant replay, zoom punch and camera shake. Dead time is in DeadTime.cpp,
// vertical and square versions in GamingVersions.cpp.

#include <wabcut/model/GamingOps.h>

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/TimeRemap.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace wabcut {

namespace {

using edit::detail::invalidArgument;
using edit::detail::notFound;

double frameTicks(Rate rate) { return static_cast<double>(framesToTicks(1, rate).value); }

/// The frame boundary nearest `t` (ticks from a frame boundary).
Ticks nearestFrame(double t, Rate rate) {
    const double frame = frameTicks(rate);
    return framesToTicks(static_cast<std::int64_t>(std::llround(t / frame)), rate);
}

/// A clip of media the replay can retime: forward, constant speed, not remapped or frozen, not nested, not generated.
std::optional<std::string> replayProblem(const ProjectState& state, const Clip& clip) {
    const MediaItem* item = state.findMedia(clip.media);
    const std::string name = "clip " + std::to_string(clip.id.value) + (clip.name().empty() ? "" : " (" + clip.name() + ")");
    if (clip.isNested()) {
        return name + " plays a nested sequence";
    }
    if (!item) {
        return name + " plays no media";
    }
    if (clip.isRemapped()) {
        return name + " is time-remapped (turn remapping off first)";
    }
    if (clip.reversed) {
        return name + " plays in reverse";
    }
    if (clip.isFreeze()) {
        return name + " is a freeze frame";
    }
    return std::nullopt;
}

bool replayable(const ProjectState& state, const Clip& clip) {
    const MediaItem* item = state.findMedia(clip.media);
    return item && !item->isGenerator() && !clip.isNested() && !clip.hasUnknownSource();
}

/// The replay's time warp: moment time x in [0, L] -> replay time y in [0, T]. With ramps, the first R and last R
/// replay ticks are cubic Hermite ramps between 100 % and the middle's speed v (moment ticks per replay tick).
class Warp {
public:
    Warp(Ticks moment, Ticks replay) : m_l(static_cast<double>(moment.value)), m_t(static_cast<double>(replay.value)) {}
    Warp(Ticks moment, Ticks replay, Ticks rampMoment, Ticks rampReplay)
        : m_l(static_cast<double>(moment.value)), m_t(static_cast<double>(replay.value)),
          m_r(static_cast<double>(rampMoment.value)), m_rr(static_cast<double>(rampReplay.value)), m_ramp(true) {
        m_v = (m_l - 2.0 * m_r) / (m_t - 2.0 * m_rr);
    }

    bool ramped() const { return m_ramp; }
    /// Replay times where the warp changes shape (the ramps' ends), inside (0, T).
    std::vector<double> breakpoints() const {
        return m_ramp ? std::vector<double>{m_rr, m_t - m_rr} : std::vector<double>{};
    }
    /// Moment time at replay time y.
    double x(double y) const {
        if (!m_ramp) {
            return y * m_l / m_t;
        }
        if (y <= m_rr) {
            return hermite(y / m_rr, 0.0, m_r, 1.0 * m_rr, m_v * m_rr);
        }
        if (y >= m_t - m_rr) {
            return hermite((y - (m_t - m_rr)) / m_rr, m_l - m_r, m_l, m_v * m_rr, 1.0 * m_rr);
        }
        return m_r + (y - m_rr) * m_v;
    }
    /// dx/dy at replay time y.
    double slope(double y) const {
        if (!m_ramp) {
            return m_l / m_t;
        }
        if (y <= m_rr) {
            return hermiteSlope(y / m_rr, 0.0, m_r, 1.0 * m_rr, m_v * m_rr) / m_rr;
        }
        if (y >= m_t - m_rr) {
            return hermiteSlope((y - (m_t - m_rr)) / m_rr, m_l - m_r, m_l, m_v * m_rr, 1.0 * m_rr) / m_rr;
        }
        return m_v;
    }
    /// Replay time where moment time x plays (the inverse; monotonic, so bisection).
    double y(double xs) const {
        if (xs <= 0.0) {
            return 0.0;
        }
        if (xs >= m_l) {
            return m_t;
        }
        double lo = 0.0;
        double hi = m_t;
        for (int i = 0; i < 80; ++i) {
            const double mid = 0.5 * (lo + hi);
            (x(mid) < xs ? lo : hi) = mid;
        }
        return 0.5 * (lo + hi);
    }

private:
    // Cubic Hermite on u in [0, 1] from p0 to p1 with tangents m0, m1 (already scaled by the interval).
    static double hermite(double u, double p0, double p1, double m0, double m1) {
        const double u2 = u * u;
        const double u3 = u2 * u;
        return (2 * u3 - 3 * u2 + 1) * p0 + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * p1 + (u3 - u2) * m1;
    }
    static double hermiteSlope(double u, double p0, double p1, double m0, double m1) {
        const double u2 = u * u;
        return (6 * u2 - 6 * u) * p0 + (3 * u2 - 4 * u + 1) * m0 + (-6 * u2 + 6 * u) * p1 + (3 * u2 - 2 * u) * m1;
    }

    double m_l = 0.0;  // moment length
    double m_t = 0.0;  // replay length
    double m_r = 0.0;  // moment ticks each ramp covers
    double m_rr = 0.0; // replay ticks each ramp lasts
    double m_v = 1.0;  // the middle's speed
    bool m_ramp = false;
};

/// A piece of the moment: a clip trimmed to the moment, on its track.
struct Piece {
    TrackId track;
    TrackKind kind = TrackKind::Video;
    std::size_t trackIndex = 0; // V1 = 0 / A1 = 0
    Clip clip;
};

/// A free unlocked video track above video index `above` over `range`.
std::optional<TrackId> freeVideoTrackAbove(const Sequence& seq, std::size_t above, TimeRange range) {
    for (std::size_t i = above + 1; i < seq.videoTracks.size(); ++i) {
        const Track& t = *seq.videoTracks[i];
        if (t.locked) {
            continue;
        }
        const bool busy = std::any_of(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return c.timelineRange.overlaps(range); });
        if (!busy) {
            return t.id;
        }
    }
    return std::nullopt;
}

const Clip* clipEndingAt(const Track& track, Ticks t) {
    for (const Clip& c : track.clips) {
        if (c.end() == t) {
            return &c;
        }
    }
    return nullptr;
}

const Clip* clipStartingAt(const Track& track, Ticks t) {
    const std::size_t i = track.firstClipAtOrAfter(t);
    return i < track.clips.size() && track.clips[i].start() == t ? &track.clips[i] : nullptr;
}

/// The value units a remap slope takes: source ticks per second of curve time.
double remapSlope(double sourcePerTimeline) { return sourcePerTimeline * static_cast<double>(kTicksPerSecond); }

/// The video piece's copy with a remap curve following the warp over replay [yFrom, yTo).
std::optional<Curve> replayRemap(const Clip& piece, Ticks momentStart, const Warp& warp, Ticks yFrom, Ticks yTo,
                                 std::optional<Ticks> mediaEnd) {
    const double x0 = static_cast<double>((piece.start() - momentStart).value);
    const double speed = static_cast<double>(piece.speed.num) / static_cast<double>(piece.speed.den);
    const double sourceIn = static_cast<double>(piece.sourceIn.value);
    // A piece that runs to the media's end stops a tick short of it, so handles past the replay's end never read past
    // the media.
    const Ticks out = mediaEnd && piece.sourceOut() >= *mediaEnd ? *mediaEnd - Ticks{1} : piece.sourceOut();
    const double sourceOut = static_cast<double>(out.value);
    std::vector<double> ys{static_cast<double>(yFrom.value)};
    for (const double b : warp.breakpoints()) {
        if (b > static_cast<double>(yFrom.value) && b < static_cast<double>(yTo.value)) {
            ys.push_back(std::round(b));
        }
    }
    ys.push_back(static_cast<double>(yTo.value));
    Curve curve;
    for (const double yv : ys) {
        Keyframe k;
        k.time = piece.sourceIn + Ticks{static_cast<std::int64_t>(yv) - yFrom.value};
        const double source = std::clamp(sourceIn + (warp.x(yv) - x0) * speed, sourceIn, sourceOut);
        k.value = std::round(source);
        if (warp.ramped()) {
            const double s = remapSlope(warp.slope(yv) * speed);
            k.interpolation = Interpolation::Bezier;
            k.in = BezierHandle{s, 1.0 / 3.0};
            k.out = BezierHandle{s, 1.0 / 3.0};
        }
        if (!curve.keys.empty() && curve.keys.back().time >= k.time) {
            continue;
        }
        curve.keys.push_back(k);
    }
    if (curve.keys.size() < 2 || remapProblem(curve)) {
        return std::nullopt;
    }
    return curve;
}

EffectStack freshEffects(edit::detail::SequenceEditor& ed, const EffectStack& effects) {
    EffectStack out = effects;
    for (EffectInstance& e : out) {
        e.id = ed.newEffectId();
    }
    return out;
}

} // namespace

namespace edit {

std::expected<ReplayResult, Error> instantReplay(const ProjectState& state, SequenceId sequenceId, const ReplayOptions& options) {
    const auto found = detail::findSequence(state, sequenceId);
    if (!found) {
        return std::unexpected(found.error());
    }
    const Sequence& seq = **found;
    if (options.clip.has_value() == options.range.has_value()) {
        return std::unexpected(invalidArgument("instant replay takes a clip or a range (one of them)"));
    }
    if (!std::isfinite(options.speed) || options.speed < kMinReplaySpeed || options.speed > kMaxReplaySpeed) {
        return std::unexpected(invalidArgument("the replay speed must be 0.05..1"));
    }
    if (options.flashDuration.value < 0) {
        return std::unexpected(invalidArgument("the flash duration must not be negative"));
    }

    // The moment and its pieces.
    std::vector<Piece> pieces;
    std::vector<std::string> notes;
    TimeRange moment;
    const auto addPiece = [&](const Track& track, std::size_t index, const Clip& clip, TimeRange within) {
        Clip p = clip;
        if (p.start() < within.start) {
            p.moveHead(within.start - p.start());
        }
        if (p.end() > within.end()) {
            p.moveTail(within.end() - p.end());
        }
        if (p.duration().value > 0) {
            pieces.push_back({track.id, track.kind, index, std::move(p)});
        }
    };
    if (options.clip) {
        const auto loc = seq.findClip(*options.clip);
        if (!loc) {
            return std::unexpected(notFound("clip " + std::to_string(options.clip->value) + " is not in this sequence"));
        }
        const Clip& clip = loc->clip();
        const auto pos = seq.trackPosition(loc->track->id);
        if (pos->kind != TrackKind::Video) {
            return std::unexpected(invalidArgument("instant replay takes a video clip (its audio follows with includeAudio)"));
        }
        if (loc->track->locked) {
            return std::unexpected(invalidArgument("the clip's track is locked"));
        }
        if (const auto problem = replayProblem(state, clip)) {
            return std::unexpected(invalidArgument("instant replay cannot slow " + *problem));
        }
        moment = clip.timelineRange;
        addPiece(*loc->track, pos->index, clip, moment);
        if (clip.linkGroup.isValid()) {
            for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
                if (kind == TrackKind::Audio && !options.includeAudio) {
                    continue;
                }
                const auto& tracks = seq.tracks(kind);
                for (std::size_t i = 0; i < tracks.size(); ++i) {
                    const Track& t = *tracks[i];
                    if (t.locked) {
                        continue;
                    }
                    for (const Clip& c : t.clips) {
                        if (c.id != clip.id && c.linkGroup == clip.linkGroup && c.timelineRange.overlaps(moment)) {
                            if (const auto problem = replayProblem(state, c)) {
                                return std::unexpected(invalidArgument("instant replay cannot slow " + *problem));
                            }
                            addPiece(t, i, c, moment);
                        }
                    }
                }
            }
        }
    } else {
        moment = *options.range;
        if (moment.duration.value <= 0 || moment.start.value < 0) {
            return std::unexpected(invalidArgument("the replay range is empty"));
        }
        for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
            if (kind == TrackKind::Audio && !options.includeAudio) {
                continue;
            }
            const auto& tracks = seq.tracks(kind);
            for (std::size_t i = 0; i < tracks.size(); ++i) {
                const Track& t = *tracks[i];
                for (const Clip& c : t.clips) {
                    if (!c.timelineRange.overlaps(moment) || !replayable(state, c)) {
                        continue;
                    }
                    if (t.locked) {
                        notes.push_back("clip " + std::to_string(c.id.value) + " is on a locked track: not replayed");
                        break;
                    }
                    if (const auto problem = replayProblem(state, c)) {
                        return std::unexpected(invalidArgument("instant replay cannot slow " + *problem));
                    }
                    addPiece(t, i, c, moment);
                }
            }
        }
    }
    if (std::none_of(pieces.begin(), pieces.end(), [](const Piece& p) { return p.kind == TrackKind::Video; })) {
        return std::unexpected(invalidArgument("nothing to replay: no video clip of media in the moment"));
    }

    // The replay's length and its warp.
    const Rate rate = seq.frameRate;
    const Ticks frame = framesToTicks(1, rate);
    const Ticks a = moment.start;
    const Ticks b = moment.end();
    const Ticks length = moment.duration;
    const double l = static_cast<double>(length.value);
    const double s = options.speed;
    const bool ramp = options.ramp && s < 1.0 && length >= Ticks{frame.value * 8};
    Ticks replayLength;
    std::optional<Warp> warp;
    if (ramp) {
        const double r = std::min(l / 4.0, static_cast<double>(kTicksPerSecond) * 0.3);
        Ticks rampReplay = nearestFrame(2.0 * r / (1.0 + s), rate);
        rampReplay = std::max(rampReplay, frame);
        const Ticks middle = std::max(nearestFrame((l - 2.0 * r) / s, rate), frame);
        replayLength = rampReplay + rampReplay + middle;
        warp.emplace(length, replayLength, Ticks{static_cast<std::int64_t>(std::llround(r))}, rampReplay);
    } else {
        replayLength = std::max(nearestFrame(l / s, rate), frame);
        warp.emplace(length, replayLength);
    }

    // Insert the gap and place the retimed copies.
    detail::SequenceEditor ed(state, seq);
    std::set<std::uint64_t> pieceTracks;
    for (const Piece& p : pieces) {
        pieceTracks.insert(p.track.value);
    }
    for (const TrackId id : ed.trackIds()) {
        const Track& t = ed.track(id);
        if (!t.locked && (pieceTracks.contains(id.value) || t.syncLocked)) {
            ed.rippleInsert(id, b, replayLength);
        }
    }
    // The markers follow with the captions (SequenceEditor, 0.4) when asked to: instant replay keeps its own option.
    ed.setMarkersFollow(options.rippleMarkers);
    ed.captionsInsert(b, replayLength);
    std::map<std::uint64_t, LinkGroupId> groups;
    std::map<std::uint64_t, std::size_t> groupSizes;
    for (const Piece& p : pieces) {
        if (p.clip.linkGroup.isValid()) {
            ++groupSizes[p.clip.linkGroup.value];
        }
    }
    ReplayResult result;
    result.moment = moment;
    result.replay = TimeRange{b, replayLength};
    std::size_t remapFallbacks = 0;
    for (const Piece& p : pieces) {
        const double x0 = static_cast<double>((p.clip.start() - a).value);
        const double x1 = static_cast<double>((p.clip.end() - a).value);
        Ticks yFrom = p.clip.start() == a ? Ticks{} : nearestFrame(warp->y(x0), rate);
        Ticks yTo = p.clip.end() == b ? replayLength : nearestFrame(warp->y(x1), rate);
        yFrom = std::clamp(yFrom, Ticks{}, replayLength - frame);
        yTo = std::clamp(yTo, yFrom + frame, replayLength);
        Clip n = p.clip;
        n.id = ed.newClipId();
        n.timelineRange = TimeRange{b + yFrom, yTo - yFrom};
        if (p.clip.linkGroup.isValid() && groupSizes[p.clip.linkGroup.value] > 1) {
            auto g = groups.find(p.clip.linkGroup.value);
            if (g == groups.end()) {
                g = groups.emplace(p.clip.linkGroup.value, ed.newLinkGroup()).first;
            }
            n.linkGroup = g->second;
        } else {
            n.linkGroup = {};
        }
        ClipContent content = p.clip.content();
        content.effects = freshEffects(ed, content.effects);
        const std::optional<Curve> remap =
            p.kind == TrackKind::Video ? replayRemap(p.clip, a, *warp, yFrom, yTo, ed.sourceBounds(p.clip).end) : std::optional<Curve>{};
        if (remap) {
            content.timeRemap = *remap;
            n.speed = Speed{1, 1};
        } else {
            // Audio (remapped audio is silent) and the rare piece whose curve does not hold: a constant speed.
            if (p.kind == TrackKind::Video) {
                ++remapFallbacks;
            }
            const auto speed = speedForDuration(p.clip.sourceDuration(), yTo - yFrom);
            if (!speed) {
                notes.push_back("clip " + std::to_string(p.clip.id.value) + " could not be retimed: left out");
                continue;
            }
            n.speed = *speed;
            n.maintainPitch = false;
        }
        n.setContent(std::move(content));
        result.clips.push_back(n.id);
        ed.placeNewClip(p.track, std::move(n), false);
    }
    if (remapFallbacks > 0) {
        notes.push_back(std::to_string(remapFallbacks) + " video clip(s) replay at a constant speed (no ramp)");
    }
    EditResult edited = ed.finish(b);
    ProjectState working = edited.state;

    // The stinger on a free track above the replay's top video track.
    const Sequence* now = working.findSequence(sequenceId);
    std::size_t topVideo = 0;
    std::size_t mainVideo = seq.videoTracks.size();
    for (const Piece& p : pieces) {
        if (p.kind == TrackKind::Video) {
            topVideo = std::max(topVideo, p.trackIndex);
            mainVideo = std::min(mainVideo, p.trackIndex);
        }
    }
    if (options.label) {
        std::optional<TrackId> track = freeVideoTrackAbove(*now, topVideo, result.replay);
        if (!track) {
            auto added = addTrack(working, sequenceId, TrackKind::Video);
            if (!added) {
                return std::unexpected(added.error());
            }
            working = std::move(added->state);
            track = added->id;
        }
        auto placed = instantiateTemplate(working, sequenceId, *track, b, replayLength, *options.label, options.labelValues);
        if (!placed) {
            return std::unexpected(invalidArgument("the replay stinger: " + placed.error().message));
        }
        working = std::move(placed->state);
        result.label = placed->placed.empty() ? ClipId{} : placed->placed.front();
    }

    // Flashes at the cut into the replay and the cut out of it, on the main replay track.
    if (options.flash) {
        Ticks flashLength = options.flashDuration.value > 0 ? options.flashDuration : Ticks{kTicksPerSecond / 5};
        flashLength = std::max(nearestFrame(static_cast<double>(flashLength.value), rate), Ticks{frame.value * 2});
        const Ticks half = nearestFrame(static_cast<double>(flashLength.value) / 2.0, rate);
        const TrackId mainTrack = seq.videoTracks[mainVideo]->id;
        const auto flashAt = [&](Ticks cut, bool intoReplay) {
            const Sequence* cur = working.findSequence(sequenceId);
            const Track* t = cur->findTrack(mainTrack);
            const Clip* out = clipEndingAt(*t, cut);
            const Clip* in = clipStartingAt(*t, cut);
            const Clip* replayClip = intoReplay ? in : out;
            if (!replayClip || std::find(result.clips.begin(), result.clips.end(), replayClip->id) == result.clips.end()) {
                return;
            }
            TransitionSpec spec;
            spec.effect = "dipToWhite";
            if (out && in) {
                spec.outgoing = out->id;
                spec.incoming = in->id;
                spec.before = std::min(half, out->duration());
                spec.after = std::min(flashLength - half, in->duration());
            } else if (in) {
                spec.incoming = in->id;
                spec.after = std::min(flashLength, in->duration());
            } else {
                spec.outgoing = out->id;
                spec.before = std::min(flashLength, out->duration());
            }
            auto added = addTransition(working, sequenceId, mainTrack, spec, HandleFit::UseAvailable);
            if (!added) {
                notes.push_back(std::string("no flash ") + (intoReplay ? "into" : "out of") + " the replay: " + added.error().message);
                return;
            }
            working = std::move(added->state);
            result.flashes.insert(result.flashes.end(), added->createdTransitions.begin(), added->createdTransitions.end());
        };
        flashAt(b, true);
        flashAt(b + replayLength, false);
    }

    result.edit = std::move(edited);
    result.edit.state = std::move(working);
    result.edit.changes = diff(state, result.edit.state);
    result.edit.created.insert(result.edit.created.end(), result.clips.begin(), result.clips.end());
    if (result.label.isValid()) {
        result.edit.created.push_back(result.label);
    }
    std::sort(result.edit.created.begin(), result.edit.created.end());
    result.edit.created.erase(std::unique(result.edit.created.begin(), result.edit.created.end()), result.edit.created.end());
    result.edit.createdTransitions = result.flashes;
    result.edit.playhead = b;
    result.notes = std::move(notes);
    return result;
}

} // namespace edit

// ---- Zoom punch and shake -----------------------------------------------------------------------------------------------

namespace {

bool isEasing(Interpolation i) { return i != Interpolation::Hold; }

/// A video clip of the sequence that keys may be written on.
std::expected<Sequence::ClipLocation, Error> keyableVideoClip(const Sequence& seq, ClipId id) {
    const auto loc = seq.findClip(id);
    if (!loc) {
        return std::unexpected(notFound("clip " + std::to_string(id.value) + " is not in this sequence"));
    }
    if (seq.trackPosition(loc->track->id)->kind != TrackKind::Video) {
        return std::unexpected(invalidArgument("zoom punch and shake take a video clip"));
    }
    if (loc->track->locked) {
        return std::unexpected(invalidArgument("the clip's track is locked"));
    }
    return *loc;
}

/// `value` with the keys inside curve times [from, to] replaced by `keys` (sorted). A constant becomes a curve.
ParamValue withKeys(const ParamValue& value, Ticks from, Ticks to, const std::vector<Keyframe>& keys) {
    Curve out;
    if (const Curve* c = value.curve()) {
        for (const Keyframe& k : c->keys) {
            if (k.time < from || k.time > to) {
                out.keys.push_back(k);
            }
        }
    }
    out.keys.insert(out.keys.end(), keys.begin(), keys.end());
    std::sort(out.keys.begin(), out.keys.end(), [](const Keyframe& x, const Keyframe& y) { return x.time < y.time; });
    out.keys.erase(std::unique(out.keys.begin(), out.keys.end(), [](const Keyframe& x, const Keyframe& y) { return x.time == y.time; }),
                   out.keys.end());
    return ParamValue(std::move(out));
}

Keyframe key(Ticks time, double value, Interpolation interpolation) {
    Keyframe k;
    k.time = time;
    k.value = value;
    k.interpolation = interpolation;
    return k;
}

/// Sets several intrinsic parameters of one clip as one edit.
edit::EditOutcome setParameters(const ProjectState& state, SequenceId seq, ClipId clip, const std::vector<std::pair<ParamId, ParamValue>>& values) {
    ProjectState working = state;
    for (const auto& [param, value] : values) {
        auto r = edit::setParameter(working, seq, clip, edit::ParamRef::of(param), value);
        if (!r) {
            return std::unexpected(r.error());
        }
        working = std::move(r->state);
    }
    edit::EditResult result;
    result.state = std::move(working);
    result.changes = diff(state, result.state);
    return result;
}

std::uint64_t splitMix64(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/// A uniform double in [0, 1).
double unit(std::uint64_t& state) { return static_cast<double>(splitMix64(state) >> 11) * (1.0 / 9007199254740992.0); }

} // namespace

std::optional<std::string> zoomPunchProblem(const ZoomPunchOptions& o) {
    if (!std::isfinite(o.scale) || o.scale < 1.01 || o.scale > 10.0) {
        return "the zoom must be 1.01..10";
    }
    if (o.duration.value <= 0 || o.duration > Ticks::fromSeconds(30)) {
        return "the punch must last up to 30 s";
    }
    if (!std::isfinite(o.attack) || o.attack < 0.05 || o.attack > 0.9) {
        return "attack must be 0.05..0.9";
    }
    if (!std::isfinite(o.release) || o.release < 0.0 || o.release > 0.9 || o.attack + o.release > 1.0 + 1e-9) {
        return "release must be 0..0.9 and attack + release at most 1";
    }
    if (!isEasing(o.easing)) {
        return "a punch cannot hold (pick linear, easeIn, easeOut, easeInOut or bezier)";
    }
    if (o.focus && (!std::isfinite((*o.focus)[0]) || !std::isfinite((*o.focus)[1]) || (*o.focus)[0] < 0.0 || (*o.focus)[0] > 1.0 ||
                    (*o.focus)[1] < 0.0 || (*o.focus)[1] > 1.0)) {
        return "the focus must be inside the frame (0..1)";
    }
    return std::nullopt;
}

std::optional<std::string> shakeProblem(const ShakeOptions& o) {
    if (!std::isfinite(o.intensity) || o.intensity < 0.5 || o.intensity > 1000.0) {
        return "the intensity must be 0.5..1000 pixels";
    }
    if (o.duration.value <= 0 || o.duration > Ticks::fromSeconds(30)) {
        return "the shake must last up to 30 s";
    }
    if (!std::isfinite(o.frequency) || o.frequency < 1.0 || o.frequency > 60.0) {
        return "the frequency must be 1..60 per second";
    }
    if (!std::isfinite(o.rotation) || o.rotation < 0.0 || o.rotation > 45.0) {
        return "the rotation must be 0..45 degrees";
    }
    return std::nullopt;
}

std::vector<ShakeSample> shakePattern(const ShakeOptions& o, std::uint64_t seed, Rate rate) {
    const Ticks frame = framesToTicks(1, rate);
    const Ticks duration = std::max(nearestFrame(static_cast<double>(o.duration.value), rate), Ticks{frame.value * 2});
    const double perKey = std::max(1.0, std::round(static_cast<double>(kTicksPerSecond) / o.frequency / frameTicks(rate)));
    const std::int64_t stepFrames = static_cast<std::int64_t>(perKey);
    std::vector<ShakeSample> out;
    out.push_back({Ticks{}, 0.0, 0.0, 0.0});
    std::uint64_t state = seed;
    double sign = unit(state) < 0.5 ? -1.0 : 1.0;
    for (std::int64_t f = stepFrames;; f += stepFrames) {
        const Ticks t = framesToTicks(f, rate);
        if (t >= duration) {
            break;
        }
        const double u = static_cast<double>(t.value) / static_cast<double>(duration.value);
        const double decay = (1.0 - u) * (1.0 - u);
        const double rx = sign * (0.45 + 0.55 * unit(state));
        const double ry = 2.0 * unit(state) - 1.0;
        const double rr = 2.0 * unit(state) - 1.0;
        sign = -sign;
        out.push_back({t, o.intensity * decay * rx, o.intensity * decay * ry, o.rotation * decay * rr});
    }
    out.push_back({duration, 0.0, 0.0, 0.0});
    return out;
}

namespace edit {

EditOutcome zoomPunch(const ProjectState& state, SequenceId sequenceId, ClipId clipId, Ticks at, const ZoomPunchOptions& o) {
    const auto found = detail::findSequence(state, sequenceId);
    if (!found) {
        return std::unexpected(found.error());
    }
    if (const auto problem = zoomPunchProblem(o)) {
        return std::unexpected(invalidArgument("zoom punch: " + *problem));
    }
    const Sequence& seq = **found;
    const auto loc = keyableVideoClip(seq, clipId);
    if (!loc) {
        return std::unexpected(loc.error());
    }
    const Clip& clip = loc->clip();
    if (!clip.timelineRange.contains(at)) {
        return std::unexpected(invalidArgument("the punch must hit inside the clip"));
    }
    const Rate rate = seq.frameRate;
    const Ticks frame = framesToTicks(1, rate);
    const double d = static_cast<double>(o.duration.value);
    // The punch's window on the timeline, clamped into the clip (its last frame starts at end - frame).
    const Ticks last = clip.end() - frame;
    const Ticks t0 = std::max(clip.start(), at - nearestFrame(d * o.attack, rate));
    const Ticks t1 = std::min(at, last);
    const Ticks t3 = std::min(last, t0 + std::max(nearestFrame(d, rate), frame));
    const Ticks t2 = std::clamp(t3 - nearestFrame(d * o.release, rate), t1, t3);
    const Interpolation easing = o.easing == Interpolation::Bezier ? Interpolation::EaseInOut : o.easing;
    struct ZoomKey {
        Ticks time;
        double zoom = 1.0;
        Interpolation interpolation = Interpolation::Linear;
    };
    std::vector<ZoomKey> zoom;
    if (t1 > t0) {
        zoom.push_back({t0, 1.0, easing}); // else the punch cuts in at the clip's start
    }
    if (o.release == 0.0 || t2 >= t3) {
        zoom.push_back({t1, o.scale, Interpolation::Hold}); // holds, then cuts back at the end
    } else {
        zoom.push_back({t1, o.scale, easing});
        if (t2 > t1) {
            zoom.push_back({t2, o.scale, easing});
        }
    }
    zoom.push_back({t3, 1.0, easing});
    std::vector<Keyframe> scaleKeys;
    std::vector<Keyframe> xKeys;
    std::vector<Keyframe> yKeys;
    for (const ZoomKey& z : zoom) {
        const Ticks c = clip.curveTimeAt(z.time);
        if (!scaleKeys.empty() && scaleKeys.back().time >= c) {
            continue;
        }
        const double base = clip.parameterAt(ParamId::Scale, z.time);
        scaleKeys.push_back(key(c, base * z.zoom, z.interpolation));
        if (o.focus) {
            const double qx = ((*o.focus)[0] - 0.5) * seq.width;
            const double qy = ((*o.focus)[1] - 0.5) * seq.height;
            const double px = clip.parameterAt(ParamId::PositionX, z.time);
            const double py = clip.parameterAt(ParamId::PositionY, z.time);
            xKeys.push_back(key(c, z.zoom * px + (1.0 - z.zoom) * qx, z.interpolation));
            yKeys.push_back(key(c, z.zoom * py + (1.0 - z.zoom) * qy, z.interpolation));
        }
    }
    if (scaleKeys.size() < 2) {
        return std::unexpected(invalidArgument("the clip is too short for a punch here"));
    }
    const Ticks from = scaleKeys.front().time;
    const Ticks to = scaleKeys.back().time;
    std::vector<std::pair<ParamId, ParamValue>> values{{ParamId::Scale, withKeys(clip.properties()[ParamId::Scale], from, to, scaleKeys)}};
    if (o.focus) {
        values.emplace_back(ParamId::PositionX, withKeys(clip.properties()[ParamId::PositionX], from, to, xKeys));
        values.emplace_back(ParamId::PositionY, withKeys(clip.properties()[ParamId::PositionY], from, to, yKeys));
    }
    return setParameters(state, sequenceId, clipId, values);
}

EditOutcome shake(const ProjectState& state, SequenceId sequenceId, ClipId clipId, Ticks at, const ShakeOptions& o) {
    const auto found = detail::findSequence(state, sequenceId);
    if (!found) {
        return std::unexpected(found.error());
    }
    if (const auto problem = shakeProblem(o)) {
        return std::unexpected(invalidArgument("shake: " + *problem));
    }
    const Sequence& seq = **found;
    const auto loc = keyableVideoClip(seq, clipId);
    if (!loc) {
        return std::unexpected(loc.error());
    }
    const Clip& clip = loc->clip();
    if (!clip.timelineRange.contains(at)) {
        return std::unexpected(invalidArgument("the shake must start inside the clip"));
    }
    const Rate rate = seq.frameRate;
    const Ticks last = clip.end() - framesToTicks(1, rate);
    std::uint64_t seed = o.seed;
    if (seed == 0) {
        std::uint64_t mix = clipId.value * 0x9E3779B97F4A7C15ull ^ static_cast<std::uint64_t>(at.value);
        seed = splitMix64(mix) | 1u;
    }
    const std::vector<ShakeSample> pattern = shakePattern(o, seed, rate);
    const double minSide = static_cast<double>(std::max(1, std::min(seq.width, seq.height)));
    const double maxSide = static_cast<double>(std::max(seq.width, seq.height));
    std::vector<Keyframe> xKeys;
    std::vector<Keyframe> yKeys;
    std::vector<Keyframe> rKeys;
    std::vector<Keyframe> sKeys;
    const double pi = std::acos(-1.0);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const ShakeSample& sample = pattern[i];
        const Ticks t = std::min(at + sample.time, last);
        const Ticks c = clip.curveTimeAt(t);
        if (!xKeys.empty() && xKeys.back().time >= c) {
            break; // the clip ends: the shake is cut short there (its last key already holds the base)
        }
        const bool edge = i == 0 || i + 1 == pattern.size() || t == last;
        const double dx = edge ? 0.0 : sample.dx;
        const double dy = edge ? 0.0 : sample.dy;
        const double dr = edge ? 0.0 : sample.rotation;
        xKeys.push_back(key(c, clip.parameterAt(ParamId::PositionX, t) + dx, Interpolation::Linear));
        yKeys.push_back(key(c, clip.parameterAt(ParamId::PositionY, t) + dy, Interpolation::Linear));
        if (o.rotation > 0.0) {
            rKeys.push_back(key(c, clip.parameterAt(ParamId::Rotation, t) + dr, Interpolation::Linear));
        }
        if (o.cover) {
            // Enough zoom that the largest offset (and rotation) left from here on cannot show the clip's edges.
            const double u = static_cast<double>(sample.time.value) / static_cast<double>(pattern.back().time.value);
            const double decay = edge ? 0.0 : (1.0 - u) * (1.0 - u);
            const double cover = 1.0 + decay * (2.0 * o.intensity / minSide + std::sin(o.rotation * pi / 180.0) * maxSide / minSide);
            sKeys.push_back(key(c, clip.parameterAt(ParamId::Scale, t) * cover, Interpolation::Linear));
        }
    }
    if (xKeys.size() < 2) {
        return std::unexpected(invalidArgument("the clip is too short for a shake here"));
    }
    const Ticks from = xKeys.front().time;
    const Ticks to = xKeys.back().time;
    std::vector<std::pair<ParamId, ParamValue>> values{
        {ParamId::PositionX, withKeys(clip.properties()[ParamId::PositionX], from, to, xKeys)},
        {ParamId::PositionY, withKeys(clip.properties()[ParamId::PositionY], from, to, yKeys)}};
    if (!rKeys.empty()) {
        values.emplace_back(ParamId::Rotation, withKeys(clip.properties()[ParamId::Rotation], from, to, rKeys));
    }
    if (!sKeys.empty()) {
        values.emplace_back(ParamId::Scale, withKeys(clip.properties()[ParamId::Scale], from, to, sKeys));
    }
    return setParameters(state, sequenceId, clipId, values);
}

} // namespace edit

} // namespace wabcut
