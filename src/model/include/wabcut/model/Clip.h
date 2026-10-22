#pragma once

// A clip: one stream of one media item placed on a track - or, since C.8, of a sequence (a nested sequence or a
// multicam source, Nesting.h): Clip::sequence is then set and Clip::media is not.

#include <wabcut/model/AudioChannels.h>
#include <wabcut/model/Blend.h>
#include <wabcut/model/ColorManagement.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/FieldOptions.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/Parameters.h>
#include <wabcut/model/Time.h>
#include <wabcut/model/TimeInterpolation.h>
#include <wabcut/model/TimeRemap.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wabcut {

struct Graphic; // Graphics.h

enum class StreamKind : std::uint8_t { Video, Audio };

/// Which stream of a media item a clip plays: the `index`-th video or audio stream, i.e. an index into
/// MediaInfo::video / MediaInfo::audio (not the container stream index, which lives in that entry).
struct StreamRef {
    StreamKind kind = StreamKind::Video;
    std::int32_t index = 0;

    constexpr bool operator==(const StreamRef&) const = default;
};

/// Most angles a multicam source has (AngleSet holds them as bits).
inline constexpr std::size_t kMaxMulticamAngles = 64;

/// The angles a multicam clip plays (Nesting.h): bit i = angle i of its multicam source. A video clip shows exactly
/// one angle; an audio clip plays one or more (mixed). Empty on every other clip.
struct AngleSet {
    std::uint64_t bits = 0;

    static constexpr AngleSet of(std::size_t angle) {
        return angle < kMaxMulticamAngles ? AngleSet{std::uint64_t{1} << angle} : AngleSet{};
    }
    constexpr bool empty() const { return bits == 0; }
    constexpr bool contains(std::size_t angle) const { return angle < kMaxMulticamAngles && ((bits >> angle) & 1u) != 0; }
    constexpr std::size_t count() const {
        std::size_t n = 0;
        for (std::uint64_t b = bits; b != 0; b &= b - 1) {
            ++n;
        }
        return n;
    }
    /// The lowest angle in the set (the only one of a video clip); kMaxMulticamAngles when empty.
    constexpr std::size_t first() const {
        for (std::size_t i = 0; i < kMaxMulticamAngles; ++i) {
            if (contains(i)) {
                return i;
            }
        }
        return kMaxMulticamAngles;
    }
    constexpr bool operator==(const AngleSet&) const = default;
};

/// Exact playback speed: source ticks advanced per timeline tick, as a ratio (2/1 = twice as fast).
/// 0/1 is a freeze frame (a frame hold): the clip shows its source In for its whole length. Direction is
/// separate (Clip::reversed), so a speed is never negative. The edit engine maps time through these helpers.
struct Speed {
    std::int32_t num = 1;
    std::int32_t den = 1;

    constexpr bool operator==(const Speed&) const = default;
    constexpr bool isValid() const { return num >= 0 && den > 0; }
    constexpr bool isUnity() const { return num == den && num > 0; }
    constexpr bool isFreeze() const { return num == 0; }
};

/// floor(t * num / den), exact (128-bit intermediate). Saturates if the result does not fit int64, which
/// cannot happen for timeline-range values and positive speeds of at most 2^31.
constexpr Ticks scaleTicks(Ticks t, std::int64_t num, std::int64_t den) {
    if (const auto v = mulDiv(t.value, num, den, Rounding::Floor)) {
        return {*v};
    }
    return (t.value < 0) != (num < 0) ? Ticks::min() : Ticks::max();
}

/// Timeline duration -> source duration for a clip played at `speed`.
constexpr Ticks toSourceTicks(Ticks timeline, Speed speed) { return scaleTicks(timeline, speed.num, speed.den); }
/// Source duration -> the longest timeline duration that does not read past it.
constexpr Ticks toTimelineTicks(Ticks source, Speed speed) { return scaleTicks(source, speed.den, speed.num); }

