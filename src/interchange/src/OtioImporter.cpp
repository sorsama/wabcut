// OpenTimelineIO JSON (.otio), read directly from the published schema (no Python):
//   Timeline.1 { name, global_start_time: RationalTime|null, tracks: Stack.1 }
//   Stack.1 / Track.1 { name, kind ("Video"|"Audio"), children[], source_range, markers[], effects[], enabled
//   } Clip.1 { media_reference } / Clip.2 { media_references{key: ref}, active_media_reference_key }
//          + source_range: TimeRange|null, markers[], effects[], enabled
//   Gap.1 { source_range }   Transition.1 { transition_type, in_offset, out_offset } (takes no track time)
//   ExternalReference.1 { target_url, available_range }  MissingReference.1  GeneratorReference.1
//   RationalTime.1 { value, rate }  TimeRange.1 { start_time, duration }  Marker.1/2 { marked_range|range,
//   color } LinearTimeWarp.1 { time_scalar }  FreezeFrame.1
// SerializableCollection.1 { children[] } holds several timelines.
//
// OTIO's explicit gaps become Wabcut's implicit gaps; a track's trailing gap becomes the sequence duration.
// OTIO has no A/V links: a video clip and audio clips of the same media, range and source In are linked.
//
// Files Wabcut wrote carry metadata.wabcut, version 1 (OtioExporter.cpp documents it), on the timeline, tracks,
// clips, nested stacks, transitions, markers and media references. Where it is present and valid it replaces what
// OTIO rounds or has no field for: exact ticks, speeds and time remaps, clip parameters, channel maps, effects,
// titles, link groups (instead of inferred links), track flags, sequence settings, marker and transition details and
// media facts, so Wabcut -> OTIO -> Wabcut keeps everything the IR holds. Every value is checked; a bad one is
// skipped and reported. Exact times are used only while they agree with the OTIO times to within half a frame: an
// object moved or trimmed in another editor keeps its OTIO times. Names, comments and clip enabled flags are read from
// OTIO, which holds them exactly (renames in another editor survive). A nested sequence is defined by the copy of its
// stack that has a "sequence" section; its other copies refer to it by key. Files without it import as before.

#include "ImporterCommon.h"
#include "Importers.h"
#include "TimeMath.h"

#include <wabcut/model/Nesting.h>
#include <wabcut/model/TimeRemap.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <set>

