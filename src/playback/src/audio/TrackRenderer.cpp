#include <wabcut/playback/audio/TrackRenderer.h>

#include "Fader.h"
#include "NestedMix.h"

#include <wabcut/playback/audio/ChannelLayout.h>

#include <wabcut/audio/Gain.h>
#include <wabcut/audio/TimeMath.h>
#include <wabcut/audio/dsp/SpeedRender.h>
#include <wabcut/audio/effects/EffectChain.h>

#include <wabcut/model/AudioChannelOps.h>
#include <wabcut/model/TimeRemap.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

namespace wabcut::playback {

using audio::timemath::mulDivSaturate;

namespace {

/// Index of the first clip that ends after `t`.
std::size_t firstClipEndingAfter(const ClipList& clips, Ticks t) {
    const std::size_t after = clips.upperBoundStart(t);
    return (after > 0 && clips[after - 1].end() > t) ? after - 1 : after;
}

/// A.10: a media clip sounds at any speed (forwards or reversed, a constant speed or a remap: SpeedRender.h); a nested
/// or multicam clip only at 100 % (a remapped one in its 100 % spans). Freeze frames are silent.
bool playable(const Clip& clip) {
    return clip.enabled && clip.stream.kind == StreamKind::Audio && !clip.hasUnknownSource() && !clip.isFreeze() &&
           (clip.isRemapped() || clip.speed.isUnity() || !clip.isNested());
}

/// v / d rounded down (d > 0).
std::int64_t divFloor(std::int64_t v, std::int64_t d) { return v >= 0 ? v / d : -((-v + d - 1) / d); }

/// A.10: the source position of every frame of a run (dsp::SpeedPositions): its first clip's remap curve, or its
/// constant speed extended past its ends, in source frames at the render rate. Reverse plays the frame that ends at
/// the position (TimeRemap.h), so a 100 % reverse reads exactly the frames a reversed clip reads. Inside a ramp the
/// curve is evaluated on a 32-frame grid of absolute frames and interpolated between (a ramp's position is smooth; the
/// grid makes a frame's position independent of the block it is rendered in); straight segments are exact.
class RunPositions final : public audio::dsp::SpeedPositions {
public:
    /// Absolute frame F plays at timeline time origin + (F - base) * tpf.
    RunPositions(const Clip& clip, Ticks origin, std::int64_t base, std::int64_t tpf)
        : m_clip(clip), m_remap(clip.timeRemap()), m_origin(origin), m_base(base), m_tpf(tpf) {}

    void fill(std::int64_t first, int count, double* out) const override {
        for (int i = 0; i < count; ++i) {
            out[i] = at(first + i);
        }
    }

private:
    static constexpr std::int64_t kCell = 32;

    Ticks timeOf(std::int64_t frame) const { return m_origin + Ticks{(frame - m_base) * m_tpf}; }

    double at(std::int64_t frame) const {
        const Ticks t = timeOf(frame);
        if (m_remap == nullptr) {
            const double offset = static_cast<double>((t - m_clip.start()).value) * static_cast<double>(m_clip.speed.num) /
                                  static_cast<double>(m_clip.speed.den);
            const double tpf = static_cast<double>(m_tpf);
            return m_clip.reversed ? (static_cast<double>(m_clip.sourceOut().value) - offset) / tpf - 1.0
                                   : (static_cast<double>(m_clip.sourceIn.value) + offset) / tpf;
        }
        const Ticks c = m_clip.curveTimeAt(t);
        const RemapSegment& s = segmentAt(c);
        const double shift = s.motion == RemapMotion::Reverse ? 1.0 : 0.0;
        if (s.straight || s.motion == RemapMotion::Hold) {
            return frames(remapPositionAt(*m_remap, c)) - shift;
        }
        // A ramp: the grid's cell around the frame, when both its corners lie in the same segment.
        const std::int64_t f0 = divFloor(frame, kCell) * kCell;
        if (f0 != m_cell) {
            m_cell = f0;
            const Ticks c0 = m_clip.curveTimeAt(timeOf(f0));
            const Ticks c1 = m_clip.curveTimeAt(timeOf(f0 + kCell));
            m_cellUsable = c0 >= s.from && c1 < s.to;
            if (m_cellUsable) {
                m_cellFrom = frames(remapPositionAt(*m_remap, c0));
                m_cellTo = frames(remapPositionAt(*m_remap, c1));
            }
        }
        if (!m_cellUsable || c < s.from || c >= s.to) {
            return frames(remapPositionAt(*m_remap, c)) - shift;
        }
        const double u = static_cast<double>(frame - f0) / static_cast<double>(kCell);
        return m_cellFrom + (m_cellTo - m_cellFrom) * u - shift;
    }

    double frames(Ticks position) const { return static_cast<double>(position.value) / static_cast<double>(m_tpf); }

    const RemapSegment& segmentAt(Ticks c) const {
        if (!m_haveSegment || c < m_segment.from || c >= m_segment.to) {
            m_segment = remapSegmentAt(*m_remap, c);
            m_haveSegment = true;
            m_cell = std::numeric_limits<std::int64_t>::min(); // a cell checked against another segment
        }
        return m_segment;
    }