/// The part of a clip that edits move around but rarely change: its name, parameters (with their keyframe
/// curves) and effect stack. It is immutable and shared: a razor cut gives both pieces the same block, and
/// copying a clip (which ripple edits do for every later clip) never copies strings, curves or effects.
struct ClipContent {
    std::string name;
    ClipProperties properties;
    /// Applied in order after the clip's intrinsic effects (Motion/Opacity or Volume/Panner).
    EffectStack effects;
    /// Time remapping (TimeRemap.h): curve time -> source position. Set: the clip is remapped (speed 100 %, not
    /// reversed, its sourceIn a curve time).
    std::optional<Curve> timeRemap;
    /// A source of a kind this build does not know (a newer project): the file's "source" object, kept as JSON text
    /// and saved back unchanged (the policy of unknown effects). Such a clip has neither media nor a sequence;
    /// evaluators show and play nothing for it, and edits move, trim and delete it like any clip.
    std::string unknownSource;
    /// A.9, audio clips: Premiere's Modify > Audio Channels for this clip (its channel format and which source channels
    /// feed each of its channels). Unset: its media item's map, else the stream as it always played. Saved only when set.
    std::optional<AudioChannelMap> channels;
    /// A graphic clip's layers (Graphics.h, C.13): set on clips of a "wabcut.graphic" generator item. Immutable and
    /// shared like the rest of the block; compared by value.
    std::shared_ptr<const Graphic> graphic;
    /// C.15, video clips: the Opacity section's Blend Mode, how the clip combines with what is below it (Blend.h; the
    /// math in docs/internal/c15-parity-handoff.md). Not keyframable (opacity is). Saved only when not Normal.
    BlendMode blendMode = BlendMode::Normal;
    /// P.2 (E5), video clips: Premiere's Clip > Video Options > Scale to Frame Size. The picture is fitted into the
    /// sequence frame before Motion (FrameScaling.h, scaleToFrameFactor), so Motion > Scale stays 100 % for a fitted
    /// picture. Saved only when set.
    bool scaleToFrame = false;
    /// P.2 (E5), frame holds: Premiere's Frame Hold Options > Hold Filters. On a freeze frame the clip's keyframed
    /// parameters and effects hold at their value at the held frame too (curveTimeAt stays at sourceIn); off (the
    /// default), they keep animating through the hold. Saved only when set.
    bool holdFilters = false;
    /// P.2, video clips: Time Interpolation (TimeInterpolation.h), how a retimed clip makes the frames between its
    /// media's frames. Saved only when not Frame Sampling.
    TimeInterpolation timeInterpolation = TimeInterpolation::FrameSampling;
    /// I.9, video clips: how the clip's decoded pictures are read into the working space (ColorManagement.h). Unset:
    /// from the stream's colour tags (Auto). Ignored in display-referred Rec.709 projects. Saved only when set.
    std::optional<InputColorSpace> inputColorSpace;
    /// 0.2, video clips: Premiere's Clip > Video Options > Field Options (FieldOptions.h). Saved only when not the
    /// default.
    ClipFieldOptions fieldOptions;

    bool operator==(const ClipContent& other) const;
};

struct Clip {
    ClipId id;
    MediaId media;
    StreamRef stream;
    /// Where the clip sits on its track, half-open [start, end).
    TimeRange timelineRange;
    /// Source time of the clip's first frame.
    Ticks sourceIn;
    Speed speed;
    bool enabled = true;
    /// Invalid when the clip is not linked.
    LinkGroupId linkGroup;
    /// 0.2, Premiere's Clip > Group (ClipGroups.h): invalid when the clip is not grouped. Saved only when set.
    ClipGroupId group;
    std::int32_t colorIndex = 0;
    /// Plays its source range backwards: the clip starts on the last frame of [sourceIn, sourceOut).
    bool reversed = false;
    /// Audio: keep the pitch when the speed is not 1 (time-stretch instead of resample). Stored for the
    /// audio engine; meaningless for video.
    bool maintainPitch = false;
    /// C.8: the clip plays this sequence instead of media (a nested sequence, or a multicam source when the sequence
    /// has Sequence::multicam). `media` is invalid then; `stream` is Video 0 (the nested picture) or Audio 0 (the
    /// nested mix, Nesting.h); sourceIn and the speed are in the nested sequence's time.
    SequenceId sequence;
    /// A multicam clip's active angle(s) (AngleSet); empty on other clips.
    AngleSet angles;