namespace wabcut::interchange::detail {

namespace {

using Json = nlohmann::json;

/// "Clip.2" -> "Clip"; "" when the object has no schema.
std::string_view schemaName(const Json& j) {
    if (!j.is_object()) {
        return {};
    }
    const auto it = j.find("OTIO_SCHEMA");
    if (it == j.end() || !it->is_string()) {
        return {};
    }
    const std::string& s = it->get_ref<const std::string&>();
    const std::string_view view(s);
    return view.substr(0, view.find('.'));
}

const Json* member(const Json& j, const char* key) {
    if (!j.is_object()) {
        return nullptr;
    }
    const auto it = j.find(key);
    return it == j.end() || it->is_null() ? nullptr : &*it;
}

std::string stringMember(const Json& j, const char* key) {
    const Json* m = member(j, key);
    return m && m->is_string() ? m->get<std::string>() : std::string();
}

std::optional<double> numberMember(const Json& j, const char* key) {
    const Json* m = member(j, key);
    if (!m || !m->is_number()) {
        return std::nullopt;
    }
    const double v = m->get<double>();
    return std::isfinite(v) ? std::optional<double>(v) : std::nullopt;
}

std::optional<bool> boolMember(const Json& j, const char* key) {
    const Json* m = member(j, key);
    return m && m->is_boolean() ? std::optional<bool>(m->get<bool>()) : std::nullopt;
}

struct OtioTime {
    double value = 0.0;
    double rate = 0.0;
};

std::optional<OtioTime> rationalTime(const Json* j) {
    if (!j || !j->is_object()) {
        return std::nullopt;
    }
    const auto value = numberMember(*j, "value");
    const auto rate = numberMember(*j, "rate");
    if (!value || !rate || *rate <= 0.0) {
        return std::nullopt;
    }
    return OtioTime{*value, *rate};
}

std::optional<Ticks> toTicks(const OtioTime& t, bool& exact) {
    const auto r = rationalTimeToTicks(t.value, t.rate);
    if (!r) {
        return std::nullopt;
    }
    exact = exact && r->exact;
    return Ticks{r->value};
}

struct OtioRange {
    Ticks start;
    Ticks duration;
    double rate = 0.0;
};

std::optional<OtioRange> timeRange(const Json* j, bool& exact) {
    if (!j || !j->is_object()) {
        return std::nullopt;
    }
    const auto start = rationalTime(member(*j, "start_time"));
    const auto duration = rationalTime(member(*j, "duration"));
    if (!start || !duration) {
        return std::nullopt;
    }
    const auto s = toTicks(*start, exact);
    const auto d = toTicks(*duration, exact);
    if (!s || !d) {
        return std::nullopt;
    }
    return OtioRange{*s, *d, duration->rate};
}

// ---- metadata.wabcut (OtioExporter.cpp) ----------------------------------------------------------------------------

/// The metadata.wabcut version this reader understands.
constexpr std::int64_t kWabcutMetadataVersion = 1;
/// Bounds on Wabcut metadata, which is as untrusted as the rest of the file.
constexpr std::size_t kMaxCurveKeys = 100'000;
constexpr std::size_t kMaxClipEffects = 1'000;
constexpr std::size_t kMaxTitleStyle = 256;
constexpr std::int64_t kMaxStreamIndex = 64;
constexpr std::size_t kMaxMediaStreams = 64;
constexpr std::int64_t kMaxMediaChannels = 1024;
constexpr std::int64_t kMaxFrameSize = 32768;
/// A Wabcut clip that is not linked: kept out of link inference (its links are exact), then reset to -1.
constexpr std::int64_t kNotLinked = std::numeric_limits<std::int64_t>::max();

/// A JSON integer that fits int64 (exact values are never written as floating point).
std::optional<std::int64_t> asInt64(const Json& j) {
    if (j.is_number_unsigned()) {
        const auto v = j.get<std::uint64_t>();
        if (v > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(v);
    }
    if (j.is_number_integer()) {
        return j.get<std::int64_t>();
    }
    return std::nullopt;
}

std::optional<std::int64_t> intMember(const Json& j, const char* key) {
    const Json* m = member(j, key);
    return m ? asInt64(*m) : std::nullopt;
}

std::optional<Ticks> ticksMember(const Json& j, const char* key) {
    const auto v = intMember(j, key);
    return v ? std::optional<Ticks>(Ticks{*v}) : std::nullopt;
}

/// {"num", "den"} as a valid, normalised rate.
std::optional<Rate> rateMember(const Json& j, const char* key) {
    const Json* m = member(j, key);
    if (!m || !m->is_object()) {
        return std::nullopt;
    }
    const auto num = intMember(*m, "num");
    const auto den = intMember(*m, "den");
    return num && den ? Rate::make(*num, *den) : std::nullopt;
}

std::optional<BezierHandle> handleFrom(const Json& j) {
    if (!j.is_object()) {
        return std::nullopt;
    }
    BezierHandle h;
    if (member(j, "slope")) {
        const auto slope = numberMember(j, "slope");
        if (!slope) {
            return std::nullopt;
        }
        h.slope = *slope;
    }
    if (member(j, "influence")) {
        const auto influence = numberMember(j, "influence");
        if (!influence) {
            return std::nullopt;
        }
        h.influence = *influence;
    }
    return h;
}

/// Keys [{"t": ticks, "v": value, "i"?, "in"?, "out"?}] as a well-formed curve, or nullopt.
std::optional<Curve> curveFrom(const Json& keys, std::size_t maxKeys) {
    if (!keys.is_array() || keys.empty() || keys.size() > maxKeys) {
        return std::nullopt;
    }
    Curve curve;
    curve.keys.reserve(keys.size());
    for (const Json& k : keys) {
        const auto t = intMember(k, "t");
        const auto v = numberMember(k, "v");
        if (!t || !v) {
            return std::nullopt;
        }
        Keyframe key;
        key.time = Ticks{*t};
        key.value = *v;
        if (const Json* i = member(k, "i")) {
            const auto interpolation =
                i->is_string() ? interpolationFromString(i->get_ref<const std::string&>()) : std::nullopt;
            if (!interpolation) {
                return std::nullopt;
            }
            key.interpolation = *interpolation;
        }
        if (const Json* in = member(k, "in")) {
            const auto handle = handleFrom(*in);
            if (!handle) {
                return std::nullopt;
            }
            key.in = *handle;
        }
        if (const Json* out = member(k, "out")) {
            const auto handle = handleFrom(*out);
            if (!handle) {
                return std::nullopt;
            }
            key.out = *handle;
        }
        curve.keys.push_back(key);
    }
    if (!curve.isWellFormed()) {
        return std::nullopt;
    }
    return curve;
}

/// {"value": x} or {"keys": [...]}.
std::optional<ParamValue> paramFrom(const Json& j) {
    if (const Json* keys = member(j, "keys")) {
        auto curve = curveFrom(*keys, kMaxCurveKeys);
        if (!curve) {
            return std::nullopt;
        }
        return ParamValue(std::move(*curve));
    }
    if (const auto value = numberMember(j, "value")) {
        return ParamValue(*value);
    }
    return std::nullopt;
}

/// Clip parameters by stable key. Unknown keys and invalid or out-of-range values are skipped and named in `rejected`.
ClipProperties propertiesFrom(const Json& j, std::vector<std::string>& rejected) {
    ClipProperties properties;
    if (!j.is_object()) {
        rejected.push_back("its parameters");
        return properties;
    }
    for (const auto& entry : j.items()) {
        const auto id = parameterByKey(entry.key());
        const auto value = paramFrom(entry.value());
        if (!id || !value || !value->isValid(parameterInfo(*id).minValue, parameterInfo(*id).maxValue)) {
            rejected.push_back(entry.key());
            continue;
        }
        properties[*id] = *value;
    }
    return properties;
}

std::optional<AudioChannelMap> channelsFrom(const Json& j) {
    const auto format = channelFormatFromString(stringMember(j, "format"));
    const Json* routes = member(j, "channels");
    if (!format || !routes || !routes->is_array() || routes->size() > static_cast<std::size_t>(kMaxClipChannels)) {
        return std::nullopt;
    }
    AudioChannelMap result;
    result.format = *format;
    for (const Json& r : *routes) {
        if (!r.is_object()) {
            return std::nullopt;
        }
        ChannelRoute route;
        if (const Json* sources = member(r, "sources")) {
            if (!sources->is_array() || sources->size() > static_cast<std::size_t>(kMaxRouteSources)) {
                return std::nullopt;
            }
            for (const Json& s : *sources) {
                const auto source = asInt64(s);
                if (!source || *source < 0 || *source >= kMaxSourceChannels) {
                    return std::nullopt;
                }
                route.sources.push_back(static_cast<std::int32_t>(*source));
            }
        }
        route.muted = boolMember(r, "muted").value_or(false);
        if (const Json* gain = member(r, "gainDb")) {
            auto value = paramFrom(*gain);
            if (!value) {
                return std::nullopt;
            }
            route.gainDb = std::move(*value);
        }
        result.channels.push_back(std::move(route));
    }
    if (channelMapProblem(result)) {
        return std::nullopt;
    }
    return result;
}

/// An effect data entry: {"points": [[x, y], ...]} or {"text": "..."}.
std::optional<EffectData> dataFrom(const std::string& key, const Json& j) {
    EffectData d;
    d.key = key;
    if (const Json* points = member(j, "points")) {
        if (!points->is_array() || points->size() > kMaxEffectPoints) {
            return std::nullopt;
        }
        std::vector<EffectPoint> list;
        for (const Json& p : *points) {
            if (!p.is_array() || p.size() != 2 || !p.front().is_number() || !p.back().is_number()) {
                return std::nullopt;
            }
            list.push_back({p.front().get<double>(), p.back().get<double>()});
        }
        d.value = std::move(list);
    } else if (const Json* text = member(j, "text"); text && text->is_string()) {
        d.value = text->get<std::string>();
    } else {
        return std::nullopt;
    }
    if (effectDataProblem(d)) {
        return std::nullopt;
    }
    return d;
}

/// A mask as .wabcut project files write it; an AI object mask (shape "matte") is never written, so never read.
std::optional<Mask> maskFrom(const Json& j) {
    const auto id = intMember(j, "id");
    const auto shape = maskShapeFromString(stringMember(j, "shape"));
    const auto mode = maskModeFromString(stringMember(j, "mode"));
    if (!id || *id < 0 || *id > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()) || !shape ||
        *shape == MaskShape::Matte || !mode) {
        return std::nullopt;
    }
    Mask m;
    m.id = static_cast<std::uint32_t>(*id);
    m.name = stringMember(j, "name");
    m.shape = *shape;
    m.mode = *mode;
    m.inverted = boolMember(j, "inverted").value_or(false);
    m.enabled = boolMember(j, "enabled").value_or(true);
    if (const Json* params = member(j, "params")) {
        if (!params->is_object()) {
            return std::nullopt;
        }
        for (const auto& entry : params->items()) {
            auto value = paramFrom(entry.value());
            if (!value) {
                return std::nullopt;
            }
            m.set(entry.key(), std::move(*value));
        }
    }
    if (const Json* path = member(j, "path")) {
        if (!path->is_array() || path->size() > kMaxMaskPathKeys) {
            return std::nullopt;
        }
        for (const Json& k : *path) {
            const auto t = intMember(k, "t");
            const auto interpolation = interpolationFromString(stringMember(k, "i"));
            const Json* vertices = member(k, "v");
            if (!t || !interpolation || !vertices || !vertices->is_array() || vertices->size() > kMaxMaskVertices) {
                return std::nullopt;
            }
            MaskPathKey key;
            key.time = Ticks{*t};
            key.interpolation = *interpolation;
            for (const Json& v : *vertices) {
                if (!v.is_array() || v.size() != 6) {
                    return std::nullopt;
                }
                double values[6] = {};
                for (std::size_t n = 0; n < 6; ++n) {
                    if (!v[n].is_number()) {
                        return std::nullopt;
                    }
                    values[n] = v[n].get<double>();
                }
                key.vertices.push_back({values[0], values[1], values[2], values[3], values[4], values[5]});
            }
            m.path.push_back(std::move(key));
        }
    }
    if (maskProblem(m)) {
        return std::nullopt;
    }
    return m;
}

/// An effect instance, checked as the model checks a stack (`why` says what is wrong). Its id stays unset: whoever adds
/// it to a project gives it one.
std::optional<EffectInstance> effectFrom(const Json& j, EffectKind kind, std::string& why) {
    EffectInstance e;
    e.effect = stringMember(j, "effect");
    if (e.effect.empty()) {
        why = "it has no effect id";
        return std::nullopt;
    }
    if (member(j, "version")) {
        const auto version = intMember(j, "version");
        if (!version || *version < 1 || *version > std::numeric_limits<std::int32_t>::max()) {
            why = "its version is not valid";
            return std::nullopt;
        }
        e.version = static_cast<std::int32_t>(*version);
    }
    e.enabled = boolMember(j, "enabled").value_or(true);
    if (const Json* params = member(j, "params")) {
        if (!params->is_object()) {
            why = "its parameters are not valid";
            return std::nullopt;
        }
        for (const auto& entry : params->items()) {
            auto value = paramFrom(entry.value());
            if (!value) {
                why = "its parameter " + entry.key() + " is not valid";
                return std::nullopt;
            }
            e.set(entry.key(), std::move(*value));
        }
    }
    e.mask = stringMember(j, "mask");
    if (const Json* data = member(j, "data")) {
        if (!data->is_object() || data->size() > kMaxEffectDataEntries) {
            why = "its data is not valid";
            return std::nullopt;
        }
        for (const auto& entry : data->items()) {
            auto d = dataFrom(entry.key(), entry.value());
            if (!d) {
                why = "its data " + entry.key() + " is not valid";
                return std::nullopt;
            }
            e.setData(std::move(*d));
        }
    }
    if (const Json* masks = member(j, "masks")) {
        if (!masks->is_array() || masks->size() > kMaxMasks) {
            why = "its masks are not valid";
            return std::nullopt;
        }
        for (const Json& m : *masks) {
            auto mask = maskFrom(m);
            if (!mask) {
                why = "a mask of it is not valid";
                return std::nullopt;
            }
            e.masks.push_back(std::move(*mask));
        }
    }
    e.id = EffectInstanceId{1}; // a stand-in id for the check
    if (const auto problem = effectStackProblem(EffectStack{e}, kind, 2)) {
        why = *problem;
        return std::nullopt;
    }
    e.id = EffectInstanceId{};
    return e;
}

std::optional<IrTitle> titleFrom(const Json& j) {
    const Json* text = member(j, "text");
    if (!text || !text->is_string()) {
        return std::nullopt;
    }
    IrTitle title;
    title.text = text->get<std::string>();
    title.fontFamily = stringMember(j, "fontFamily");
    title.fontStyle = stringMember(j, "fontStyle");
    if (const Json* style = member(j, "style")) {
        if (!style->is_object() || style->size() > kMaxTitleStyle) {
            return std::nullopt;
        }
        for (const auto& entry : style->items()) {
            if (entry.key().empty() || !entry.value().is_number()) {
                return std::nullopt;
            }
            const double value = entry.value().get<double>();
            if (!std::isfinite(value)) {
                return std::nullopt;
            }
            title.style.push_back(EffectParam{entry.key(), ParamValue(value)});
        }
    }
    return title;
}

/// A multicam source's angles; their track indices must name tracks the sequence has.
std::optional<IrMulticam> multicamFrom(const Json& j, std::size_t videoTracks, std::size_t audioTracks) {
    const auto sync = multicamSyncFromString(stringMember(j, "sync"));
    const Json* angles = member(j, "angles");
    if (!sync || !angles || !angles->is_array() || angles->empty() || angles->size() > kMaxMulticamAngles) {
        return std::nullopt;
    }
    IrMulticam multicam;
    multicam.sync = *sync;
    for (const Json& a : *angles) {
        if (!a.is_object()) {
            return std::nullopt;
        }
        IrMulticamAngle angle;
        angle.name = stringMember(a, "name");
        if (member(a, "video")) {
            const auto video = intMember(a, "video");
            if (!video || *video < 0 || static_cast<std::uint64_t>(*video) >= videoTracks) {
                return std::nullopt;
            }
            angle.videoTrack = static_cast<std::size_t>(*video);
        }
        if (const Json* audio = member(a, "audio")) {
            if (!audio->is_array() || audio->size() > audioTracks) {
                return std::nullopt;
            }
            for (const Json& t : *audio) {
                const auto track = asInt64(t);
                if (!track || *track < 0 || static_cast<std::uint64_t>(*track) >= audioTracks) {
                    return std::nullopt;
                }
                angle.audioTracks.push_back(static_cast<std::size_t>(*track));
            }
        }
        const auto offset = ticksMember(a, "offset");
        if (!offset || offset->value < 0 || *offset > kMaxMulticamOffset) {
            return std::nullopt;
        }
        angle.offset = *offset;
        multicam.angles.push_back(std::move(angle));
    }
    return multicam;
}

std::optional<TransitionKind> transitionKindFrom(std::string_view name) {
    static constexpr std::pair<TransitionKind, std::string_view> kKinds[] = {
        {TransitionKind::Dissolve, "dissolve"}, {TransitionKind::DipToBlack, "dipToBlack"},
        {TransitionKind::DipToWhite, "dipToWhite"}, {TransitionKind::Wipe, "wipe"},
        {TransitionKind::AudioCrossfade, "audioCrossfade"}, {TransitionKind::Other, "other"}};
    for (const auto& [kind, text] : kKinds) {
        if (text == name) {
            return kind;
        }
    }
    return std::nullopt;
}

bool isDissolve(TransitionKind kind) {
    return kind == TransitionKind::Dissolve || kind == TransitionKind::AudioCrossfade;
}

/// The clip's active media reference (as mediaFor picks it), or null.
const Json* activeReference(const Json& clip) {
    const Json* ref = member(clip, "media_reference");
    if (const Json* refs = member(clip, "media_references"); refs && refs->is_object()) {
        std::string key = stringMember(clip, "active_media_reference_key");
        if (key.empty()) {
            key = "DEFAULT_MEDIA";
        }
        ref = member(*refs, key.c_str());
    }
    return ref;
}

class OtioParser {
public:
    OtioParser(const ParseContext& context, IrDocument& doc) : m_ctx(context), m_doc(doc), m_media(doc) {}