    const Clip& m_clip;
    const Curve* m_remap;
    Ticks m_origin;
    std::int64_t m_base;
    std::int64_t m_tpf;
    mutable RemapSegment m_segment;
    mutable bool m_haveSegment = false;
    mutable std::int64_t m_cell = std::numeric_limits<std::int64_t>::min();
    mutable bool m_cellUsable = false;
    mutable double m_cellFrom = 0.0;
    mutable double m_cellTo = 0.0;
};

bool hasRunnableEffects(const EffectStack& stack) {
    return std::any_of(stack.begin(), stack.end(), [](const EffectInstance& e) {
        return e.enabled && audio::isImplementedAudioEffect(e.effect) && isEvaluable(e, EffectKind::Audio);
    });
}

/// A.4: blends `take` into `dst` (frames [from, to) of the stream, interleaved) by `strength` (a Fraction: 0 plays the
/// source, 1 the take; null = the default, 1) on the parameter grid like the clip's Volume: values at cell boundaries,
/// linear between, so preview and export blend identically whatever their block sizes. At 1 the take's samples are
/// copied exactly.
template <typename CurveTimeOf>
void blendTake(const ParamValue* strength, std::int64_t from, std::int64_t to, float* dst, const float* take, int channels,
               CurveTimeOf curveTimeOf) noexcept {
    const auto ch = static_cast<std::size_t>(channels);
    const auto at = [&](std::int64_t f) { return strength != nullptr ? std::clamp(strength->valueAt(curveTimeOf(f)), 0.0, 1.0) : 1.0; };
    const auto mix = [&](std::size_t frame, float s) {
        float* d = dst + frame * ch;
        const float* t = take + frame * ch;
        if (s >= 1.0f) {
            std::copy_n(t, ch, d);
        } else if (s > 0.0f) {
            for (std::size_t c = 0; c < ch; ++c) {
                d[c] = d[c] * (1.0f - s) + t[c] * s;
            }
        }
    };
    if (strength == nullptr || strength->isConstant()) {
        const auto s = static_cast<float>(at(from));
        for (std::int64_t f = from; f < to; ++f) {
            mix(static_cast<std::size_t>(f - from), s);
        }
        return;
    }
    constexpr std::int64_t cell = audio::kParamCellFrames;
    for (std::int64_t f = from; f < to;) {
        const std::int64_t c0 = (f >= 0 ? f / cell : -((-f + cell - 1) / cell)) * cell;
        const std::int64_t c1 = c0 + cell;
        const double a = at(c0);
        const double b = at(c1);
        const std::int64_t end = std::min(to, c1);
        for (; f < end; ++f) {
            const double p = static_cast<double>(f - c0) / static_cast<double>(cell);
            mix(static_cast<std::size_t>(f - from), static_cast<float>(a + (b - a) * p));
        }
    }
}

const Transition* headTransition(const Track& track, ClipId clip) {
    for (const Transition& t : track.transitions) {
        if (t.incoming == clip) {
            return &t;
        }
    }
    return nullptr;
}

const Transition* tailTransition(const Track& track, ClipId clip) {
    for (const Transition& t : track.transitions) {
        if (t.outgoing == clip) {
            return &t;
        }
    }
    return nullptr;
}

/// Whether `next` plays on in the same voice as `prev`: a through edit of forward clips with the same effects and no
/// transition on the edit point.
bool sameVoice(const Track& track, const Clip& prev, const Clip& next) {
    return !prev.reversed && !next.reversed && playable(prev) && playable(next) && continuesSeamlessly(prev, next) &&
           (prev.sharesContentWith(next) || prev.effects() == next.effects()) &&
           tailTransition(track, prev.id) == nullptr && headTransition(track, next.id) == nullptr;
}

/// C.8: `prev` and `next` are a multicam clip cut where it switches angle (multicam.switchAngle): the same source,
/// contiguous at 100 % forwards, other angles, no transition there. Such a cut crossfades the angles.
bool angleSwitch(const Track& track, const Clip& prev, const Clip& next) {
    const auto straight = [](const Clip& c) { return playable(c) && c.speed.isUnity() && !c.reversed && !c.isRemapped(); };
    return prev.isNested() && next.sequence == prev.sequence && !prev.angles.empty() && !next.angles.empty() &&
           prev.angles != next.angles && prev.stream == next.stream && prev.end() == next.start() &&
           prev.sourceOut() == next.sourceIn && straight(prev) && straight(next) && tailTransition(track, prev.id) == nullptr &&
           headTransition(track, next.id) == nullptr;
}

/// Gain of the incoming (`incoming` = true) or outgoing side of an audio transition at progress p in [0, 1].
double transitionGain(const Transition& t, double p, bool incoming) {
    p = std::clamp(p, 0.0, 1.0);
    if (t.effect == "constantGain") {
        return incoming ? p : 1.0 - p;
    }
    if (t.effect == "exponentialFade") {
        // 0.2, Premiere's Exponential Fade: each side over a smooth logarithmic curve, g(x) = log10(1 + 9x), x = how far
        // in the side is (1 at full level): like Constant Power (A falls slowly at first, B rises fast at first) but more
        // gradual; both are 0.74 (-2.6 dB) at the middle.
        const double x = incoming ? p : 1.0 - p;
        return std::log10(1.0 + 9.0 * x);
    }
    return incoming ? std::sin(p * std::numbers::pi / 2.0) : std::cos(p * std::numbers::pi / 2.0); // constantPower
}

/// Source time a clip reads at timeline time `t`, extended past the clip's edges (transition handles) without the
/// clamping Clip::sourceTimeAt does.
Ticks sourceTimeExtended(const Clip& clip, Ticks t) {
    if (clip.isRemapped()) {
        return clip.sourceTimeAt(t);
    }
    const Ticks offset = toSourceTicks(t - clip.start(), clip.speed);
    return clip.reversed ? clip.sourceOut() - Ticks{1} - offset : clip.sourceIn + offset;
}

} // namespace

struct TrackRenderer::Impl {
    struct Voice {
        ClipId key;                 ///< the run's first clip
        audio::EffectChain chain;
        std::int64_t nextInput = 0; ///< next input frame for the chain
        bool primed = false;        ///< has processed input (nextInput is meaningful)
        bool seen = false;

        /// C.8: the sound of a nested or multicam run (its source instead of media).
        std::unique_ptr<detail::NestedMix> nested;

        /// A.10: the speed renderers of the run's pieces (SpeedPlan), by the piece's first frame (absolute).
        struct Speed {
            std::int64_t begin = 0;
            audio::dsp::SpeedMode mode = audio::dsp::SpeedMode::Varispeed;
            std::unique_ptr<audio::dsp::SpeedRenderer> renderer;
            bool used = false;
        };
        std::vector<Speed> speed;

        Voice(ClipId k, int channels, int sampleRate) : key(k), chain(channels, sampleRate) {}
    };

    /// A.10: how a run's audio follows its source position (audio/dsp/SpeedRender.h). Inactive (the plain path) for
    /// clips at 100 % forwards or reversed, and for nested and multicam clips (heard at 100 % only).
    struct SpeedPlan {
        bool active = false;
        audio::dsp::SpeedMode mode = audio::dsp::SpeedMode::Varispeed;
        bool remapped = false;
        /// Frames [first, second) read straight from the media: a remap's 100 % forward spans.
        std::vector<std::pair<std::int64_t, std::int64_t>> direct;
        /// Frames rendered along the position: a constant speed's whole run; a remap's ramps, other speeds and reverse
        /// segments (holds are silent), with 100 % gaps shorter than minimumPieceGap folded in.
        std::vector<std::pair<std::int64_t, std::int64_t>> pieces;
        /// Where the run sounds (direct and pieces merged): the edge envelope silences the rest and fades its edges.
        std::vector<std::pair<std::int64_t, std::int64_t>> audible;
    };

    /// A run of clips sharing a voice, and where its output may sound (transition handles included).
    struct Run {
        std::size_t first = 0;
        std::size_t last = 0;
        Ticks outStart;
        Ticks outEnd;
        const Transition* head = nullptr;
        const Transition* tail = nullptr;
        bool headSwitch = false; ///< C.8: crossfades in from the angle before (angleSwitch)
        bool tailSwitch = false; ///< crossfades out into the angle after
    };

    Impl(int ch, Rate r, TrackRendererOptions o)
        : busChannels(ch), channels(ch), rate(r), tpf(r.ticksPerUnit()), options(o),
          trackChain(ch, static_cast<int>(r.num / std::max(r.den, 1))) {}

    const int busChannels; ///< what render() hands out
    /// What the voices, the track chain and the mix buffers run at: the bus's, or the track's own channels when its
    /// 5.1 panner places it on the bus last (pansLast). Set by restart().
    int channels;
    const Rate rate;
    const std::int64_t tpf;
    const TrackRendererOptions options;

    bool started = false;
    TrackId trackId;
    Ticks next;                 ///< the start a continuing render() has
    Ticks origin;               ///< timeline time of frame 0
    std::int64_t cursor = 0;    ///< frame of `next`
    std::int64_t trackInput = 0; ///< next input frame of the track chain
    audio::EffectChain trackChain;
    std::vector<std::unique_ptr<Voice>> voices;
    std::shared_ptr<const AudioRedactions> redactions;
    std::shared_ptr<const AudioTakes> takes;
    std::vector<float> take; ///< an enhanced take's samples for one read
    std::shared_ptr<const ProjectState> project; ///< C.8: where nested and multicam clips find their sequences
    std::vector<ClipId> nesting;                 ///< the clips leading to this track (setNesting)
    std::vector<ClipId> childPath;               ///< nesting + the nested clip being read

    std::vector<float> mix;
    std::vector<float> routeSource; ///< A.9: a routed read at the source's channel count
    std::vector<float> routeClip;   ///< A.9: the same in the clip's channels
    std::vector<float> voiceIn;
    std::vector<float> reversed;
    std::vector<float> discard;
    std::vector<float> trackOut; ///< a block at the track's channels before its 5.1 panner (pansLast)
    std::vector<float> pieceOut; ///< A.10: one speed piece's output for a read