    constexpr Ticks start() const { return timelineRange.start; }
    constexpr Ticks end() const { return timelineRange.end(); }
    constexpr Ticks duration() const { return timelineRange.duration; }
    constexpr bool isFreeze() const { return speed.isFreeze(); }
    /// Plays a sequence (nested or multicam) rather than media.
    constexpr bool isNested() const { return sequence.isValid(); }
    /// Plays a source of a kind this build does not know (ClipContent::unknownSource).
    bool hasUnknownSource() const { return !content().unknownSource.empty(); }
    /// Time remapping is on (TimeRemap.h): the content holds a remap curve.
    bool isRemapped() const { return content().timeRemap.has_value(); }
    /// The remap curve, or null.
    const Curve* timeRemap() const { return content().timeRemap ? &*content().timeRemap : nullptr; }
    /// A.9: the clip's own channel map, or null (MediaItem::channels then applies, see effectiveChannelMap).
    const AudioChannelMap* channelMap() const { return content().channels ? &*content().channels : nullptr; }
    /// A graphic clip's layers (Graphics.h), or null.
    const Graphic* graphic() const { return content().graphic.get(); }
    /// I.9: the clip's input colour space override, or nullopt (Auto: from the stream's tags).
    std::optional<InputColorSpace> inputColorSpace() const { return content().inputColorSpace; }
    /// Amount of source media the clip plays: [sourceIn, sourceOut). 0 for a freeze frame. A remapped clip: the
    /// curve time it spans (sourceIn is a curve time there).
    constexpr Ticks sourceDuration() const { return toSourceTicks(timelineRange.duration, speed); }
    /// Source time just past the clip's last source frame (the top of its source range, also when reversed). A
    /// remapped clip: the curve time at its end.
    constexpr Ticks sourceOut() const { return sourceIn + sourceDuration(); }
    /// Source time shown at timeline position `t` (t inside timelineRange); always inside the source range
    /// (a freeze frame: sourceIn). Evaluators use this, not timeMap(), for reversed clips. A remapped clip: the
    /// remap curve at curveTimeAt(t), for any `t` (transition handles read the curve beyond the clip's ends).
    Ticks sourceTimeAt(Ticks t) const {
        if (const Curve* remap = timeRemap()) {
            return remapSourceAt(*remap, sourceIn + (t - start()));
        }
        if (isFreeze()) {
            return sourceIn;
        }
        const Ticks offset = toSourceTicks(t - start(), speed);
        if (!reversed) {
            return sourceIn + offset;
        }
        const Ticks top = sourceOut() - Ticks{1};
        return top - offset < sourceIn ? sourceIn : top - offset;
    }
    /// The clip's timeline -> source mapping (negative speed when reversed, 0 for a freeze frame). Not for a remapped
    /// clip (use sourceTimeAt).
    constexpr TimeMap timeMap() const {
        if (reversed && !isFreeze()) {
            return {start(), sourceOut() - Ticks{1}, -std::int64_t{speed.num}, speed.den};
        }
        return {start(), sourceIn, speed.num, speed.den};
    }
    /// Keyframe curve time at timeline position `t` (Parameters.h: curves are in media time). The source time
    /// shown, except on a freeze frame, where effects keep animating at normal rate from the held frame
    /// (Premiere's Frame Hold with "Hold Filters" off), and on a remapped clip, where curve time runs with the
    /// timeline from sourceIn the same way (effects animate at normal rate through holds, ramps and reverse). P.2: a freeze
    /// frame with Hold Filters (ClipContent::holdFilters) holds its parameters at sourceIn.
    Ticks curveTimeAt(Ticks t) const {
        if (isFreeze() && content().holdFilters) {
            return sourceIn;
        }
        return isFreeze() || isRemapped() ? sourceIn + (t - start()) : sourceTimeAt(t);
    }
    /// Timeline ranges where the clip plays its media forwards at exactly 100 %, in order: where its audio is heard.
    /// A remapped clip: its straight 100 % forward segments; otherwise the whole clip at speed 100 %, not reversed;
    /// none for other speeds, reverse and freeze frames.
    std::vector<TimeRange> unitySpeedRanges() const;
    /// Timeline time of a curve time, if the clip shows it (inverse of curveTimeAt, the first such time).
    std::optional<Ticks> timelineTimeOfCurveTime(Ticks curveTime) const;
    /// The media range shown over timeline [from, to) (from < to): from the lowest source time shown to just past the
    /// highest, at any speed and direction, on a freeze frame (its frame's first tick) and through a remap (its holds,
    /// ramps and reverse segments; not [sourceIn, sourceOut), which are curve times there).
    TimeRange shownSourceRange(Ticks from, Ticks to) const;
    /// The first timeline time inside the clip that shows media time `source`, or nullopt (never on a freeze frame).
    /// A remapped clip: where its curve first reaches it (a hold only at exactly its frame); a fast segment that skips
    /// past it gives the first time showing a later (forwards) or earlier (reverse) source.
    std::optional<Ticks> timelineTimeOfSourceTime(Ticks source) const;
    /// An intrinsic parameter's value at timeline position `t`.
    double parameterAt(ParamId param, Ticks t) const { return properties().value(param, curveTimeAt(t)); }