    std::expected<void, Error> parseRoot(const Json& root);

private:
    SourceLocation at(const std::string& pointer) const {
        return {m_ctx.fileLabel, pointer.empty() ? "/" : pointer, 0};
    }
    void report(ReportOutcome o, ReportCategory c, std::string reason, const std::string& pointer) {
        m_doc.report.add(o, c, std::move(reason), at(pointer), m_seq ? m_seq->name : std::string());
    }

    std::expected<void, Error> parseTimeline(const Json& timeline, const std::string& pointer);
    /// The nested stacks met so far, each as a sequence of its own keyed "stack:<pointer>" (C.8), or by its Wabcut key.
    std::expected<void, Error> parsePendingStacks();
    void parseTrack(const Json& track, const std::string& pointer, std::size_t indexInStack);
    /// True when the clip's times came from its Wabcut metadata.
    bool parseClip(const Json& clip, const std::string& pointer, IrTrack& track, Ticks position,
                   Ticks duration, std::optional<OtioRange> sourceRange);
    std::size_t mediaFor(const Json& clip, const std::string& pointer, const std::string& clipName,
                         std::optional<Ticks>& availableStart, std::string& unsupportedWhy);
    void parseMarkers(const Json& owner, const std::string& pointer, Ticks offset,
                      std::optional<TimeRange> visible, const std::string& approximation);
    /// Reports the item's effects; a clip's constant time effect (LinearTimeWarp, FreezeFrame) is returned
    /// instead, for the builder to retime the clip. Time effects on tracks and stacks are reported.
    std::optional<IrSpeed> parseEffects(const Json& item, const std::string& pointer, const std::string& itemName,
                                        bool clip = false);
    std::optional<Ticks> itemDuration(const Json& item, std::optional<OtioRange>& range, int depth = 0);

    // metadata.wabcut (see the file comment).
    /// metadata.wabcut[section] of an object Wabcut wrote, or null: absent, malformed or another version (a newer
    /// version is reported once).
    const Json* wabcutSection(const Json& object, const char* section);
    /// Half a frame at the timeline's rate: exact times further than this from the OTIO times were changed in
    /// another editor after Wabcut wrote them, and the OTIO times are kept.
    std::int64_t halfFrame() const;
    bool agrees(Ticks exact, Ticks otio) const;
    void badMetadata(ReportCategory category, const std::string& what, const std::string& pointer);
    void applyMediaMetadata(const Json& meta, IrMedia& media, const std::string& pointer);
    void applyTrackMetadata(const Json& meta, IrTrack& track, const std::string& pointer);
    void applyTransitionMetadata(const Json& meta, IrTransition& transition, const std::string& pointer);
    void applyMarkerMetadata(const Json& meta, Marker& marker, const std::string& pointer);
    /// A clip's or nested stack's Wabcut data; true when its exact times were used.
    bool applyClipMetadata(const Json& meta, IrClip& clip, TrackKind kind, const std::string& pointer);
    /// The exact speed or time remap (none: 100 %) in place of OTIO's time effect; false when it is not valid (the
    /// OTIO speed is kept).
    bool applyRetiming(const Json& meta, IrClip& clip, const std::string& pointer);
    void applyEffects(const Json& list, IrClip& clip, TrackKind kind, const std::string& pointer);
    /// The sequence's Wabcut settings, once its tracks are read; true when they gave its frame size.
    bool applySequenceMetadata(const Json& meta, const std::string& pointer);

    /// A nested stack to parse as a sequence: the item, its JSON pointer and the key its nested clips name it by.
    struct PendingStack {
        const Json* item = nullptr;
        std::string pointer;
        std::string key;
    };