    int sampleRate() const { return static_cast<int>(rate.num / std::max(rate.den, 1)); }
    /// Review A5-P2-10: a mono or stereo track on a 5.1 bus runs its clips, their effects and its own effects at the
    /// track's channel count, and its 5.1 panner places the result on the bus as the last step (Premiere's order):
    /// stereo effects see the track's channels, not the puck's spread over five speakers (a reverb never feeds the
    /// LFE, a plug-in gets the channels it takes).
    bool pansLast(const Track& track) const {
        return formatForChannels(busChannels) == ChannelFormat::Surround51 &&
               (track.format == ChannelFormat::Mono || track.format == ChannelFormat::Stereo);
    }
    int channelsFor(const Track& track) const { return pansLast(track) ? channelCount(track.format) : busChannels; }
    Ticks timeOf(std::int64_t f) const { return origin + Ticks{f * tpf}; }
    /// First frame at or after `t`.
    std::int64_t frameAtOrAfter(Ticks t) const {
        const std::int64_t d = (t - origin).value;
        return d >= 0 ? mulDivSaturate(d, 1, tpf, Rounding::Ceil) : -mulDivSaturate(-d, 1, tpf, Rounding::Floor);
    }
    std::int64_t edgeFadeFrames() const {
        return options.edgeFade.value > 0 ? mulDivSaturate(options.edgeFade.value, 1, tpf, Rounding::Nearest) : 0;
    }
    /// C.8: an angle switch's crossfade, frames: switchLead() of them before the cut, the rest after it.
    std::int64_t switchFrames() const {
        return options.angleCrossfade.value > 0 ? mulDivSaturate(options.angleCrossfade.value, 1, tpf, Rounding::Nearest) : 0;
    }
    std::int64_t switchLead() const { return switchFrames() / 2; }
    /// Whether the cut between clips[i] and clips[i + 1] is an angle switch that crossfades (both clips longer than it).
    bool crossfadesAfter(const Track& track, std::size_t i) const {
        const ClipList& clips = track.clips;
        const Ticks length{switchFrames() * tpf};
        return length.value > 0 && i + 1 < clips.size() && clips[i].duration() > length && clips[i + 1].duration() > length &&
               angleSwitch(track, clips[i], clips[i + 1]);
    }
    /// Whether clips[i] sounds after its end: in a transition's tail handle or an angle switch's crossfade.
    bool soundsPastEnd(const Track& track, std::size_t i) const {
        return tailTransition(track, track.clips[i].id) != nullptr || crossfadesAfter(track, i);
    }

    Run runAt(const Track& track, std::size_t index) const {
        const ClipList& clips = track.clips;
        Run run;
        run.first = index;
        while (run.first > 0 && sameVoice(track, clips[run.first - 1], clips[run.first])) {
            --run.first;
        }
        run.last = index;
        while (run.last + 1 < clips.size() && sameVoice(track, clips[run.last], clips[run.last + 1])) {
            ++run.last;
        }
        const Clip& first = clips[run.first];
        const Clip& last = clips[run.last];
        run.outStart = first.start();
        run.outEnd = last.end();
        run.head = headTransition(track, first.id);
        run.tail = tailTransition(track, last.id);
        if (run.head && run.head->outgoing.isValid()) {
            run.outStart = first.start() - run.head->before; // two-sided: B starts early from its head handle
        }
        if (run.tail && run.tail->incoming.isValid()) {
            run.outEnd = last.end() + run.tail->after; // two-sided: A plays on into its tail handle
        }
        // C.8: an angle switch crossfades around the cut, both angles playing through it.
        run.headSwitch = run.head == nullptr && run.first > 0 && crossfadesAfter(track, run.first - 1);
        run.tailSwitch = run.tail == nullptr && crossfadesAfter(track, run.last);
        if (run.headSwitch) {
            run.outStart = first.start() - Ticks{switchLead() * tpf};
        }
        if (run.tailSwitch) {
            run.outEnd = last.end() + Ticks{(switchFrames() - switchLead()) * tpf};
        }
        return run;
    }

    Voice& voiceFor(ClipId key) {
        for (auto& v : voices) {
            if (v->key == key) {
                return *v;
            }
        }
        voices.push_back(std::make_unique<Voice>(key, channels, sampleRate()));
        return *voices.back();
    }

    /// C.8: `count` frames of a nested or multicam clip's sound from nested time `readFrom` into `target` (reversed:
    /// the frames from readFrom forwards, which the caller turns around).
    std::size_t readNested(Voice& voice, const Clip& clip, Ticks readFrom, float* target, std::size_t count, AudioFeed& feed,
                           TrackRenderStats& stats) {
        if (!project) {
            return 0;
        }
        if (!voice.nested) {
            voice.nested = std::make_unique<detail::NestedMix>(channels, rate, options);
        }
        childPath.assign(nesting.begin(), nesting.end());
        childPath.push_back(clip.id);
        const detail::NestContext context{&project, &redactions, &takes, childPath};
        const std::span<float> out(target, count * static_cast<std::size_t>(channels));
        if (clip.reversed) {
            voice.nested->readReversed(context, clip, clip.sourceIn, readFrom, out, feed, stats);
        } else {
            voice.nested->read(context, clip, readFrom, out, feed, stats);
        }
        return count;
    }

    /// A.9: how a clip's channels reach the renderer's channels when they do not simply decode to them: the source read
    /// at its own channel count, the channel map (sources averaged, mutes, per-channel volume), then the clip's format
    /// to the track's and, unless the track's 5.1 panner places it later (pansLast), the track's to the bus.
    struct Routing {
        int sourceChannels = 2;
        AudioChannelMap map; ///< the effective map (the identity of the stream when the clip has none)
        ChannelMatrix matrix; ///< render channels x clip channels
    };

    const MediaItem* mediaOf(const Clip& clip) const { return project ? project->findMedia(clip.media) : nullptr; }

    /// The routing of `clip` on `track`, or nullopt for the plain path: no map, a stereo track rendered at one or two
    /// channels. The plain path decodes the stream to those channels as before A.9.
    std::optional<Routing> routingFor(const Track& track, const Clip& clip) const {
        const MediaItem* media = mediaOf(clip);
        const AudioChannelMap* map = edit::effectiveChannelMap(clip, media);
        if (map == nullptr && track.format == ChannelFormat::Stereo && channels <= 2) {
            return std::nullopt;
        }
        Routing r;
        int native = 0;
        if (media != nullptr && clip.stream.index >= 0 && static_cast<std::size_t>(clip.stream.index) < media->info.audio.size()) {
            native = media->info.audio[static_cast<std::size_t>(clip.stream.index)].channels;
        }
        if (native <= 0) { // not probed: as many as the map reads, else the bus's
            native = channels;
            if (map != nullptr) {
                native = 1;
                for (const ChannelRoute& route : map->channels) {
                    for (const std::int32_t s : route.sources) {
                        native = std::max(native, s + 1);
                    }
                }
            }
        }
        r.sourceChannels = std::clamp(native, 1, kMaxSourceChannels);
        r.map = map != nullptr ? *map : AudioChannelMap::identity(r.sourceChannels);
        const int clipChannels = r.map.channelCount();
        const ChannelFormat trackFormat = track.format;
        const int trackChannels = trackFormat == ChannelFormat::Adaptive ? clipChannels : channelCount(trackFormat);
        const ChannelMatrix toTrack = formatMatrix(r.map.format, clipChannels, trackFormat, trackChannels);
        if (pansLast(track)) {
            r.matrix = toTrack; // the renderer runs at the track's channels; panToBus places them on the bus
            return r;
        }
        r.matrix = formatMatrix(trackFormat, trackChannels, formatForChannels(channels), channels) * toTrack;
        return r;
    }