    /// Timeline ticks the head can be extended by before running out of source media (`extent` = available
    /// media, nullopt = unbounded; the result nullopt = unbounded).
    std::optional<Ticks> headRoom(std::optional<Ticks> extent) const;
    /// Timeline ticks the tail can be extended by.
    std::optional<Ticks> tailRoom(std::optional<Ticks> extent) const;
    /// The same within the media [start, end) (MediaItem::sourceBounds: a restricted subclip's range); `end` nullopt =
    /// unbounded. headRoom(extent) is headRoom(Ticks{}, extent).
    std::optional<Ticks> headRoom(Ticks start, std::optional<Ticks> end) const;
    std::optional<Ticks> tailRoom(Ticks start, std::optional<Ticks> end) const;
    /// Moves the clip's In point by `d` timeline ticks (positive = later), keeping the source under the rest
    /// of the clip where it is: updates start, duration and sourceIn (direction-aware).
    void moveHead(Ticks d);
    /// Moves the clip's Out point by `d` timeline ticks (positive = later), direction-aware.
    void moveTail(Ticks d);
    /// Shortens this clip to [start, at) and returns [at, end) as a copy (same id: the caller sets one); both
    /// keep showing the same source frames as before (direction-aware).
    Clip splitOff(Ticks at);

    const ClipContent& content() const { return m_content ? *m_content : defaultContent(); }
    const std::string& name() const { return content().name; }
    const ClipProperties& properties() const { return content().properties; }

    /// Copy-on-write setters: they give this clip a new content block and leave other clips that
    /// shared the old one untouched.
    void setContent(ClipContent content) {
        m_content = std::make_shared<const ClipContent>(std::move(content));
    }
    /// Shares an existing content block (null = default content).
    void setContent(std::shared_ptr<const ClipContent> shared) { m_content = std::move(shared); }
    void setName(std::string name) {
        ClipContent next = content();
        next.name = std::move(name);
        setContent(std::move(next));
    }
    void setProperties(const ClipProperties& properties) {
        ClipContent next = content();
        next.properties = properties;
        setContent(std::move(next));
    }
    void setGraphic(std::shared_ptr<const Graphic> graphic) {
        ClipContent next = content();
        next.graphic = std::move(graphic);
        setContent(std::move(next));
    }
    /// C.15: the clip's blend mode (ClipContent::blendMode).
    BlendMode blendMode() const { return content().blendMode; }
    void setBlendMode(BlendMode mode) {
        ClipContent next = content();
        next.blendMode = mode;
        setContent(std::move(next));
    }
    /// P.2: the clip's Time Interpolation (ClipContent::timeInterpolation).
    TimeInterpolation timeInterpolation() const { return content().timeInterpolation; }
    const EffectStack& effects() const { return content().effects; }
    void setEffects(EffectStack effects) {
        ClipContent next = content();
        next.effects = std::move(effects);
        setContent(std::move(next));
    }
    /// True when both clips share one content block (no deep compare).
    bool sharesContentWith(const Clip& other) const { return m_content == other.m_content; }

    /// Value equality, including the content.
    bool operator==(const Clip& o) const {
        return id == o.id && media == o.media && stream == o.stream && timelineRange == o.timelineRange &&
               sourceIn == o.sourceIn && speed == o.speed && enabled == o.enabled && linkGroup == o.linkGroup &&
               group == o.group && colorIndex == o.colorIndex && reversed == o.reversed && maintainPitch == o.maintainPitch &&
               sequence == o.sequence && angles == o.angles && (m_content == o.m_content || content() == o.content());
    }

private:
    static const ClipContent& defaultContent() {
        static const ClipContent kDefault;
        return kDefault;
    }

    std::shared_ptr<const ClipContent> m_content; // null = defaultContent()
};

} // namespace wabcut