    const ParseContext& m_ctx;
    IrDocument& m_doc;
    MediaTable m_media;
    IrSequence* m_seq = nullptr;
    /// Nested stacks (compound clips) to parse after the current timeline, and the next one to parse.
    std::vector<PendingStack> m_pendingStacks;
    std::size_t m_nextPending = 0;
    /// Wabcut nests whose defining copy is queued, and the first other copy of each (it stands in when the defining
    /// copy was removed in another editor).
    std::set<std::string> m_definedStacks;
    std::map<std::string, PendingStack> m_referenceStacks;
    /// Wabcut link groups -> the document's link groups.
    std::map<std::int64_t, std::int64_t> m_linkGroups;
    std::optional<double> m_rateHint;
    /// The current timeline's exact frame rate (Wabcut metadata), and whether it has Wabcut metadata at all.
    std::optional<Rate> m_exactRate;
    bool m_exactTimes = false;
    bool m_newerMetadata = false;
    std::int64_t m_nextLink = 0;
    bool m_inexact = false;
};

std::optional<Ticks> OtioParser::itemDuration(const Json& item, std::optional<OtioRange>& range, int depth) {
    bool exact = true;
    range = timeRange(member(item, "source_range"), exact);
    if (!exact) {
        m_inexact = true;
    }
    if (range) {
        if (!m_rateHint && range->rate > 0.0) {
            m_rateHint = range->rate;
        }
        return range->duration;
    }
    // Untrimmed compositions last as long as their content: a stack its longest child, a track the sum of
    // its items (transitions take no time).
    const std::string_view schema = schemaName(item);
    if (schema == "Stack" || schema == "Track") {
        if (depth >= kMaxNesting) {
            return std::nullopt;
        }
        const Json* children = member(item, "children");
        Ticks total{};
        if (children && children->is_array()) {
            for (const Json& child : *children) {
                if (schemaName(child) == "Transition") {
                    continue;
                }
                std::optional<OtioRange> childRange;
                const auto d = itemDuration(child, childRange, depth + 1);
                if (!d) {
                    return std::nullopt;
                }
                if (schema == "Stack") {
                    total = std::max(total, *d);
                } else if (const auto sum = checkedAdd(total.value, d->value)) {
                    total = Ticks{*sum};
                } else {
                    return std::nullopt;
                }
            }
        }
        return total;
    }
    // A clip without a source range plays its media's available range.
    const Json* ref = member(item, "media_reference");
    if (const Json* refs = member(item, "media_references"); refs && refs->is_object()) {
        const std::string key = stringMember(item, "active_media_reference_key");
        if (const Json* active = member(*refs, key.empty() ? "DEFAULT_MEDIA" : key.c_str())) {
            ref = active;
        }
    }
    if (ref) {
        exact = true;
        if (auto available = timeRange(member(*ref, "available_range"), exact)) {
            range = available;
            return available->duration;
        }
    }
    return std::nullopt;
}

std::size_t OtioParser::mediaFor(const Json& clip, const std::string& pointer, const std::string& clipName,
                                 std::optional<Ticks>& availableStart, std::string& unsupportedWhy) {
    const Json* ref = member(clip, "media_reference");
    std::string refPointer = pointer + "/media_reference";
    if (const Json* refs = member(clip, "media_references"); refs && refs->is_object()) {
        std::string key = stringMember(clip, "active_media_reference_key");
        if (key.empty()) {
            key = "DEFAULT_MEDIA";
        }
        ref = member(*refs, key.c_str());
        refPointer = pointer + "/media_references/" + key;
    }
    const std::string_view schema = ref ? schemaName(*ref) : std::string_view("MissingReference");
    bool exact = true;
    if (ref) {
        if (const auto available = timeRange(member(*ref, "available_range"), exact)) {
            availableStart = available->start;
        }
    }
    IrMedia media;
    media.location = at(refPointer);
    if (schema == "ExternalReference") {
        const std::string url = stringMember(*ref, "target_url");
        bool decoded = false;
        media.reference = referenceFrom(url, stringMember(*ref, "name"), decoded);
        if (!decoded) {
            report(ReportOutcome::Approximated, ReportCategory::Media,
                   "media reference '" + url + "' of '" + clipName +
                       "' is not a file path; the media is offline",
                   refPointer);
        }
        media.key = "url:" + url;
        media.name = media.reference.name.empty() ? clipName : media.reference.name;
    } else if (schema == "MissingReference" || schema.empty()) {
        // OTIO's own "media unknown" marker; keep the clip offline under its name.
        const std::string name = ref ? stringMember(*ref, "name") : std::string();
        media.name = !name.empty() ? name : clipName;
        media.reference.name = media.name;
        media.reference.original = media.name;
        media.key = "missing:" + media.name;
    } else if (schema == "GeneratorReference") {
        unsupportedWhy =
            "generated media (" + stringMember(*ref, "generator_kind") + ") is not supported yet";
        return kNoMedia;
    } else if (schema == "ImageSequenceReference") {
        unsupportedWhy = "image sequences are not supported yet";
        return kNoMedia;
    } else {
        unsupportedWhy = "unknown media reference type " + std::string(schema);
        return kNoMedia;
    }
    if (availableStart) {
        if (const Json* available = member(*ref, "available_range")) {
            bool ok = true;
            if (const auto r = timeRange(available, ok)) {
                media.duration = r->duration;
            }
        }
    }
    media.stillImage = isStillImageName(media.reference.path.empty() ? media.name : media.reference.path);
    if (const Json* meta = ref ? wabcutSection(*ref, "media") : nullptr) {
        applyMediaMetadata(*meta, media, refPointer);
    }
    return m_media.add(std::move(media));
}

std::optional<IrSpeed> OtioParser::parseEffects(const Json& item, const std::string& pointer,
                                                const std::string& itemName, bool clip) {
    std::optional<IrSpeed> speed;
    const Json* effects = member(item, "effects");
    if (!effects || !effects->is_array()) {
        return speed;
    }
    for (std::size_t i = 0; i < effects->size(); ++i) {
        const Json& e = (*effects)[i];
        const std::string p = pointer + "/effects/" + std::to_string(i);
        const std::string_view schema = schemaName(e);
        if (schema == "FreezeFrame" || schema == "LinearTimeWarp") {
            // OTIO: the clip's source_range starts at the media In and lasts the clip's (timeline) duration;
            // the time scalar says how fast it plays that media.
            const double scalar = schema == "FreezeFrame" ? 0.0 : numberMember(e, "time_scalar").value_or(1.0);
            if (scalar == 1.0) {
                continue;
            }
            if (!clip || speed) {
                report(ReportOutcome::Approximated, ReportCategory::Speed,
                       "'" + itemName + "' has a time effect that is not on a clip (or a second one); it plays at 100%",
                       p);
                continue;
            }
            IrSpeed sp;
            sp.location = at(p);
            if (scalar == 0.0) {
                sp.ratio = wabcut::Speed{0, 1};
            } else if (const auto ratio = speedFromDouble(std::abs(scalar))) {
                sp.ratio = ratio->speed;
                sp.reversed = scalar < 0.0;
                if (!ratio->exact) {
                    sp.approximation = "the time scalar " + std::to_string(scalar) + " was rounded to " +
                                       speedLabel(ratio->speed);
                }
            } else {
                report(ReportOutcome::Approximated, ReportCategory::Speed,
                       "'" + itemName + "' has an unusable time scalar; it plays at 100%", p);
                continue;
            }
            speed = sp;
        } else if (schema == "TimeEffect") {
            // A non-linear time warp: OTIO has no standard curve for one (only LinearTimeWarp and FreezeFrame),
            // so there is nothing exact to remap; the clip keeps its length and media In at 100%.
            std::string name = stringMember(e, "effect_name");
            report(ReportOutcome::Approximated, ReportCategory::Speed,
                   "'" + itemName + "' has a non-linear time effect" + (name.empty() ? "" : " '" + name + "'") +
                       " that OTIO does not describe; it plays at 100%",
                   p);
        } else {
            std::string name = stringMember(e, "effect_name");
            if (name.empty()) {
                name = stringMember(e, "name");
            }
            report(ReportOutcome::Dropped, ReportCategory::Effect,
                   "effect '" + (name.empty() ? std::string(schema) : name) + "' on '" + itemName +
                       "' is not supported yet",
                   p);
        }
    }
    return speed;
}

void OtioParser::parseMarkers(const Json& owner, const std::string& pointer, Ticks offset,
                              std::optional<TimeRange> visible, const std::string& approximation) {
    const Json* markers = member(owner, "markers");
    if (!markers || !markers->is_array()) {
        return;
    }
    for (std::size_t i = 0; i < markers->size(); ++i) {
        const Json& m = (*markers)[i];
        const std::string p = pointer + "/markers/" + std::to_string(i);
        bool exact = true;
        auto range = timeRange(member(m, "marked_range"), exact);
        if (!range) {
            range = timeRange(member(m, "range"), exact); // Marker.1
        }
        const std::string name = stringMember(m, "name");
        if (!range) {
            report(ReportOutcome::Dropped, ReportCategory::Marker, "marker '" + name + "' has no time range",
                   p);
            continue;
        }
        if (visible && !(visible->contains(range->start) || range->start == visible->start)) {
            report(ReportOutcome::Dropped, ReportCategory::Marker,
                   "clip marker '" + name + "' lies outside the part of the clip used in the timeline", p);
            continue;
        }
        IrMarker marker;
        marker.marker.start = range->start + offset;
        marker.marker.duration = range->duration;
        marker.marker.name = name;
        marker.marker.comment = stringMember(m, "comment");
        if (const Json* color = member(m, "color"); color && color->is_object()) {
            // Marker.3: Color.1 {r, g, b, a, name} with components in [0, 1].
            const std::string colorName = stringMember(*color, "name");
            if (!colorName.empty()) {
                marker.marker.colorIndex = markerColorFromName(colorName);
            } else {
                auto component = [&](const char* key) {
                    return static_cast<int>(
                        std::lround(std::clamp(numberMember(*color, key).value_or(0.0), 0.0, 1.0) * 255.0));
                };
                marker.marker.colorIndex = markerColorFromRgb(component("r"), component("g"), component("b"));
            }
        } else {
            marker.marker.colorIndex = markerColorFromName(stringMember(m, "color"));
        }
        marker.approximation = approximation;
        marker.location = at(p);
        // Wabcut writes its sequence markers on the stack (sequence time); a clip marker's metadata is not its own.
        if (const Json* meta = !visible && offset.value == 0 ? wabcutSection(m, "marker") : nullptr) {
            applyMarkerMetadata(*meta, marker.marker, p);
        }
        m_seq->markers.push_back(std::move(marker));
    }
}

bool OtioParser::parseClip(const Json& clip, const std::string& pointer, IrTrack& track, Ticks position,
                           Ticks duration, std::optional<OtioRange> sourceRange) {
    std::string name = stringMember(clip, "name");
    const Json* meta = wabcutSection(clip, "clip");
    // A Wabcut title: a text generator to OTIO, its text and style in the metadata.
    std::optional<IrTitle> title;
    if (const Json* titleJson = meta ? member(*meta, "title") : nullptr) {
        const Json* ref = activeReference(clip);
        if (ref && schemaName(*ref) == "GeneratorReference") {
            title = titleFrom(*titleJson);
            if (!title) {
                badMetadata(ReportCategory::Clip, "title '" + name + "'", pointer);
            }
        }
    }
    std::optional<Ticks> availableStart;
    std::size_t media = kNoMedia;
    if (!title) {
        std::string unsupportedWhy;
        media = mediaFor(clip, pointer, name, availableStart, unsupportedWhy);
        if (name.empty() && media != kNoMedia) {
            name = m_doc.media[media].name;
        }
        if (media == kNoMedia) {
            report(ReportOutcome::Dropped, ReportCategory::Clip, "'" + name + "': " + unsupportedWhy, pointer);
            return false;
        }
    }
    IrClip c;
    c.name = name;
    c.media = media;
    c.stream = {streamKindOf(track.kind), 0};
    c.start = position;
    c.duration = duration;
    // OTIO source times are in the media's own timeline (often starting at its timecode); the model counts
    // from the media's first frame.
    const Ticks sourceStart = sourceRange ? sourceRange->start : Ticks{};
    c.sourceIn = availableStart ? sourceStart - *availableStart : sourceStart;
    c.enabled = boolMember(clip, "enabled").value_or(true);
    c.location = at(pointer);
    if (title) {
        c.title = std::move(title);
    } else if (track.kind == TrackKind::Video) {
        m_doc.media[media].hasVideo = true;
    } else if (m_doc.media[media].audioChannels.empty()) {
        m_doc.media[media].audioChannels.push_back(0);
    }
    c.speed = parseEffects(clip, pointer, name, true);
    const bool exact = meta && applyClipMetadata(*meta, c, track.kind, pointer);
    // Clip markers are in the clip's source time.
    parseMarkers(clip, pointer, position - sourceStart, TimeRange{sourceStart, duration},
                 "clip marker on '" + name + "' became a sequence marker (it no longer moves with the clip)");
    track.clips.push_back(std::move(c));
    return exact;
}

void OtioParser::parseTrack(const Json& trackJson, const std::string& pointer, std::size_t indexInStack) {
    const std::string kind = stringMember(trackJson, "kind");
    IrTrack track;
    if (kind == "Video") {
        track.kind = TrackKind::Video;
    } else if (kind == "Audio") {
        track.kind = TrackKind::Audio;
    } else {
        report(ReportOutcome::Dropped, ReportCategory::Track,
               "track " + std::to_string(indexInStack + 1) + " has kind '" + kind +
                   "' (only Video and Audio are imported)",
               pointer);
        return;
    }
    track.name = stringMember(trackJson, "name");
    track.enabled = boolMember(trackJson, "enabled").value_or(true);
    track.location = at(pointer);
    if (const Json* meta = wabcutSection(trackJson, "track")) {
        applyTrackMetadata(*meta, track, pointer);
    }
    if (member(trackJson, "source_range")) {
        report(ReportOutcome::Approximated, ReportCategory::Track,
               "track '" + track.name + "' has a source range; the whole track was imported", pointer);
    }
    parseEffects(trackJson, pointer, track.name.empty() ? "track" : track.name);

    const Json* children = member(trackJson, "children");
    Ticks position{};
    if (children && children->is_array()) {
        for (std::size_t i = 0; i < children->size(); ++i) {
            const Json& item = (*children)[i];
            const std::string p = pointer + "/children/" + std::to_string(i);
            const std::string_view schema = schemaName(item);
            if (schema == "Transition") {
                bool exact = true;
                const auto in = rationalTime(member(item, "in_offset"));
                const auto out = rationalTime(member(item, "out_offset"));
                const auto before = in ? toTicks(*in, exact) : std::optional<Ticks>(Ticks{});
                const auto after = out ? toTicks(*out, exact) : std::optional<Ticks>(Ticks{});
                if (!before || !after || before->value < 0 || after->value < 0) {
                    report(ReportOutcome::Dropped, ReportCategory::Transition,
                           "transition with invalid offsets", p);
                    continue;
                }
                IrTransition t;
                t.name = stringMember(item, "name");
                const std::string type = stringMember(item, "transition_type");
                t.kind = type == "SMPTE_Dissolve"
                             ? (track.kind == TrackKind::Audio ? TransitionKind::AudioCrossfade
                                                               : TransitionKind::Dissolve)
                             : TransitionKind::Other;
                if (t.name.empty()) {
                    t.name = type;
                }
                t.cut = position;
                t.before = *before;
                t.after = *after;
                t.location = at(p);
                if (const Json* meta = wabcutSection(item, "transition")) {
                    applyTransitionMetadata(*meta, t, p);
                }
                track.transitions.push_back(std::move(t));
                continue;
            }
            // Items whose exact times come from Wabcut metadata (and the gaps between them) round nothing.
            const bool wasInexact = m_inexact;
            std::optional<OtioRange> range;
            const auto duration = itemDuration(item, range);
            const std::string itemName = stringMember(item, "name");
            if (!duration || duration->value < 0) {
                report(ReportOutcome::Dropped, ReportCategory::Clip,
                       "'" + itemName + "' (" + std::string(schema) + ") has no usable duration", p);
                continue;
            }
            if (schema == "Gap") {
                track.gaps.push_back({{position, *duration}, at(p)});
                if (m_exactTimes) {
                    m_inexact = wasInexact;
                }
            } else if (schema == "Clip") {
                if (parseClip(item, p, track, position, *duration, range)) {
                    m_inexact = wasInexact;
                }
            } else if (schema == "Stack") {
                // C.8: a nested stack is a compound clip: a nested clip of a sequence made from its tracks.
                IrClip c;
                c.name = itemName.empty() ? std::string("Nested stack") : itemName;
                PendingStack nested{&item, p, "stack:" + p};
                bool queue = true;
                const Json* meta = wabcutSection(item, "clip");
                if (meta) {
                    // Wabcut writes a nested sequence wherever a clip plays it: the copy with a "sequence" section
                    // defines it, the others name it by key.
                    if (const std::string key = stringMember(*meta, "sequence"); !key.empty()) {
                        nested.key = "wabcut:" + key;
                        if (wabcutSection(item, "sequence")) {
                            queue = m_definedStacks.insert(nested.key).second;
                        } else {
                            queue = false;
                            m_referenceStacks.try_emplace(nested.key, nested);
                        }
                    }
                    c.enabled = boolMember(item, "enabled").value_or(true);
                }
                c.sequenceKey = nested.key;
                c.stream = {track.kind == TrackKind::Video ? StreamKind::Video : StreamKind::Audio, 0};
                c.start = position;
                c.duration = *duration;
                c.sourceIn = range ? range->start : Ticks{};
                c.speed = parseEffects(item, p, c.name, true);
                c.location = at(p);
                if (meta && applyClipMetadata(*meta, c, track.kind, p)) {
                    m_inexact = wasInexact;
                }
                track.clips.push_back(std::move(c));
                if (queue) {
                    m_pendingStacks.push_back(std::move(nested));
                }
            } else if (schema == "Track") {
                report(ReportOutcome::Dropped, ReportCategory::Clip,
                       "nested track '" + itemName + "' is not supported yet; its time is left as a gap", p);
            } else {
                report(ReportOutcome::Dropped, ReportCategory::Clip,
                       "unknown item type '" + std::string(schema) + "'; its time is left as a gap", p);
            }
            const auto next = checkedAdd(position.value, duration->value);
            if (!next) {
                report(ReportOutcome::Dropped, ReportCategory::Track,
                       "track runs past the representable time range", p);
                break;
            }
            position = Ticks{*next};
        }
    }
    // Track markers are in track time, which is sequence time for a track in the top-level stack.
    parseMarkers(trackJson, pointer, Ticks{}, std::nullopt, "track marker became a sequence marker");
    if (!m_seq->duration || position > *m_seq->duration) {
        m_seq->duration = position;
    }
    (track.kind == TrackKind::Video ? m_seq->videoTracks : m_seq->audioTracks).push_back(std::move(track));
}

std::expected<void, Error> OtioParser::parseTimeline(const Json& timeline, const std::string& pointer) {
    IrSequence sequence;
    sequence.name = stringMember(timeline, "name");
    // Wabcut's sequence settings (on a nested stack, whose OTIO name is its clip's, also the sequence's name).
    const Json* meta = wabcutSection(timeline, "sequence");
    if (meta) {
        if (std::string name = stringMember(*meta, "name"); !name.empty()) {
            sequence.name = std::move(name);
        }
    }
    if (sequence.name.empty()) {
        sequence.name = "Timeline " + std::to_string(m_doc.sequences.size() + 1);
    }
    sequence.location = at(pointer);
    m_doc.sequences.push_back(std::move(sequence));
    m_seq = &m_doc.sequences.back();
    m_rateHint.reset();
    m_exactRate = meta ? rateMember(*meta, "frameRate") : std::nullopt;
    m_exactTimes = meta != nullptr;

    const std::string_view schema = schemaName(timeline);
    const Json* stack = &timeline;
    std::string stackPointer = pointer;
    if (schema == "Timeline") {
        stack = member(timeline, "tracks");
        stackPointer = pointer + "/tracks";
        if (!stack || schemaName(*stack) != "Stack") {
            return std::unexpected(parseError("timeline '" + m_seq->name + "' has no track stack"));
        }
    }

    const auto globalStart = rationalTime(member(timeline, "global_start_time"));
    if (globalStart) {
        m_rateHint = globalStart->rate;
    }
    if (schemaName(*stack) == "Track") {
        parseTrack(*stack, stackPointer, 0);
    } else {
        const Json* children = member(*stack, "children");
        if (children && children->is_array()) {
            for (std::size_t i = 0; i < children->size(); ++i) {
                const Json& child = (*children)[i];
                const std::string p = stackPointer + "/children/" + std::to_string(i);
                if (schemaName(child) == "Track") {
                    parseTrack(child, p, i);
                } else {
                    report(ReportOutcome::Dropped, ReportCategory::Track,
                           "stack child " + std::to_string(i) + " is a " + std::string(schemaName(child)) +
                               ", not a track",
                           p);
                }
            }
        }
        parseMarkers(*stack, stackPointer, Ticks{}, std::nullopt, {});
        // A Wabcut nested stack's time effects are its clip's speed (read with the clip), not the sequence's.
        if (!wabcutSection(*stack, "clip")) {
            parseEffects(*stack, stackPointer, m_seq->name);
        }
    }

    // OTIO has no sequence settings: the rate comes from the timeline's times; the frame size is unknown.
    if (m_exactRate) {
        m_seq->frameRate = *m_exactRate;
    } else {
        const auto rate = m_rateHint ? rateFromDouble(*m_rateHint) : std::nullopt;
        m_seq->frameRate = rate.value_or(kRate24);
        if (!rate) {
            report(ReportOutcome::Approximated, ReportCategory::SequenceSetting,
                   "the timeline's frame rate is unknown; 24 fps assumed", pointer);
        }
    }
    const bool sized = meta && applySequenceMetadata(*meta, pointer);
    if (!sized) {
        report(ReportOutcome::Approximated, ReportCategory::SequenceSetting,
               "OpenTimelineIO has no frame size; 1920x1080 assumed", pointer);
    }
    if (globalStart && globalStart->value > 0.0) {
        // X.1: the sequence keeps its start timecode (Sequence::startTimecode); times count from it.
        bool exact = true;
        if (const auto start = toTicks(*globalStart, exact)) {
            m_seq->startTimecode = *start;
        }
    }
    std::stable_sort(m_seq->markers.begin(), m_seq->markers.end(),
                     [](const IrMarker& a, const IrMarker& b) { return a.marker.start < b.marker.start; });
    inferLinks(*m_seq, m_nextLink);
    // Clips Wabcut says are not linked sat out the inference; now they are plain unlinked clips.
    for (auto* tracks : {&m_seq->videoTracks, &m_seq->audioTracks}) {
        for (IrTrack& t : *tracks) {
            for (IrClip& c : t.clips) {
                if (c.linkGroup == kNotLinked) {
                    c.linkGroup = -1;
                }
            }
        }
    }
    if (m_inexact) {
        report(ReportOutcome::Approximated, ReportCategory::Clip,
               "some times were fractional frames and were rounded to the nearest tick", pointer);
        m_inexact = false;
    }
    m_seq = nullptr;
    return {};
}

std::expected<void, Error> OtioParser::parsePendingStacks() {
    for (;;) {
        while (m_nextPending < m_pendingStacks.size()) {
            // A copy: parsing appends to the list.
            const PendingStack next = m_pendingStacks[m_nextPending++];
            if (auto r = parseTimeline(*next.item, next.pointer); !r) {
                return r;
            }
            m_doc.sequences.back().key = next.key;
        }
        // A Wabcut nest whose defining copy is gone (its clip removed in another editor): another copy stands in.
        bool queued = false;
        for (const auto& [key, copy] : m_referenceStacks) {
            if (m_definedStacks.insert(key).second) {
                m_doc.report.add(ReportOutcome::Approximated, ReportCategory::Clip,
                                 "the nested sequence of '" + stringMember(*copy.item, "name") +
                                     "' lost the copy that defines it (edited in another editor); it was rebuilt "
                                     "from this copy, without its Wabcut settings",
                                 at(copy.pointer));
                m_pendingStacks.push_back(copy);
                queued = true;
            }
        }
        if (!queued) {
            return {};
        }
    }
}

// ---- metadata.wabcut ----------------------------------------------------------------------------------------------

const Json* OtioParser::wabcutSection(const Json& object, const char* section) {
    const Json* metadata = member(object, "metadata");
    const Json* data = metadata ? member(*metadata, "wabcut") : nullptr;
    if (!data || !data->is_object()) {
        return nullptr;
    }
    const auto version = intMember(*data, "version");
    if (!version || *version != kWabcutMetadataVersion) {
        if (version && *version > kWabcutMetadataVersion && !m_newerMetadata) {
            m_newerMetadata = true;
            m_doc.report.add(ReportOutcome::Approximated, ReportCategory::SequenceSetting,
                             "the file was written by a newer Wabcut (its Wabcut data is version " +
                                 std::to_string(*version) + "); only what OpenTimelineIO itself says was imported",
                             at(""));
        }
        return nullptr;
    }
    const Json* found = member(*data, section);
    return found && found->is_object() ? found : nullptr;
}

std::int64_t OtioParser::halfFrame() const {
    Rate rate = kRate24;
    if (m_exactRate) {
        rate = *m_exactRate;
    } else if (const auto hinted = m_rateHint ? rateFromDouble(*m_rateHint) : std::nullopt) {
        rate = *hinted;
    }
    const auto frame = framesToTicks(1, rate, Rounding::Nearest);
    return frame ? frame->value / 2 : 0;
}

bool OtioParser::agrees(Ticks exact, Ticks otio) const {
    const auto difference = checkedSub(exact.value, otio.value);
    const std::int64_t tolerance = halfFrame();
    return difference && *difference >= -tolerance && *difference <= tolerance;
}

void OtioParser::badMetadata(ReportCategory category, const std::string& what, const std::string& pointer) {
    report(ReportOutcome::Approximated, category,
           "Wabcut data for " + what + " is not valid; the OpenTimelineIO values were used", pointer);
}

void OtioParser::applyMediaMetadata(const Json& meta, IrMedia& media, const std::string& pointer) {
    const std::string key = stringMember(meta, "key");
    if (key.empty()) {
        badMetadata(ReportCategory::Media, "media '" + media.name + "'", pointer);
        return;
    }
    // The key keeps Wabcut's media items apart even when two share a file; the reference keeps a copy relinked in
    // another editor apart from the rest.
    media.key = "wabcut:" + key + "\n" + media.reference.original;
    const auto duration = ticksMember(meta, "duration");
    if (duration && media.duration && !agrees(*duration, *media.duration)) {
        return; // relinked to another file: what Wabcut knew of the old one does not apply
    }
    if (const auto v = boolMember(meta, "hasVideo")) {
        media.hasVideo = *v;
    }
    if (const auto v = boolMember(meta, "stillImage")) {
        media.stillImage = *v;
    }
    const auto width = intMember(meta, "width");
    const auto height = intMember(meta, "height");
    if (width && height && *width >= 0 && *height >= 0 && *width <= kMaxFrameSize && *height <= kMaxFrameSize) {
        media.width = static_cast<std::int32_t>(*width);
        media.height = static_cast<std::int32_t>(*height);
    }
    if (const auto r = rateMember(meta, "frameRate")) {
        media.frameRate = *r;
    }
    if (const auto r = rateMember(meta, "sampleRate")) {
        media.sampleRate = *r;
    }
    if (const Json* channels = member(meta, "audioChannels")) {
        std::vector<std::int32_t> counts;
        bool valid = channels->is_array() && channels->size() <= kMaxMediaStreams;
        for (std::size_t s = 0; valid && s < channels->size(); ++s) {
            const auto count = asInt64((*channels)[s]);
            valid = count && *count >= 0 && *count <= kMaxMediaChannels;
            if (valid) {
                counts.push_back(static_cast<std::int32_t>(*count));
            }
        }
        if (valid) {
            media.audioChannels = std::move(counts);
        } else {
            badMetadata(ReportCategory::Media, "the audio streams of '" + media.name + "'", pointer);
        }
    }
    if (duration && duration->value > 0) {
        media.duration = *duration;
    }
    if (const auto tc = ticksMember(meta, "startTimecode"); tc && tc->value >= 0) {
        media.startTimecode = *tc;
    }
    // X.1: the source's markers (Premiere's clip markers), in media time; each checked as the model checks markers.
    if (const Json* markers = member(meta, "markers"); markers && markers->is_array() && markers->size() <= 10'000) {
        media.markers.clear();
        for (const Json& k : *markers) {
            IrMarker marker;
            marker.marker.name = stringMember(k, "name");
            marker.marker.comment = stringMember(k, "comment");
            const auto start = ticksMember(k, "start");
            const auto length = ticksMember(k, "duration");
            const auto color = intMember(k, "colorIndex");
            if (!start || !length || start->value < 0 || length->value < 0) {
                badMetadata(ReportCategory::Marker, "a marker of '" + media.name + "'", pointer);
                continue;
            }
            marker.marker.start = *start;
            marker.marker.duration = *length;
            marker.marker.colorIndex = static_cast<std::int32_t>(std::clamp<std::int64_t>(color.value_or(0), 0, 15));
            if (markerProblem(marker.marker)) {
                badMetadata(ReportCategory::Marker, "marker '" + marker.marker.name + "' of '" + media.name + "'", pointer);
                continue;
            }
            marker.location = media.location;
            media.markers.push_back(std::move(marker));
        }
    }
}

void OtioParser::applyTrackMetadata(const Json& meta, IrTrack& track, const std::string& pointer) {
    // OTIO's enabled flag is what other editors play (off for a muted track); Wabcut's flags are exact.
    if (const auto v = boolMember(meta, "enabled")) {
        track.enabled = *v;
    }
    if (const auto v = boolMember(meta, "locked")) {
        track.locked = *v;
    }
    if (const auto v = boolMember(meta, "targeted")) {
        track.targeted = *v;
    }
    if (const auto v = boolMember(meta, "muted")) {
        track.muted = *v;
    }
    if (member(meta, "role")) {
        const auto role = trackRoleFromString(stringMember(meta, "role"));
        if (!role) {
            badMetadata(ReportCategory::Track, "the role of track '" + track.name + "'", pointer);
        } else if (track.kind == TrackKind::Audio) {
            track.role = *role;
        }
    }
}

void OtioParser::applyTransitionMetadata(const Json& meta, IrTransition& transition, const std::string& pointer) {
    // The kind OTIO cannot name (dips, wipes), unless another editor changed the transition's type since.
    const auto kind = transitionKindFrom(stringMember(meta, "kind"));
    if (kind && isDissolve(*kind) == isDissolve(transition.kind)) {
        transition.kind = *kind;
        // The model's effect id, when it is short and plain (addTransition checks it is a known transition).
        const std::string effect = stringMember(meta, "effect");
        if (!effect.empty() && effect.size() <= 64 &&
            std::all_of(effect.begin(), effect.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_'; })) {
            transition.effect = effect;
        }
    }
    const auto cut = ticksMember(meta, "cut");
    const auto before = ticksMember(meta, "before");
    const auto after = ticksMember(meta, "after");
    if (!cut || !before || !after || before->value < 0 || after->value < 0) {
        badMetadata(ReportCategory::Transition, "transition '" + transition.name + "'", pointer);
    } else if (agrees(*cut, transition.cut) && agrees(*before, transition.before) && agrees(*after, transition.after)) {
        transition.cut = *cut;
        transition.before = *before;
        transition.after = *after;
    } else {
        report(ReportOutcome::Approximated, ReportCategory::Transition,
               "transition '" + transition.name +
                   "' was changed in another editor after Wabcut wrote it; its OpenTimelineIO times were used",
               pointer);
    }
}

void OtioParser::applyMarkerMetadata(const Json& meta, Marker& marker, const std::string& pointer) {
    Marker exact = marker;
    const auto start = ticksMember(meta, "start");
    const auto duration = ticksMember(meta, "duration");
    if (start && duration && agrees(*start, marker.start) && agrees(*duration, marker.duration)) {
        exact.start = *start;
        exact.duration = *duration;
    } else {
        report(ReportOutcome::Approximated, ReportCategory::Marker,
               "marker '" + marker.name +
                   "' was moved in another editor after Wabcut wrote it (or its Wabcut time is not valid); its "
                   "OpenTimelineIO time was used",
               pointer);
    }
    // OTIO has eight colours: Wabcut's index is exact unless the colour was changed elsewhere.
    const auto color = intMember(meta, "colorIndex");
    if (color && *color >= std::numeric_limits<std::int32_t>::min() &&
        *color <= std::numeric_limits<std::int32_t>::max()) {
        const auto index = static_cast<std::int32_t>(*color);
        if ((index >= 0 && index < 8 ? index : 0) == marker.colorIndex) {
            exact.colorIndex = index;
        }
    }
    if (member(meta, "type")) {
        if (const auto type = markerTypeFromName(stringMember(meta, "type"))) {
            exact.type = *type;
        }
    }
    exact.url = stringMember(meta, "url");
    exact.target = stringMember(meta, "target");
    if (const Json* h = member(meta, "highlight"); h && h->is_object()) { // C.15
        MarkerHighlight highlight;
        highlight.score = numberMember(*h, "score").value_or(-1.0);
        highlight.confidence =
            highlightConfidenceFromString(stringMember(*h, "confidence")).value_or(HighlightConfidence::Medium);
        if (const Json* reasons = member(*h, "reasons"); reasons && reasons->is_array() &&
                                                       reasons->size() <= MarkerHighlight::kMaxReasons) {
            for (const Json& r : *reasons) {
                if (r.is_string()) {
                    highlight.reasons.push_back(r.get<std::string>());
                }
            }
        }
        // The peak keeps its offset into the marker when the marker was moved elsewhere.
        const auto peak = ticksMember(*h, "peak");
        highlight.peak = peak && start ? exact.start + (*peak - *start) : exact.start; // `start`: the Wabcut start above
        highlight.source = stringMember(*h, "source");
        exact.highlight = std::move(highlight);
    }
    if (const auto problem = markerProblem(exact)) {
        badMetadata(ReportCategory::Marker, "marker '" + marker.name + "' (" + *problem + ")", pointer);
        return;
    }
    marker = std::move(exact);
}

bool OtioParser::applyClipMetadata(const Json& meta, IrClip& clip, TrackKind kind, const std::string& pointer) {
    const std::string who = "'" + clip.name + "'";
    // Exact times, while they agree with the OTIO ones. A remapped clip's OTIO source start is a source position,
    // not its curve time, so only its place on the track is compared.
    const bool remapped = member(meta, "remap") != nullptr;
    const auto start = ticksMember(meta, "start");
    const auto duration = ticksMember(meta, "duration");
    const auto sourceIn = ticksMember(meta, "sourceIn");
    bool timed = false;
    if (!start || !duration || !sourceIn || duration->value <= 0) {
        badMetadata(ReportCategory::Clip, "the times of " + who, pointer);
    } else if (agrees(*start, clip.start) && agrees(*duration, clip.duration) &&
               (remapped || agrees(*sourceIn, clip.sourceIn))) {
        const Ticks otioSourceIn = clip.sourceIn;
        clip.start = *start;
        clip.duration = *duration;
        clip.sourceIn = *sourceIn;
        if (!applyRetiming(meta, clip, pointer) && remapped) {
            clip.sourceIn = otioSourceIn; // OTIO's speed runs from its source position, not from a curve time
        }
        timed = true;
    } else {
        report(ReportOutcome::Approximated, ReportCategory::Clip,
               who + " was moved or trimmed in another editor after Wabcut wrote it; its OpenTimelineIO times were "
                     "used" +
                   std::string(remapped ? ", and its time remap became the constant speed OpenTimelineIO gives" : ""),
               pointer);
    }
    if (const Json* stream = member(meta, "stream")) {
        const auto index = intMember(*stream, "index");
        const bool valid = stringMember(*stream, "kind") == (kind == TrackKind::Video ? "video" : "audio") && index &&
                           *index >= 0 && *index < kMaxStreamIndex;
        if (!valid) {
            badMetadata(ReportCategory::Clip, "the stream of " + who, pointer);
        } else if (clip.media != kNoMedia) {
            clip.stream.index = static_cast<std::int32_t>(*index); // nested clips and titles play stream 0
        }
    }
    // Exact links: a group Wabcut wrote, or none (not inferred).
    if (const auto group = intMember(meta, "linkGroup"); group && *group >= 0) {
        const auto [entry, inserted] = m_linkGroups.emplace(*group, m_nextLink);
        if (inserted) {
            ++m_nextLink;
        }
        clip.linkGroup = entry->second;
    } else {
        clip.linkGroup = kNotLinked;
    }
    if (const Json* properties = member(meta, "properties")) {
        std::vector<std::string> rejected;
        const ClipProperties values = propertiesFrom(*properties, rejected);
        if (!(values == ClipProperties{})) {
            clip.properties = values;
        }
        if (!rejected.empty()) {
            std::string list;
            for (const std::string& r : rejected) {
                list += (list.empty() ? "" : ", ") + r;
            }
            report(ReportOutcome::Approximated, ReportCategory::Effect,
                   who + ": Wabcut values that are not valid were not imported (" + list + ")", pointer);
        }
    }
    if (const Json* channels = member(meta, "channels"); channels && kind == TrackKind::Audio) {
        if (auto channelMap = channelsFrom(*channels)) {
            clip.channels = std::move(*channelMap);
        } else {
            report(ReportOutcome::Approximated, ReportCategory::AudioMapping,
                   who + ": its Wabcut channel map is not valid; the clip plays its stream as it is", pointer);
        }
    }
    if (const Json* effects = member(meta, "effects")) {
        applyEffects(*effects, clip, kind, pointer);
    }
    if (const Json* angles = member(meta, "angles"); angles && !clip.sequenceKey.empty()) {
        std::vector<std::size_t> picked;
        bool valid = angles->is_array() && angles->size() <= kMaxMulticamAngles;
        for (std::size_t i = 0; valid && i < angles->size(); ++i) {
            const auto angle = asInt64((*angles)[i]);
            valid = angle && *angle >= 0 && *angle < static_cast<std::int64_t>(kMaxMulticamAngles);
            if (valid) {
                picked.push_back(static_cast<std::size_t>(*angle));
            }
        }
        if (valid) {
            clip.angles = std::move(picked);
        } else {
            badMetadata(ReportCategory::Clip, "the multicam angles of " + who, pointer);
        }
    }
    return timed;
}

bool OtioParser::applyRetiming(const Json& meta, IrClip& clip, const std::string& pointer) {
    const std::optional<IrSpeed> otioSpeed = clip.speed;
    clip.speed.reset();
    clip.remap.reset();
    if (const Json* remap = member(meta, "remap")) {
        const auto curveIn = ticksMember(*remap, "curveIn");
        const Json* keys = member(*remap, "keys");
        auto curve = keys ? curveFrom(*keys, kMaxRemapKeys) : std::nullopt;
        if (!curveIn || !curve || remapProblem(*curve)) {
            badMetadata(ReportCategory::Speed, "the time remap of '" + clip.name + "'", pointer);
            clip.speed = otioSpeed;
            return false;
        }
        IrRemap r;
        r.curve = std::move(*curve);
        r.curveIn = *curveIn;
        r.location = clip.location;
        clip.remap = std::move(r);
        return true;
    }
    if (const Json* speed = member(meta, "speed")) {
        const auto num = intMember(*speed, "num");
        const auto den = intMember(*speed, "den");
        constexpr std::int64_t kMax = std::numeric_limits<std::int32_t>::max();
        if (!num || !den || *num < 0 || *den <= 0 || *num > kMax || *den > kMax) {
            badMetadata(ReportCategory::Speed, "the speed of '" + clip.name + "'", pointer);
            clip.speed = otioSpeed;
            return false;
        }
        IrSpeed sp;
        sp.ratio = *num == 0 ? wabcut::Speed{0, 1}
                             : wabcut::Speed{static_cast<std::int32_t>(*num), static_cast<std::int32_t>(*den)};
        sp.reversed = *num > 0 && boolMember(*speed, "reversed").value_or(false);
        sp.location = clip.location;
        if (!sp.ratio.isUnity() || sp.reversed) {
            clip.speed = sp;
        }
    }
    return true;
}

void OtioParser::applyEffects(const Json& list, IrClip& clip, TrackKind kind, const std::string& pointer) {
    if (!list.is_array()) {
        badMetadata(ReportCategory::Effect, "the effects of '" + clip.name + "'", pointer);
        return;
    }
    const EffectKind effectKind = kind == TrackKind::Video ? EffectKind::Video : EffectKind::Audio;
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (i == kMaxClipEffects) {
            report(ReportOutcome::Dropped, ReportCategory::Effect,
                   "'" + clip.name + "' has more than " + std::to_string(kMaxClipEffects) +
                       " effects; the rest were not imported",
                   pointer);
            break;
        }
        std::string why;
        if (auto effect = effectFrom(list[i], effectKind, why)) {
            clip.effects.push_back(std::move(*effect));
        } else {
            report(ReportOutcome::Dropped, ReportCategory::Effect,
                   "effect " + std::to_string(i + 1) + " ('" + stringMember(list[i], "effect") + "') of '" +
                       clip.name + "' was not imported: " + why,
                   pointer);
        }
    }
}

bool OtioParser::applySequenceMetadata(const Json& meta, const std::string& pointer) {
    IrSequence& s = *m_seq;
    if (const auto v = boolMember(meta, "dropFrame")) {
        s.dropFrame = *v;
    }
    bool sized = false;
    const auto width = intMember(meta, "width");
    const auto height = intMember(meta, "height");
    if (width && height && *width > 0 && *height > 0 && *width <= kMaxFrameSize && *height <= kMaxFrameSize) {
        s.width = static_cast<std::int32_t>(*width);
        s.height = static_cast<std::int32_t>(*height);
        sized = true;
    } else {
        badMetadata(ReportCategory::SequenceSetting, "the frame size of '" + s.name + "'", pointer);
    }
    if (const auto r = rateMember(meta, "audioRate")) {
        s.audioRate = *r;
    }
    if (const auto in = ticksMember(meta, "markIn"); in && in->value >= 0) {
        s.markIn = *in;
    }
    if (const auto out = ticksMember(meta, "markOut"); out && out->value >= 0) {
        s.markOut = *out;
    }
    // The exact duration (its trailing gap), unless the timeline was lengthened or shortened in another editor.
    if (const auto duration = ticksMember(meta, "duration"); duration && duration->value >= 0) {
        if (!s.duration || agrees(*duration, *s.duration)) {
            s.duration = *duration;
        } else {
            report(ReportOutcome::Approximated, ReportCategory::SequenceSetting,
                   "the length of '" + s.name +
                       "' was changed in another editor after Wabcut wrote it; its OpenTimelineIO length was used",
                   pointer);
        }
    }
    if (const Json* multicam = member(meta, "multicam")) {
        if (auto info = multicamFrom(*multicam, s.videoTracks.size(), s.audioTracks.size())) {
            s.multicam = std::move(*info);
        } else {
            badMetadata(ReportCategory::SequenceSetting, "the multicam angles of '" + s.name + "'", pointer);
        }
    }
    return sized;
}

std::expected<void, Error> OtioParser::parseRoot(const Json& root) {
    const std::string_view schema = schemaName(root);
    if (schema == "Timeline" || schema == "Stack" || schema == "Track") {
        if (auto r = parseTimeline(root, ""); !r) {
            return r;
        }
        return parsePendingStacks();
    }
    if (schema == "SerializableCollection") {
        const Json* children = member(root, "children");
        if (!children || !children->is_array()) {
            return std::unexpected(parseError("OTIO collection has no children"));
        }
        // Reserve so the m_seq pointer into m_doc.sequences stays valid while parsing.
        m_doc.sequences.reserve(children->size());
        for (std::size_t i = 0; i < children->size(); ++i) {
            const Json& child = (*children)[i];
            const std::string p = "/children/" + std::to_string(i);
            if (schemaName(child) == "Timeline") {
                if (auto r = parseTimeline(child, p); !r) {
                    return r;
                }
            } else {
                m_doc.report.add(ReportOutcome::Dropped, ReportCategory::Clip,
                                 "collection item " + std::to_string(i) + " (" +
                                     std::string(schemaName(child)) + ") is not a timeline",
                                 at(p));
            }
        }
        if (m_doc.sequences.empty()) {
            return std::unexpected(parseError("OTIO collection contains no timelines"));
        }
        return parsePendingStacks(); // nested stacks after the timelines, which keep their order
    }
    return std::unexpected(parseError("OTIO root is '" + std::string(schema) + "', not a Timeline"));
}

} // namespace

std::expected<IrDocument, Error> parseOtio(std::string_view data, const ParseContext& context) {
    Json root;
    try {
        root = Json::parse(data.begin(), data.end());
    } catch (const Json::parse_error& e) {
        return std::unexpected(parseError("not valid JSON (byte " + std::to_string(e.byte) + ")"));
    } catch (const std::exception&) {
        return std::unexpected(parseError("not valid JSON"));
    }
    if (schemaName(root).empty()) {
        return std::unexpected(parseError("JSON without OTIO_SCHEMA is not OpenTimelineIO"));
    }
    IrDocument doc;
    doc.sequences.reserve(1);
    OtioParser parser(context, doc);
    try {
        if (auto r = parser.parseRoot(root); !r) {
            return std::unexpected(r.error());
        }
    } catch (const std::exception& e) {
        // Defensive: the parser uses non-throwing accessors, but a type error must not escape.
        return std::unexpected(parseError(std::string("malformed OTIO: ") + e.what()));
    }
    return doc;
}

} // namespace wabcut::interchange::detail