    /// Reads `count` frames through `r` into `target` (bus channels, source order): `read(span, channels)` fills a span
    /// at a channel count (the source, or an enhanced take). frameOf(k) is the output frame of read frame k (the
    /// per-channel volume's grid position).
    template <class Read, class FrameOf>
    std::expected<std::size_t, Error> readRouted(const Routing& r, const Clip& clip, Read read, float* target, std::size_t count,
                                                 FrameOf frameOf) {
        const auto sc = static_cast<std::size_t>(r.sourceChannels);
        const auto cc = static_cast<std::size_t>(r.map.channelCount());
        routeSource.assign(count * sc, 0.0f);
        const auto got = read(std::span<float>(routeSource.data(), count * sc), r.sourceChannels);
        if (!got) {
            std::fill(target, target + count * static_cast<std::size_t>(channels), 0.0f);
            return got;
        }
        const std::size_t have = std::min(*got, count);
        routeClip.assign(count * cc, 0.0f);
        constexpr std::int64_t cell = audio::kParamCellFrames;
        for (std::size_t j = 0; j < cc; ++j) {
            const ChannelRoute& route = r.map.channels[j];
            if (route.muted || route.sources.empty()) {
                continue;
            }
            const float scale = 1.0f / static_cast<float>(route.sources.size());
            for (std::size_t k = 0; k < have; ++k) {
                float sum = 0.0f;
                for (const std::int32_t s : route.sources) {
                    if (static_cast<std::size_t>(s) < sc) {
                        sum += routeSource[k * sc + static_cast<std::size_t>(s)];
                    }
                }
                routeClip[k * cc + j] = route.sources.size() == 1 ? sum : sum * scale;
            }
            if (route.gainDb == ParamValue()) {
                continue;
            }
            // Premiere's Channel Volume: on the parameter grid like the clip's level (values at cell points, ramps between).
            const auto gainAt = [&](std::int64_t f) {
                return detail::faderGain(route.gainDb.valueAt(clip.curveTimeAt(timeOf(f))));
            };
            if (route.gainDb.isConstant()) {
                const auto g = static_cast<float>(gainAt(0));
                for (std::size_t k = 0; k < have; ++k) {
                    routeClip[k * cc + j] *= g;
                }
                continue;
            }
            std::int64_t cached = std::numeric_limits<std::int64_t>::min();
            double a = 1.0;
            double b = 1.0;
            for (std::size_t k = 0; k < have; ++k) {
                const std::int64_t f = frameOf(k);
                const std::int64_t c0 = (f >= 0 ? f / cell : -((-f + cell - 1) / cell)) * cell;
                if (c0 != cached) {
                    cached = c0;
                    a = gainAt(c0);
                    b = gainAt(c0 + cell);
                }
                const double u = static_cast<double>(f - c0) / cell;
                routeClip[k * cc + j] *= static_cast<float>(a + (b - a) * u);
            }
        }
        r.matrix.apply(routeClip.data(), target, count);
        return have;
    }

    /// The absolute frame of frame 0 (the speed renderers' grid: timeline frames when the origin is on a sample).
    std::int64_t absoluteBase() const { return divFloor(origin.value, tpf); }

    /// A.10: the run's speed plan over its sounding range (see SpeedPlan).
    SpeedPlan planOf(const Track& track, const Run& run) const {
        SpeedPlan plan;
        const Clip& first = track.clips[run.first];
        if (first.isNested() || !playable(first) || (!first.isRemapped() && first.speed.isUnity())) {
            return plan;
        }
        plan.active = true;
        plan.mode = first.maintainPitch ? audio::dsp::SpeedMode::MaintainPitch : audio::dsp::SpeedMode::Varispeed;
        plan.remapped = first.isRemapped();
        const std::int64_t a = frameAtOrAfter(run.outStart);
        const std::int64_t b = frameAtOrAfter(run.outEnd);
        if (!plan.remapped) {
            if (b > a) {
                plan.pieces.emplace_back(a, b);
                plan.audible = plan.pieces;
            }
            return plan;
        }
        // A remapped clip runs alone (continuesSeamlessly): its curve over the run's sounding range, handles included,
        // so transitions hear what the curve plays there (review A10: handles follow the remap policy too).
        using Spans = std::vector<std::pair<std::int64_t, std::int64_t>>;
        const auto add = [](Spans& list, std::int64_t x, std::int64_t y) {
            if (y <= x) {
                return;
            }
            if (!list.empty() && list.back().second == x) {
                list.back().second = y;
            } else {
                list.emplace_back(x, y);
            }
        };
        const Ticks c0 = first.curveTimeAt(run.outStart);
        const Ticks c1 = first.curveTimeAt(run.outEnd);
        Spans direct;
        Spans pieces;
        for (const RemapSegment& s : remapSegments(*first.timeRemap())) {
            const Ticks from = std::max(c0, s.from);
            const Ticks to = std::min(c1, s.to);
            if (to <= from || s.motion == RemapMotion::Hold) {
                continue;
            }
            const std::int64_t x = frameAtOrAfter(first.start() + (from - first.sourceIn));
            const std::int64_t y = frameAtOrAfter(first.start() + (to - first.sourceIn));
            add(s.isUnitySpeed() ? direct : pieces, x, y);
        }
        // Fold a 100 % stretch between two pieces into them when it is too short for two joins; the frames between must
        // all be direct (a hold between pieces keeps them apart: it is silent).
        const std::int64_t gap = audio::dsp::minimumPieceGap(plan.mode, static_cast<double>(sampleRate()));
        const auto directCovers = [&](std::int64_t x, std::int64_t y) {
            return std::ranges::any_of(direct, [&](const auto& d) { return d.first <= x && d.second >= y; });
        };
        for (const auto& p : pieces) {
            if (!plan.pieces.empty() && p.first - plan.pieces.back().second < gap &&
                directCovers(plan.pieces.back().second, p.first)) {
                plan.pieces.back().second = p.second;
            } else {
                plan.pieces.push_back(p);
            }
        }
        for (const auto& d : direct) { // what the folded pieces now render is not read straight as well
            std::int64_t x = d.first;
            for (const auto& p : plan.pieces) {
                if (p.first < d.second && p.second > x) {
                    add(plan.direct, x, std::max(x, p.first));
                    x = std::max(x, p.second);
                }
            }
            add(plan.direct, x, d.second);
        }
        std::vector<std::pair<std::int64_t, std::int64_t>> all = plan.direct;
        all.insert(all.end(), plan.pieces.begin(), plan.pieces.end());
        std::ranges::sort(all);
        for (const auto& s : all) {
            if (!plan.audible.empty() && s.first <= plan.audible.back().second) {
                plan.audible.back().second = std::max(plan.audible.back().second, s.second);
            } else {
                plan.audible.push_back(s);
            }
        }
        return plan;
    }

    /// A.10: the source of a run's pieces (dsp::SpeedSource): the clip's media read forwards at source frames, routed,
    /// with its enhanced take blended in and redactions applied in media time (so a redacted word stays muted or
    /// bleeped at any speed). Channel Volume and the take's Strength are taken where the piece starts (constant over
    /// the piece: a piece's source frames have no single timeline time).
    class PieceSource final : public audio::dsp::SpeedSource {
    public:
        PieceSource(Impl& d, const Track& track, const Clip& clip, std::int64_t pieceFrame, AudioFeed& feed,
                    TrackRenderStats& stats)
            : m_d(d), m_clip(clip), m_frame(pieceFrame), m_feed(feed), m_stats(stats),
              m_routing(d.routingFor(track, clip)) {
            const EffectInstance* enhance = d.takes ? enhanceSpeechEffect(clip.effects()) : nullptr;
            m_enhanced = enhance ? d.takes->find(clip.media, clip.stream.index, kEnhanceSpeechTake) : nullptr;
            if (m_enhanced != nullptr) {
                const ParamValue* strength = enhance->find("strength");
                m_strength = strength != nullptr
                                 ? std::clamp(strength->valueAt(clip.curveTimeAt(d.timeOf(pieceFrame))), 0.0, 1.0)
                                 : 1.0;
            }
        }

        void read(std::int64_t first, std::int64_t count, float* dst) override {
            const auto ch = static_cast<std::size_t>(m_d.channels);
            std::fill(dst, dst + static_cast<std::size_t>(count) * ch, 0.0f);
            const std::int64_t a = std::max<std::int64_t>(first, 0); // media time starts at 0
            if (a >= first + count) {
                return;
            }
            const auto n = static_cast<std::size_t>(first + count - a);
            float* out = dst + static_cast<std::size_t>(a - first) * ch;
            const Ticks at{a * m_d.tpf};
            const auto frameOf = [&](std::size_t) { return m_frame; };
            const auto got = m_routing ? m_d.readRouted(*m_routing, m_clip,
                                                        [&](std::span<float> s, int c) {
                                                            return m_feed.read(m_clip.media, m_clip.stream.index, at, s, c, m_d.rate);
                                                        },
                                                        out, n, frameOf)
                                       : m_feed.read(m_clip.media, m_clip.stream.index, at, std::span<float>(out, n * ch),
                                                     m_d.channels, m_d.rate);
            if (!got) {
                ++m_stats.feedErrors;
                std::fill(out, out + n * ch, 0.0f);
            } else if (*got < n) {
                std::fill(out + *got * ch, out + n * ch, 0.0f); // past the media's end: silence (not a short read)
            }
            if (m_enhanced != nullptr && m_strength > 0.0) {
                m_d.take.resize(n * ch);
                const auto tread =
                    m_routing ? m_d.readRouted(*m_routing, m_clip,
                                               [&](std::span<float> s, int c) { return m_feed.readTake(m_enhanced->path, at, s, c, m_d.rate); },
                                               m_d.take.data(), n, frameOf)
                              : m_feed.readTake(m_enhanced->path, at, std::span<float>(m_d.take.data(), n * ch), m_d.channels,
                                                m_d.rate);
                if (!tread) {
                    ++m_stats.takeErrors;
                } else {
                    const auto s = static_cast<float>(m_strength);
                    const std::size_t have = std::min(*tread, n) * ch;
                    for (std::size_t k = 0; k < have; ++k) {
                        out[k] = s >= 1.0f ? m_d.take[k] : out[k] * (1.0f - s) + m_d.take[k] * s;
                    }
                }
            }
            if (m_d.redactions) {
                m_d.redactions->apply(m_clip.media, m_clip.id, at, m_d.tpf, static_cast<std::int64_t>(n), m_d.channels, out,
                                      m_d.nesting);
            }
        }

    private:
        Impl& m_d;
        const Clip& m_clip;
        std::int64_t m_frame;
        AudioFeed& m_feed;
        TrackRenderStats& m_stats;
        std::optional<Routing> m_routing;
        const AudioTakes::Take* m_enhanced = nullptr;
        double m_strength = 1.0;
    };

    /// A.10: the plan's pieces for input frames [from, to) of the run (within its sounding range [s0, s1)) added into
    /// `dst`, which holds the direct reads: those fade out by the pieces' coverage where a piece fades in.
    void renderPieces(const Track& track, const Run& run, const SpeedPlan& plan, Voice& voice, std::int64_t from,
                      std::int64_t to, std::int64_t s0, std::int64_t s1, float* dst, AudioFeed& feed, TrackRenderStats& stats) {
        const auto ch = static_cast<std::size_t>(channels);
        const std::int64_t base = absoluteBase();
        const Clip& clip = track.clips[run.first];
        const RunPositions positions(clip, origin, base, tpf);
        for (auto& s : voice.speed) {
            s.used = false;
        }
        struct Active {
            audio::dsp::SpeedRenderer* renderer;
            std::int64_t begin; // piece frame (renderer frames)
            std::int64_t r0;
            std::int64_t r1;
        };
        std::vector<Active> active;
        for (const auto& [p0, p1] : plan.pieces) {
            auto slot = std::ranges::find_if(voice.speed, [&](const Voice::Speed& s) { return s.begin == base + p0 && s.mode == plan.mode; });
            if (slot == voice.speed.end()) {
                voice.speed.push_back({base + p0, plan.mode, audio::dsp::makeSpeedRenderer(plan.mode, channels, sampleRate()), false});
                slot = voice.speed.end() - 1;
            }
            audio::dsp::SpeedRenderer& r = *slot->renderer;
            r.setPiece(base + p0, base + p1);
            slot->used = true;
            const std::int64_t r0 = std::max({from, s0, r.extentBegin() - base});
            const std::int64_t r1 = std::min({to, s1, r.extentEnd() - base});
            if (r1 > r0) {
                active.push_back({&r, p0, r0, r1});
            }
        }
        // The direct reads fade out where the pieces fade in (their coverage adds up to at most 1 there).
        for (const Active& a : active) {
            for (std::int64_t f = a.r0; f < a.r1; ++f) {
                const double g = a.renderer->coverage(base + f);
                if (g > 0.0) {
                    float* frame = dst + static_cast<std::size_t>(f - from) * ch;
                    const auto keep = static_cast<float>(std::max(0.0, 1.0 - g));
                    for (std::size_t c = 0; c < ch; ++c) {
                        frame[c] *= keep;
                    }
                }
            }
        }
        for (const Active& a : active) {
            PieceSource source(*this, track, clip, a.begin, feed, stats);
            const auto n = static_cast<std::size_t>(a.r1 - a.r0);
            pieceOut.resize(n * ch);
            a.renderer->render(base + a.r0, static_cast<int>(n), positions, source, pieceOut.data());
            float* sum = dst + static_cast<std::size_t>(a.r0 - from) * ch;
            for (std::size_t k = 0; k < n * ch; ++k) {
                sum[k] += pieceOut[k];
            }
        }
        if (voice.speed.size() > 8) { // pieces long behind: their renderers go
            std::erase_if(voice.speed, [](const Voice::Speed& s) { return !s.used; });
        }
    }

    /// Source audio of a run (Volume and Panner applied) for input frames [from, to), into `dst`; silence outside
    /// the run's sounding range. A.10: a run off 100 % (plan.active) reads its 100 % spans straight and renders the rest
    /// along its position (renderPieces).
    void runInput(const Track& track, const Run& run, const SpeedPlan& plan, Voice& voice, std::int64_t from,
                  std::int64_t to, float* dst, AudioFeed& feed, TrackRenderStats& stats) {
        const auto ch = static_cast<std::size_t>(channels);
        std::fill(dst, dst + static_cast<std::size_t>(to - from) * ch, 0.0f);
        const ClipList& clips = track.clips;
        const std::int64_t s0 = std::max(from, frameAtOrAfter(run.outStart));
        const std::int64_t s1 = std::min(to, frameAtOrAfter(run.outEnd));
        for (std::size_t i = run.first; i <= run.last && s0 < s1; ++i) {
            const Clip& clip = clips[i];
            // The clip's frames; the run's first and last clips also cover the transition handles.
            const std::int64_t c0 = i == run.first ? s0 : std::max(s0, frameAtOrAfter(clip.start()));
            const std::int64_t c1 = i == run.last ? s1 : std::min(s1, frameAtOrAfter(clip.end()));
            if (c1 <= c0) {
                continue;
            }
            // A.4: the media's enhanced take, when the clip enhances speech and the take was rendered.
            const EffectInstance* enhance = takes ? enhanceSpeechEffect(clip.effects()) : nullptr;
            const AudioTakes::Take* enhanced = enhance ? takes->find(clip.media, clip.stream.index, kEnhanceSpeechTake) : nullptr;
            const auto readSpan = [&](std::int64_t begin, std::int64_t end) {
                if (end <= begin) {
                    return;
                }
                const auto count = static_cast<std::size_t>(end - begin);
                float* out = dst + static_cast<std::size_t>(begin - from) * ch;
                const Ticks firstTime = timeOf(begin);
                const Ticks readFrom = clip.reversed ? sourceTimeExtended(clip, timeOf(end - 1)) : sourceTimeExtended(clip, firstTime);
                float* target = out;
                if (clip.reversed) {
                    reversed.resize(count * ch);
                    target = reversed.data();
                }
                // A.9: a clip whose channels are mapped, or that plays on a track or bus of another format, is read at
                // its source's channel count and routed; otherwise the stream decodes to the bus.
                const std::optional<Routing> routing = clip.isNested() ? std::nullopt : routingFor(track, clip);
                const auto frameOf = [&](std::size_t k) {
                    return clip.reversed ? end - 1 - static_cast<std::int64_t>(k) : begin + static_cast<std::int64_t>(k);
                };
                const auto read =
                    clip.isNested() ? std::expected<std::size_t, Error>(readNested(voice, clip, readFrom, target, count, feed, stats))
                    : routing       ? readRouted(*routing, clip,
                                                 [&](std::span<float> s, int n) {
                                                     return feed.read(clip.media, clip.stream.index, readFrom, s, n, rate);
                                                 },
                                                 target, count, frameOf)
                                    : feed.read(clip.media, clip.stream.index, readFrom, std::span<float>(target, count * ch),
                                                channels, rate);
                std::size_t got = 0;
                if (!read) {
                    ++stats.feedErrors;
                } else {
                    got = std::min(*read, count);
                    if (got < count) {
                        stats.shortReadFrames += count - got;
                    }
                }
                std::fill(target + got * ch, target + count * ch, 0.0f);
                if (clip.reversed) {
                    for (std::size_t k = 0; k < count; ++k) {
                        std::copy_n(reversed.data() + (count - 1 - k) * ch, ch, out + k * ch);
                    }
                }
                if (enhanced == nullptr) {
                    return;
                }
                // The take at the same source time (it renders the stream from source time 0), blended in by the
                // effect's Strength. Frames the take does not have (a failed or short read) keep the source.
                take.resize(count * ch);
                const auto tread =
                    routing ? readRouted(*routing, clip,
                                         [&](std::span<float> s, int n) { return feed.readTake(enhanced->path, readFrom, s, n, rate); },
                                         take.data(), count, frameOf)
                            : feed.readTake(enhanced->path, readFrom, std::span<float>(take.data(), count * ch), channels, rate);
                if (!tread) {
                    ++stats.takeErrors;
                    return;
                }
                const std::size_t have = std::min(*tread, count);
                std::size_t first = 0;
                if (clip.reversed) {
                    std::reverse(take.begin(), take.begin() + static_cast<std::ptrdiff_t>(count * ch));
                    for (std::size_t k = 0; k < count; ++k) { // frames were reversed with their channels: put channels back
                        std::reverse(take.begin() + static_cast<std::ptrdiff_t>(k * ch), take.begin() + static_cast<std::ptrdiff_t>((k + 1) * ch));
                    }
                    first = count - have;
                }
                const ParamValue* strength = enhance->find("strength");
                blendTake(strength, begin + static_cast<std::int64_t>(first), begin + static_cast<std::int64_t>(first + have),
                          out + first * ch, take.data() + first * ch, channels,
                          [&](std::int64_t f) { return clip.curveTimeAt(timeOf(f)); });
            };
            if (plan.active) {
                // A.10: the 100 % spans of a remap read straight (sourceIn is curve time, so decode from sourceTimeAt);
                // the rest of the run, a constant speed's too, is the pieces' (below).
                for (const auto& [d0, d1] : plan.direct) {
                    readSpan(std::max(c0, d0), std::min(c1, d1));
                }
            } else if (clip.isRemapped()) {
                // A nested or multicam clip's remap: its legacy speed is 1/1 even on ramps, holds and reverse, and only
                // its straight forward 1x ranges are heard (nested sound plays at 100 % only).
                for (const TimeRange& range : clip.unitySpeedRanges()) {
                    readSpan(std::max(c0, frameAtOrAfter(range.start)), std::min(c1, frameAtOrAfter(range.end())));
                }
            } else {
                readSpan(c0, c1);
            }
        }
        if (plan.active && s0 < s1) {
            renderPieces(track, run, plan, voice, from, to, s0, s1, dst, feed, stats);
        }
        applyVolumeAndPan(clips[run.first], s0, s1, dst + static_cast<std::size_t>(std::max<std::int64_t>(s0 - from, 0)) * ch);
        // Redactions replace the marked media time after the clip's level (a fixed -12 dBFS bleep), before its effects.
        for (std::size_t i = run.first; redactions && i <= run.last && s0 < s1; ++i) {
            const Clip& clip = clips[i];
            if (clip.isNested()) {
                continue; // the media inside is redacted where it plays (NestedMix's renderers)
            }
            const std::int64_t c0 = i == run.first ? s0 : std::max(s0, frameAtOrAfter(clip.start()));
            const std::int64_t c1 = i == run.last ? s1 : std::min(s1, frameAtOrAfter(clip.end()));
            if (plan.active) {
                // A.10: the straight 100 % spans here; the pieces applied the redactions to their source in media
                // time (PieceSource), so a bleep never sounds where the run is silent (holds, past the media).
                for (const auto& [d0, d1] : plan.direct) {
                    const auto begin = std::max(c0, d0);
                    const auto end = std::min(c1, d1);
                    if (end > begin) {
                        redactions->apply(clip.media, clip.id, clip.sourceTimeAt(timeOf(begin)), tpf, end - begin, channels,
                                          dst + static_cast<std::size_t>(begin - from) * ch, nesting);
                    }
                }
            } else if (clip.isRemapped()) {
                // Keep the audio owner's remap policy: bleeps may replace audible 1x spans, but must not
                // generate sound in ramps, holds, reverse segments or transition handles outside those spans.
                for (const TimeRange& range : clip.unitySpeedRanges()) {
                    const auto begin = std::max(c0, frameAtOrAfter(range.start));
                    const auto end = std::min(c1, frameAtOrAfter(range.end()));
                    if (end > begin) {
                        redactions->apply(clip.media, clip.id, clip.sourceTimeAt(timeOf(begin)), tpf, end - begin,
                                          channels, dst + static_cast<std::size_t>(begin - from) * ch, nesting);
                    }
                }
            } else if (c1 > c0) {
                redactions->apply(clip.media, clip.id, sourceTimeExtended(clip, timeOf(c0)), clip.reversed ? -tpf : tpf, c1 - c0,
                                  channels, dst + static_cast<std::size_t>(c0 - from) * ch, nesting);
            }
        }
    }

    /// Volume (dB) and Panner (balance) of `clip` (every clip of a run has the same) on the parameter grid: values at
    /// cell boundaries, linear ramps between, sample by sample.
    void applyVolumeAndPan(const Clip& clip, std::int64_t from, std::int64_t to, float* dst) const {
        const ClipProperties& props = clip.properties();
        detail::applyFader(props[ParamId::VolumeDb], &props[ParamId::Pan], from, to, dst, channels,
                           [&](std::int64_t f) { return clip.curveTimeAt(timeOf(f)); });
    }

    /// A.3: the track fader and panner (Track::volume, Track::pan; timeline time) on the mixed track, after its
    /// effect stack (and its 5.1 panner), for output frames [from, to) at `dst` (bus channels).
    void applyTrackFader(const Track& track, std::int64_t from, std::int64_t to, float* dst) const {
        // A.9: on a 5.1 bus the track's stereo panner gives way to its 5.1 panner (panToBus or the routing). The level
        // is a plain gain, so it applies after the 5.1 panner as the player's callback applies it.
        detail::applyFader(track.volume, busChannels == 2 ? &track.pan : nullptr, from, to, dst, busChannels,
                           [&](std::int64_t f) { return timeOf(f); });
    }

    /// Review A5-P2-1 / A5-P2-10: the track's 5.1 panner (puck x and y, the LFE send; timeline time) from the track's
    /// channels at `in` to the bus at `out`, for frames [f0, f0 + n). On the parameter grid counted from timeline 0,
    /// like the player's track fader: gains at cell boundaries, linear between, sample by sample, so a moving puck
    /// does not step and the result does not depend on how the stream is cut into blocks (preview == export).
    void panToBus(const Track& track, std::int64_t f0, std::int64_t n, const float* in, float* out) const {
        const SurroundPan& pan = track.surround;
        const auto ic = static_cast<std::size_t>(channels);
        const auto oc = static_cast<std::size_t>(busChannels);
        using Gains = std::array<float, 6 * 2>; // row-major, oc rows of ic gains
        const auto gainsAt = [&](std::int64_t frame, Gains& g) {
            const Ticks t{frame * tpf};
            surroundPanGains(channels, pan.x.valueAt(t), pan.y.valueAt(t), detail::faderGain(pan.lfeDb.valueAt(t)),
                             g.data());
        };
        const auto place = [&](const float* g, const float* src, float* dst) {
            for (std::size_t r = 0; r < oc; ++r) {
                float sum = 0.0f;
                for (std::size_t c = 0; c < ic; ++c) {
                    sum += g[r * ic + c] * src[c];
                }
                dst[r] = sum;
            }
        };
        const auto floorDiv = [](std::int64_t v, std::int64_t d) { return v >= 0 ? v / d : -((-v + d - 1) / d); };
        const std::int64_t first = floorDiv(timeOf(f0).value, tpf); // timeline frame of f0
        Gains a{};
        if (pan.x.isConstant() && pan.y.isConstant() && pan.lfeDb.isConstant()) {
            gainsAt(first, a);
            for (std::int64_t k = 0; k < n; ++k) {
                place(a.data(), in + static_cast<std::size_t>(k) * ic, out + static_cast<std::size_t>(k) * oc);
            }
            return;
        }
        constexpr std::int64_t cell = audio::kParamCellFrames;
        Gains b{};
        Gains g{};
        std::int64_t cached = std::numeric_limits<std::int64_t>::min();
        for (std::int64_t k = 0; k < n; ++k) {
            const std::int64_t frame = first + k;
            const std::int64_t c0 = floorDiv(frame, cell) * cell;
            if (c0 != cached) {
                cached = c0;
                gainsAt(c0, a);
                gainsAt(c0 + cell, b);
            }
            const double u = static_cast<double>(frame - c0) / static_cast<double>(cell);
            for (std::size_t i = 0; i < oc * ic; ++i) {
                g[i] = static_cast<float>(a[i] + (b[i] - a[i]) * u);
            }
            place(g.data(), in + static_cast<std::size_t>(k) * ic, out + static_cast<std::size_t>(k) * oc);
        }
    }

    /// The edge envelope of a run for output frames [a, b) at `dst`: transition crossfades, else cut fades.
    void applyEnvelope(const Track& track, const Run& run, const SpeedPlan& plan, std::int64_t a, std::int64_t b,
                       float* dst) const {
        const auto ch = static_cast<std::size_t>(channels);
        const ClipList& clips = track.clips;
        const std::int64_t fade = edgeFadeFrames();
        const std::int64_t startFrame = frameAtOrAfter(run.outStart);
        const std::int64_t endFrame = frameAtOrAfter(run.outEnd);
        auto regionOf = [&](const Transition& t, Ticks cut) { return TimeRange{cut - t.before, t.before + t.after}; };
        const std::optional<TimeRange> headRegion =
            run.head ? std::optional<TimeRange>(regionOf(*run.head, clips[run.first].start())) : std::nullopt;
        const std::optional<TimeRange> tailRegion =
            run.tail ? std::optional<TimeRange>(regionOf(*run.tail, clips[run.last].end())) : std::nullopt;
        const bool fadeIn = !run.head && !run.headSwitch && fade > 0;
        const bool fadeOut = !run.tail && !run.tailSwitch && fade > 0;
        const std::int64_t crossfade = switchFrames();
        const Clip& first = clips[run.first];
        // A.10: a run off 100 % sounds where its plan says (holds are silent); a nested remap only in its 100 % spans.
        const bool limited = plan.active ? plan.remapped : first.isRemapped();
        if (!limited && !headRegion && !tailRegion && !fadeIn && !fadeOut && !run.headSwitch && !run.tailSwitch) {
            return;
        }
        // Review P2-21: a remapped clip is heard only in parts, which can start and end inside the run (a hold is
        // silent; before A.10 a ramp or a reverse was too). Those edges are cuts too and get the same fades, except where
        // a span meets another span or the run's own edge (faded above, or by its transition).
        struct Span {
            std::int64_t begin = 0;
            std::int64_t end = 0;
            bool fadeIn = false;
            bool fadeOut = false;
        };
        std::vector<Span> spans;
        if (plan.active && plan.remapped) {
            for (const auto& [x, y] : plan.audible) { // merged: no two of them meet
                Span s{x, y};
                s.fadeIn = fade > 0 && s.begin > startFrame;
                s.fadeOut = fade > 0 && s.end < endFrame;
                spans.push_back(s);
            }
        } else if (limited) {
            const auto audible = first.unitySpeedRanges();
            for (const TimeRange& range : audible) {
                const bool joinedBefore = std::ranges::any_of(audible, [&](const TimeRange& o) { return o.end() == range.start; });
                const bool joinedAfter = std::ranges::any_of(audible, [&](const TimeRange& o) { return o.start == range.end(); });
                Span s{frameAtOrAfter(range.start), frameAtOrAfter(range.end())};
                s.fadeIn = fade > 0 && s.begin > startFrame && !joinedBefore;
                s.fadeOut = fade > 0 && s.end < endFrame && !joinedAfter;
                spans.push_back(s);
            }
        }
        const auto denominator = static_cast<double>(fade + 1);
        for (std::int64_t k = a; k < b; ++k) {
            double g = 1.0;
            const Ticks t = timeOf(k);
            if (limited) {
                const auto span = std::ranges::find_if(spans, [&](const Span& s) { return k >= s.begin && k < s.end; });
                if (span == spans.end()) {
                    // Also silence clip-effect tails after latency compensation, including transition handles.
                    std::fill_n(dst + static_cast<std::size_t>(k - a) * ch, ch, 0.0f);
                    continue;
                }
                if (span->fadeIn && k - span->begin < fade) {
                    g *= static_cast<double>(k - span->begin + 1) / denominator;
                }
                if (span->fadeOut && span->end - 1 - k < fade) {
                    g *= static_cast<double>(span->end - k) / denominator;
                }
            }
            if (headRegion && headRegion->duration.value > 0 && t < headRegion->end()) {
                g *= transitionGain(*run.head, static_cast<double>((t - headRegion->start).value) /
                                                   static_cast<double>(headRegion->duration.value),
                                    true);
            }
            if (tailRegion && tailRegion->duration.value > 0 && t >= tailRegion->start) {
                g *= transitionGain(*run.tail, static_cast<double>((t - tailRegion->start).value) /
                                                   static_cast<double>(tailRegion->duration.value),
                                    false);
            }
            if (fadeIn && k - startFrame < fade) {
                g *= static_cast<double>(k - startFrame + 1) / denominator;
            }
            if (fadeOut && endFrame - 1 - k < fade) {
                g *= static_cast<double>(endFrame - k) / denominator;
            }
            // C.8: an angle switch. The incoming and outgoing ramps over the same frames add up to exactly 1.
            if (run.headSwitch && k - startFrame < crossfade) {
                g *= static_cast<double>(k - startFrame + 1) / static_cast<double>(crossfade + 1);
            }
            if (run.tailSwitch && endFrame - 1 - k < crossfade) {
                g *= static_cast<double>(endFrame - k) / static_cast<double>(crossfade + 1);
            }
            if (g != 1.0) {
                float* frame = dst + static_cast<std::size_t>(k - a) * ch;
                for (std::size_t c = 0; c < ch; ++c) {
                    frame[c] = static_cast<float>(frame[c] * g);
                }
            }
        }
    }

    /// Clip mix (the track chain's input) for frames [g0, g0 + n), added into `dst` (zeroed by the caller).
    void mixClips(const Track& track, std::int64_t g0, std::int64_t n, float* dst, AudioFeed& feed, TrackRenderStats& stats) {
        for (auto& v : voices) {
            v->seen = false;
        }
        const ClipList& clips = track.clips;
        const Ticks t0 = timeOf(g0);
        const Ticks t1 = timeOf(g0 + n);
        const auto ch = static_cast<std::size_t>(channels);
        std::size_t i = firstClipEndingAfter(clips, t0);
        // A clip ending before t0 may still sound in its tail transition handle or an angle switch's crossfade.
        if (i > 0 && soundsPastEnd(track, i - 1)) {
            --i;
        }
        while (i < clips.size()) {
            const Clip& clip = clips[i];
            const Transition* head = headTransition(track, clip.id);
            const Ticks earliest = head && head->outgoing.isValid() ? clip.start() - head->before
                                   : i > 0 && crossfadesAfter(track, i - 1) ? clip.start() - Ticks{switchLead() * tpf}
                                                                             : clip.start();
            if (earliest >= t1) {
                break;
            }
            const Run run = runAt(track, i);
            i = run.last + 1;
            if (!playable(clip)) {
                continue;
            }
            const std::int64_t a = std::max(g0, frameAtOrAfter(run.outStart));
            const std::int64_t b = std::min(g0 + n, frameAtOrAfter(run.outEnd));
            if (b <= a) {
                continue;
            }
            const Clip& first = clips[run.first];
            Voice& voice = voiceFor(first.id);
            if (voice.chain.bind(first.effects()) && voice.primed) {
                voice.chain.reset(); // the stack's structure changed under a running voice: start it again here
                voice.primed = false;
            }
            if (!voice.primed) {
                // The chain first takes `latency` frames that only fill its delay line. p02 audio: a voice that starts
                // inside its clip (a seek, an edit's refill, a restart) first hears the clip from up to its effects'
                // settle time earlier (never before the clip): a reverb or delay starts with the tail it would have had
                // playing through, so the restart does not click, and preview and export agree from any start.
                const std::int64_t runStart = frameAtOrAfter(run.outStart);
                const std::int64_t settle = std::min<std::int64_t>(
                    voice.chain.settleFrames(), mulDivSaturate(options.maxClipPreroll.value, 1, tpf, Rounding::Ceil));
                voice.nextInput = a > runStart ? std::max(runStart, a - std::max<std::int64_t>(settle, 0)) : a;
                voice.primed = true;
            }
            voice.seen = true;
            const std::int64_t latency = voice.chain.latencyFrames();
            const std::int64_t inFrom = std::min(voice.nextInput, a + latency);
            const std::int64_t inTo = b + latency;
            const auto count = static_cast<std::size_t>(inTo - inFrom);
            voiceIn.resize(count * ch);
            const SpeedPlan plan = planOf(track, run);
            runInput(track, run, plan, voice, inFrom, inTo, voiceIn.data(), feed, stats);
            voice.chain.process(voiceIn.data(), static_cast<int>(count), inFrom,
                                audio::CurveClock{origin, tpf, &first});
            voice.nextInput = inTo;
            // The chain's output for input frames [inFrom, inTo) is output frames [inFrom - latency, b).
            float* out = voiceIn.data() + (count - static_cast<std::size_t>(b - a)) * ch;
            applyEnvelope(track, run, plan, a, b, out);
            float* to = dst + static_cast<std::size_t>(a - g0) * ch;
            for (std::size_t k = 0; k < static_cast<std::size_t>(b - a) * ch; ++k) {
                to[k] += out[k];
            }
        }
        std::erase_if(voices, [](const std::unique_ptr<Voice>& v) { return !v->seen; });
    }

    /// Output frames [f0, f0 + n) at the bus's channels into `out`: the track at the renderer's channels, its 5.1
    /// panner when that places it last, then the track fader.
    void renderOut(const Track& track, std::int64_t f0, std::int64_t n, float* out, AudioFeed& feed,
                   TrackRenderStats& stats) {
        if (channels == busChannels) {
            renderBlock(track, f0, n, out, feed, stats);
        } else {
            trackOut.resize(static_cast<std::size_t>(n) * static_cast<std::size_t>(channels));
            renderBlock(track, f0, n, trackOut.data(), feed, stats);
            panToBus(track, f0, n, trackOut.data(), out);
        }
        if (options.trackFader) {
            applyTrackFader(track, f0, f0 + n, out);
        }
    }

    /// The track's pre-fader output for frames [f0, f0 + n) into `out` (interleaved at `channels`, zeroed here).
    void renderBlock(const Track& track, std::int64_t f0, std::int64_t n, float* out, AudioFeed& feed,
                     TrackRenderStats& stats) {
        const auto ch = static_cast<std::size_t>(channels);
        std::fill(out, out + static_cast<std::size_t>(n) * ch, 0.0f);
        if (trackChain.bind(track.effects) && trackInput != 0) {
            trackChain.reset();
            trackInput = f0;
        }
        if (trackChain.empty()) {
            mixClips(track, f0, n, out, feed, stats);
            trackInput = f0 + n;
            return;
        }
        const std::int64_t latency = trackChain.latencyFrames();
        const std::int64_t inFrom = std::min(trackInput, f0 + latency);
        const std::int64_t inTo = f0 + n + latency;
        const auto count = static_cast<std::size_t>(inTo - inFrom);
        mix.assign(count * ch, 0.0f);
        mixClips(track, inFrom, static_cast<std::int64_t>(count), mix.data(), feed, stats);
        trackChain.process(mix.data(), static_cast<int>(count), inFrom, audio::CurveClock{origin, tpf, nullptr});
        trackInput = inTo;
        std::copy_n(mix.data() + (count - static_cast<std::size_t>(n)) * ch, static_cast<std::size_t>(n) * ch, out);
    }

    bool needsPreroll(const Track& track, Ticks start) const {
        if (options.maxPreroll.value <= 0) {
            return false;
        }
        if (hasRunnableEffects(track.effects)) {
            return true;
        }
        const ClipList& clips = track.clips;
        for (std::size_t i = firstClipEndingAfter(clips, start - options.maxPreroll);
             i < clips.size() && clips[i].start() <= start; ++i) {
            if (playable(clips[i]) && hasRunnableEffects(clips[i].effects())) {
                return true;
            }
        }
        return false;
    }

    void restart(const Track& t, Ticks start, AudioFeed& feed, TrackRenderStats& stats) {
        voices.clear();
        if (const int want = channelsFor(t); want != channels) {
            channels = want; // the track's format moved it on or off its 5.1 panner: its chain runs at other channels
            trackChain = audio::EffectChain(channels, sampleRate());
        } else {
            trackChain.reset();
        }
        trackInput = 0;
        started = true;
        trackId = t.id;
        const std::int64_t preroll =
            needsPreroll(t, start) ? mulDivSaturate(options.maxPreroll.value, 1, tpf, Rounding::Ceil) : 0;
        origin = start - Ticks{preroll * tpf};
        cursor = 0;
        constexpr std::int64_t kPiece = 4096;
        while (cursor < preroll) {
            const std::int64_t n = std::min(kPiece, preroll - cursor);
            discard.resize(static_cast<std::size_t>(n) * static_cast<std::size_t>(channels));
            renderBlock(t, cursor, n, discard.data(), feed, stats);
            cursor += n;
        }
    }
};

TrackRenderer::TrackRenderer(int channels, Rate rate, TrackRendererOptions options)
    : m_impl(std::make_unique<Impl>(std::max(channels, 1), rate, options)), m_channels(std::max(channels, 1)), m_rate(rate) {}

TrackRenderer::~TrackRenderer() = default;

void TrackRenderer::render(const Track& track, Ticks start, std::span<float> out, AudioFeed& feed,
                           TrackRenderStats& stats) {
    std::fill(out.begin(), out.end(), 0.0f);
    if (!m_rate.isExact()) {
        return;
    }
    const auto ch = static_cast<std::size_t>(m_channels);
    const auto frames = static_cast<std::int64_t>(out.size() / ch);
    if (frames == 0) {
        return;
    }
    Impl& d = *m_impl;
    if (!d.started || track.id != d.trackId || start != d.next || d.channelsFor(track) != d.channels) {
        d.restart(track, start, feed, stats);
    }
    d.renderOut(track, d.cursor, frames, out.data(), feed, stats);
    d.cursor += frames;
    d.next = start + Ticks{frames * d.tpf};
}

void TrackRenderer::reset() noexcept { m_impl->started = false; }

void TrackRenderer::setRedactions(std::shared_ptr<const AudioRedactions> redactions) noexcept {
    m_impl->redactions = std::move(redactions);
}

void TrackRenderer::setAudioTakes(std::shared_ptr<const AudioTakes> takes) noexcept {
    m_impl->takes = std::move(takes);
}

void TrackRenderer::setProject(std::shared_ptr<const ProjectState> project) noexcept { m_impl->project = std::move(project); }

void TrackRenderer::setNesting(std::vector<ClipId> path) { m_impl->nesting = std::move(path); }

int TrackRenderer::activeVoices() const noexcept { return static_cast<int>(m_impl->voices.size()); }

int TrackRenderer::trackLatencyFrames() const noexcept { return m_impl->trackChain.latencyFrames(); }

} // namespace wabcut::playback
