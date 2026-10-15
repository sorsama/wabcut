// OpenTimelineIO JSON (.otio) export, written so other editors read the cut and OtioImporter.cpp reads Wabcut's
// timeline back unchanged:
//
//   Timeline.1 { metadata.wabcut, name, global_start_time (0 at the sequence rate),
//                tracks: Stack.1 { markers: Marker.2 (the sequence markers), children: Track.1 V1..Vn, then A1..An } }
//   Track.1    { kind "Video" | "Audio", enabled, children: Clip.2 | Gap.1 | Transition.1 | Stack.1 }
//   Clip.2     { source_range, effects: LinearTimeWarp.1 | FreezeFrame.1, enabled,
//                media_references.DEFAULT_MEDIA: ExternalReference.1 (a file:/// URL) | MissingReference.1 (no path)
//                                                | GeneratorReference.1 (a title, generator_kind "Text") }
//   Stack.1    a nested or multicam clip: source_range in the nested sequence's time, its time effects, its tracks
//   Transition.1 { transition_type "SMPTE_Dissolve" (dissolves, crossfades) | "Custom_Transition", in_offset (before
//                the cut), out_offset (after it) }, between the two items that meet at its cut
//
// Times are RationalTime.1 values at the sequence's frame rate, fractional where a time is not on a frame. Gaps fill
// the space between clips and run every track to the sequence's duration (its trailing gap). A clip's source_range
// starts at its media In in the file's timecode when the file's duration is known (available_range then starts at the
// file's start timecode). A constant speed is a LinearTimeWarp (negative when reversed), a frame hold a FreezeFrame.
//
// OTIO has no references, so a nested sequence is a Stack of its tracks wherever a clip plays it. One copy, the first
// the importer meets, also carries its definition (the "sequence" section below); the others repeat the tracks for
// other editors (a multicam clip's other copies only its active angles' tracks; the defining copy all of them, the
// inactive ones with their output off), up to a budget past which repeated copies are written empty.
//
// metadata.wabcut holds what OTIO rounds or has no field for, exactly: {"version": 1, <section>: {...}}, the section
// naming what the object is. Ticks are integers (254016000000 per second), rates {"num", "den"}, a parameter
// {"value": x} or {"keys": [{"t": ticks, "v": value, "i": interpolation (absent: linear), "in" / "out": {"slope",
// "influence"} (absent: the default)}]} as in .wabcut project files; "?" marks a field written only when set.
//   Timeline, a nest's defining     "sequence": {key, name, frameRate, dropFrame, width, height, audioRate, duration?,
//   Stack                           markIn?, markOut?, multicam?: {sync, angles: [{name, video? (V index), audio: [A
//                                   indices], offset}]}}
//   Track.1                         "track": {name, enabled, locked, targeted?, muted, role}
//   Clip.2, a nested clip's Stack   "clip": {name, start, duration, sourceIn, stream: {kind, index}, enabled,
//                                   linkGroup?, speed?: {num, den, reversed}, remap?: {curveIn, keys}, properties?:
//                                   {stable parameter key: parameter} (the non-default ones), channels?: {format,
//                                   channels: [{sources, muted?, gainDb?}]}, effects?: [{effect, version, enabled,
//                                   params: {key: parameter}, mask?, data?: {key: {points: [[x, y]]} | {text}}, masks?:
//                                   [{id, name, shape, mode, inverted, enabled, params, path?}]}], title?: {text,
//                                   fontFamily, fontStyle, style: {key: number}}, sequence? (the key of the sequence
//                                   it plays), angles? (a multicam clip's active angles)}
//   Transition.1                    "transition": {kind, name, cut, before, after}
//   Marker.2                        "marker": {name, comment, start, duration, colorIndex, type?, url?, target?}
//   External/MissingReference.1     "media": {key, name, hasVideo, stillImage, width, height, frameRate?, sampleRate,
//                                   audioChannels: [channels per stream], duration?, startTimecode?}
// Names, marker comments and clip enabled flags are OTIO fields too, which the importer reads (so an edit made in
// another editor wins); a track's OTIO enabled flag is its output for other editors (off when muted).

#include "Exporters.h"
#include "TimeMath.h"

#include <wabcut/interchange/FileUrl.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/TimeRemap.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wabcut::interchange::detail {

namespace {

using Json = nlohmann::ordered_json;

/// The metadata.wabcut schema version written here (OtioImporter.cpp reads the same).
constexpr std::int64_t kMetadataVersion = 1;
/// Deepest chain of nested sequences written; a deeper nest is left as a gap.
constexpr int kMaxDepth = 32;
/// Items (clips, gaps, transitions, tracks) that repeated copies of nested sequences may add in all; copies past it
/// are written empty (Wabcut reads every nest from its defining copy).
constexpr std::uint64_t kRepeatBudget = 100'000;
/// Sizes saturate here, so the budget arithmetic never overflows.
constexpr std::uint64_t kHugeSize = std::uint64_t{1} << 62;
/// OTIO marker colours in Wabcut's colour order (ImporterCommon.h's markerColorFromName reads them back).
constexpr const char* kMarkerColors[] = {"GREEN", "RED", "PURPLE", "ORANGE", "YELLOW", "WHITE", "BLUE", "CYAN"};

std::uint64_t addSizes(std::uint64_t a, std::uint64_t b) {
    return std::min(kHugeSize, std::min(a, kHugeSize) + std::min(b, kHugeSize));
}

/// The metadata name of a transition kind (OtioImporter.cpp reads the same names).
std::string_view transitionKindName(TransitionKind kind) {
    switch (kind) {
    case TransitionKind::Dissolve:
        return "dissolve";
    case TransitionKind::DipToBlack:
        return "dipToBlack";
    case TransitionKind::DipToWhite:
        return "dipToWhite";
    case TransitionKind::Wipe:
        return "wipe";
    case TransitionKind::AudioCrossfade:
        return "audioCrossfade";
    case TransitionKind::Other:
        break;
    }
    return "other";
}

/// "150%", "33.33%", "-100%": a time scalar for messages.
std::string percentLabel(double scalar) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.2f", scalar * 100.0);
    std::string text = buffer;
    while (!text.empty() && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text + "%";
}

/// A whole number as a JSON integer, anything else as a double (dumped with round-trip precision).
Json number(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::abs(v) < 9.0e15) {
        return static_cast<std::int64_t>(v);
    }
    return v;
}

/// The rate RationalTimes are written at: `rate` itself when a reader can tell it from its decimal value (whole, NTSC
/// x/1001 and short decimal rates: TimeMath.h's rateFromDouble), otherwise the nearest such rate (Wabcut's exact
/// times are in the metadata either way).
Rate otioRate(Rate rate) {
    const Rate r = rate.normalized();
    if (!r.isValid()) {
        return kRate24;
    }
    const auto read = rateFromDouble(r.toDouble());
    if (read && *read == r) {
        return r;
    }
    if (read) {
        return *read;
    }
    const auto decimal = Rate::make(std::llround(r.toDouble() * 1000.0), 1000);
    if (decimal && rateFromDouble(decimal->toDouble()) == decimal) {
        return *decimal;
    }
    return kRate24;
}

Json rateValue(Rate rate) {
    if (rate.den == 1) {
        return std::int64_t{rate.num};
    }
    return rate.toDouble();
}

/// `t` in frames at `rate`: an integer on a frame boundary, a fractional frame count otherwise.
Json frameValue(Ticks t, Rate rate) {
    if (rate.isExact()) {
        const std::int64_t perFrame = rate.ticksPerUnit();
        const std::int64_t whole = floorDiv(t.value, perFrame);
        const std::int64_t rest = t.value - whole * perFrame;
        if (rest == 0) {
            return whole;
        }
        return static_cast<double>(whole) + static_cast<double>(rest) / static_cast<double>(perFrame);
    }
    const long double frames = static_cast<long double>(t.value) * static_cast<long double>(rate.num) /
                               (static_cast<long double>(rate.den) * static_cast<long double>(kTicksPerSecond));
    return static_cast<double>(frames);
}

/// RationalTime.1 at `rate` (an otioRate).
Json rationalTime(Ticks t, Rate rate) {
    Json j;
    j["OTIO_SCHEMA"] = "RationalTime.1";
    j["rate"] = rateValue(rate);
    j["value"] = frameValue(t, rate);
    return j;
}

Json timeRange(Ticks start, Ticks duration, Rate rate) {
    Json j;
    j["OTIO_SCHEMA"] = "TimeRange.1";
    j["duration"] = rationalTime(duration, rate);
    j["start_time"] = rationalTime(start, rate);
    return j;
}

Json rateJson(Rate rate) {
    const Rate r = rate.normalized();
    Json j;
    j["num"] = r.num;
    j["den"] = r.den;
    return j;
}

/// {"wabcut": {"version": 1, section: body}}: an object's metadata.
Json wabcutMetadata(const char* section, Json body) {
    Json data;
    data["version"] = kMetadataVersion;
    data[section] = std::move(body);
    Json out;
    out["wabcut"] = std::move(data);
    return out;
}

Json handleJson(const BezierHandle& h) {
    Json j;
    j["slope"] = number(h.slope);
    j["influence"] = number(h.influence);
    return j;
}

Json curveKeys(const Curve& curve) {
    Json keys = Json::array();
    for (const Keyframe& k : curve.keys) {
        Json key;
        key["t"] = k.time.value;
        key["v"] = number(k.value);
        if (k.interpolation != Interpolation::Linear) {
            key["i"] = std::string(toString(k.interpolation));
        }
        if (!(k.in == BezierHandle{})) {
            key["in"] = handleJson(k.in);
        }
        if (!(k.out == BezierHandle{})) {
            key["out"] = handleJson(k.out);
        }
        keys.push_back(std::move(key));
    }
    return keys;
}

/// A parameter value: {"value": x} or {"keys": [...]}.
Json parameterJson(const ParamValue& v) {
    Json j;
    if (const Curve* curve = v.curve()) {
        j["keys"] = curveKeys(*curve);
    } else {
        j["value"] = number(v.constant());
    }
    return j;
}

Json channelsJson(const AudioChannelMap& m) {
    Json routes = Json::array();
    for (const ChannelRoute& route : m.channels) {
        Json r;
        r["sources"] = route.sources;
        if (route.muted) {
            r["muted"] = true;
        }
        if (!(route.gainDb == ParamValue())) {
            r["gainDb"] = parameterJson(route.gainDb);
        }
        routes.push_back(std::move(r));
    }
    Json j;
    j["format"] = std::string(toString(m.format));
    j["channels"] = std::move(routes);
    return j;
}

/// A mask as .wabcut project files write it (path vertices as [x, y, inX, inY, outX, outY] rows).
Json maskJson(const Mask& m) {
    Json params = Json::object();
    for (const MaskParam& p : m.params) {
        params[p.key] = parameterJson(p.value);
    }
    Json j;
    j["id"] = m.id;
    j["name"] = m.name;
    j["shape"] = std::string(toString(m.shape));
    j["mode"] = std::string(toString(m.mode));
    j["inverted"] = m.inverted;
    j["enabled"] = m.enabled;
    j["params"] = std::move(params);
    if (!m.path.empty()) {
        Json keys = Json::array();
        for (const MaskPathKey& k : m.path) {
            Json vertices = Json::array();
            for (const MaskVertex& v : k.vertices) {
                vertices.push_back(Json::array(
                    {number(v.x), number(v.y), number(v.inX), number(v.inY), number(v.outX), number(v.outY)}));
            }
            Json key;
            key["t"] = k.time.value;
            key["i"] = std::string(toString(k.interpolation));
            key["v"] = std::move(vertices);
            keys.push_back(std::move(key));
        }
        j["path"] = std::move(keys);
    }
    return j;
}

/// A clip's effect stack. AI object masks are left out (their mattes live in this project's analysis cache): `notes`
/// says which.
Json effectsJson(const std::vector<EffectInstance>& effects, std::vector<std::string>& notes) {
    Json list = Json::array();
    for (const EffectInstance& e : effects) {
        Json params = Json::object();
        for (const EffectParam& p : e.params) {
            params[p.key] = parameterJson(p.value);
        }
        Json j;
        j["effect"] = e.effect;
        j["version"] = e.version;
        j["enabled"] = e.enabled;
        j["params"] = std::move(params);
        if (!e.mask.empty()) {
            j["mask"] = e.mask;
        }
        if (!e.data.empty()) {
            Json data = Json::object();
            for (const EffectData& d : e.data) {
                Json value;
                if (const std::vector<EffectPoint>* points = d.points()) {
                    Json pointList = Json::array();
                    for (const EffectPoint& p : *points) {
                        pointList.push_back(Json::array({number(p.x), number(p.y)}));
                    }
                    value["points"] = std::move(pointList);
                } else {
                    value["text"] = *d.text();
                }
                data[d.key] = std::move(value);
            }
            j["data"] = std::move(data);
        }
        Json masks = Json::array();
        for (const Mask& m : e.masks) {
            if (m.shape == MaskShape::Matte || m.matte) {
                notes.push_back("the object mask '" + m.name + "' of " + e.effect +
                                " was left out (its mattes are in this project's analysis cache)");
                continue;
            }
            masks.push_back(maskJson(m));
        }
        if (!masks.empty()) {
            j["masks"] = std::move(masks);
        }
        list.push_back(std::move(j));
    }
    return list;
}

Json titleJson(const IrTitle& t) {
    Json style = Json::object();
    for (const EffectParam& p : t.style) {
        style[p.key] = number(p.value.constant());
    }
    Json j;
    j["text"] = t.text;
    j["fontFamily"] = t.fontFamily;
    j["fontStyle"] = t.fontStyle;
    j["style"] = std::move(style);
    return j;
}

/// What other editors see of a title: a text generator with its text, font and style values.
Json titleReference(const IrTitle& t) {
    Json params;
    params["text"] = t.text;
    if (!t.fontFamily.empty()) {
        params["font_family"] = t.fontFamily;
    }
    if (!t.fontStyle.empty()) {
        params["font_style"] = t.fontStyle;
    }
    Json style = Json::object();
    for (const EffectParam& p : t.style) {
        style[p.key] = number(p.value.constant());
    }
    params["style"] = std::move(style);
    Json j;
    j["OTIO_SCHEMA"] = "GeneratorReference.1";
    j["metadata"] = Json::object();
    j["name"] = "Title";
    j["available_range"] = nullptr;
    j["available_image_bounds"] = nullptr;
    j["generator_kind"] = "Text";
    j["parameters"] = std::move(params);
    return j;
}

Json timeWarp(double scalar) {
    Json j;
    j["OTIO_SCHEMA"] = "LinearTimeWarp.1";
    j["metadata"] = Json::object();
    j["name"] = "";
    j["effect_name"] = "LinearTimeWarp";
    j["time_scalar"] = number(scalar);
    return j;
}

Json freezeFrame() {
    Json j;
    j["OTIO_SCHEMA"] = "FreezeFrame.1";
    j["metadata"] = Json::object();
    j["name"] = "";
    j["effect_name"] = "FreezeFrame";
    j["time_scalar"] = 0;
    return j;
}

Json gapJson(Ticks duration, Rate rate) {
    Json j;
    j["OTIO_SCHEMA"] = "Gap.1";
    j["metadata"] = Json::object();
    j["name"] = "";
    j["source_range"] = timeRange(Ticks{}, duration, rate);
    j["effects"] = Json::array();
    j["markers"] = Json::array();
    j["enabled"] = true;
    return j;
}

/// How OTIO places a clip in its source: the start of its source_range and its time effects.
struct OtioTiming {
    /// The lowest source time the clip shows (a reversed clip plays down to it), before any timecode offset.
    Ticks start;
    Json effects = Json::array();
    /// The speed other editors see, for reports ("50%", "freeze frame"); empty at 100%.
    std::string speed;
};

OtioTiming timingOf(const IrClip& c) {
    OtioTiming t;
    t.start = c.sourceIn;
    if (c.remap) {
        // OTIO has no curve: other editors get the constant speed that runs from the clip's first position to its last
        // (Wabcut's exact curve is in the metadata).
        const Curve& curve = c.remap->curve;
        if (curve.keys.empty() || c.duration.value <= 0) {
            return t;
        }
        const Ticks first = remapPositionAt(curve, c.remap->curveIn);
        const Ticks last = remapPositionAt(curve, c.remap->curveIn + c.duration);
        t.start = std::min(first, last);
        if (first == last) {
            t.effects.push_back(freezeFrame());
            t.speed = "freeze frame";
            return t;
        }
        const double scalar = static_cast<double>((last - first).value) / static_cast<double>(c.duration.value);
        if (last - first != c.duration) {
            t.effects.push_back(timeWarp(scalar));
        }
        t.speed = percentLabel(scalar);
        return t;
    }
    if (c.speed) {
        const wabcut::Speed ratio = c.speed->ratio;
        if (ratio.num == 0 || ratio.den <= 0) {
            t.effects.push_back(freezeFrame());
            t.speed = "freeze frame";
            return t;
        }
        double scalar = static_cast<double>(ratio.num) / static_cast<double>(ratio.den);
        if (c.speed->reversed) {
            scalar = -scalar;
        }
        if (scalar != 1.0) {
            t.effects.push_back(timeWarp(scalar));
        }
        t.speed = speedLabel(ratio) + (c.speed->reversed ? " reversed" : "");
    }
    return t;
}

class OtioWriter {
public:
    OtioWriter(const IrDocument& document, const ExportContext& context)
        : m_doc(document), m_ctx(context), m_sizes(document.sequences.size()),
          m_sizing(document.sequences.size(), false), m_definingClip(document.sequences.size(), nullptr) {
        for (const IrSequence& s : m_doc.sequences) {
            m_rates.push_back(otioRate(s.frameRate));
        }
        findDefiningClips();
    }

    std::string finish() {
        Json timeline;
        timeline["OTIO_SCHEMA"] = "Timeline.1";
        if (m_doc.sequences.empty()) {
            // describeForExport always gives a sequence; an empty timeline rather than nothing.
            timeline["metadata"] = Json::object();
            timeline["name"] = "";
            timeline["global_start_time"] = nullptr;
            timeline["tracks"] = stackJson(Json::array(), Json::array());
            return dump(timeline);
        }
        m_defined.insert(0);
        timeline["metadata"] = wabcutMetadata("sequence", sequenceMetadata(0));
        timeline["name"] = m_doc.sequences.front().name;
        // X.1: the sequence's start timecode (Sequence::startTimecode).
        timeline["global_start_time"] = rationalTime(m_doc.sequences.front().startTimecode.value_or(Ticks{}), m_rates.front());
        Json markers = writeMarkers(0, true);
        Json children = writeTracks(0, nullptr, true, 0);
        timeline["tracks"] = stackJson(std::move(markers), std::move(children));
        reportSequence(0);
        reportElided();
        return dump(timeline);
    }

private:
    static std::string dump(const Json& j) {
        // Invalid UTF-8 (which a valid project never holds) becomes U+FFFD rather than an exception.
        return j.dump(4, ' ', false, Json::error_handler_t::replace) + "\n";
    }

    /// The timeline's top-level stack.
    static Json stackJson(Json markers, Json children) {
        Json j;
        j["OTIO_SCHEMA"] = "Stack.1";
        j["metadata"] = Json::object();
        j["name"] = "tracks";
        j["source_range"] = nullptr;
        j["effects"] = Json::array();
        j["markers"] = std::move(markers);
        j["enabled"] = true;
        j["children"] = std::move(children);
        return j;
    }

    /// A track's clips in the order they are written (by start).
    static std::vector<const IrClip*> clipsInOrder(const IrTrack& track) {
        std::vector<const IrClip*> clips;
        for (const IrClip& c : track.clips) {
            clips.push_back(&c);
        }
        std::stable_sort(clips.begin(), clips.end(),
                         [](const IrClip* a, const IrClip* b) { return a->start < b->start; });
        return clips;
    }

    /// The clip whose copy of each nested sequence defines it: the first one OtioImporter.cpp meets. It reads the
    /// timeline, then each defining copy in the order it met them (its tracks V1..Vn, A1..An, their clips in time
    /// order) - describeForExport's order, so the sequences come back in the order they were exported.
    void findDefiningClips() {
        if (m_doc.sequences.empty()) {
            return;
        }
        std::vector<bool> seen(m_doc.sequences.size(), false);
        std::vector<std::size_t> order{0};
        seen[0] = true;
        for (std::size_t q = 0; q < order.size(); ++q) {
            const IrSequence& s = m_doc.sequences[order[q]];
            for (const std::vector<IrTrack>* tracks : {&s.videoTracks, &s.audioTracks}) {
                for (const IrTrack& t : *tracks) {
                    for (const IrClip* c : clipsInOrder(t)) {
                        if (c->sequence < m_doc.sequences.size() && !seen[c->sequence]) {
                            seen[c->sequence] = true;
                            m_definingClip[c->sequence] = c;
                            order.push_back(c->sequence);
                        }
                    }
                }
            }
        }
    }

    std::string sequenceKey(std::size_t index) const {
        const std::string& key = m_doc.sequences[index].key;
        return key.empty() ? "sequence-" + std::to_string(index + 1) : key;
    }

    // ---- metadata sections ---------------------------------------------------------------------------------------

    Json sequenceMetadata(std::size_t index) const {
        const IrSequence& s = m_doc.sequences[index];
        Json j;
        j["key"] = sequenceKey(index);
        j["name"] = s.name;
        j["frameRate"] = rateJson(s.frameRate);
        j["dropFrame"] = s.dropFrame;
        j["width"] = s.width;
        j["height"] = s.height;
        j["audioRate"] = rateJson(s.audioRate);
        if (s.duration) {
            j["duration"] = s.duration->value;
        }
        if (s.markIn) {
            j["markIn"] = s.markIn->value;
        }
        if (s.markOut) {
            j["markOut"] = s.markOut->value;
        }
        if (s.multicam) {
            Json angles = Json::array();
            for (const IrMulticamAngle& a : s.multicam->angles) {
                Json angle;
                angle["name"] = a.name;
                if (a.videoTrack) {
                    angle["video"] = *a.videoTrack;
                }
                angle["audio"] = a.audioTracks;
                angle["offset"] = a.offset.value;
                angles.push_back(std::move(angle));
            }
            Json multicam;
            multicam["sync"] = std::string(toString(s.multicam->sync));
            multicam["angles"] = std::move(angles);
            j["multicam"] = std::move(multicam);
        }
        return j;
    }

    static Json trackMetadata(const IrTrack& t) {
        Json j;
        j["name"] = t.name;
        j["enabled"] = t.enabled;
        j["locked"] = t.locked;
        if (t.targeted) {
            j["targeted"] = *t.targeted;
        }
        j["muted"] = t.muted;
        j["role"] = std::string(toString(t.role));
        return j;
    }

    Json clipMetadata(const IrClip& c, std::vector<std::string>& notes) const {
        Json j;
        j["name"] = c.name;
        j["start"] = c.start.value;
        j["duration"] = c.duration.value;
        j["sourceIn"] = c.sourceIn.value;
        Json stream;
        stream["kind"] = c.stream.kind == StreamKind::Video ? "video" : "audio";
        stream["index"] = c.stream.index;
        j["stream"] = std::move(stream);
        j["enabled"] = c.enabled;
        if (c.linkGroup >= 0) {
            j["linkGroup"] = c.linkGroup;
        }
        if (c.remap) {
            Json remap;
            remap["curveIn"] = c.remap->curveIn.value;
            remap["keys"] = curveKeys(c.remap->curve);
            j["remap"] = std::move(remap);
        } else if (c.speed) {
            Json speed;
            speed["num"] = c.speed->ratio.num;
            speed["den"] = c.speed->ratio.den;
            speed["reversed"] = c.speed->reversed;
            j["speed"] = std::move(speed);
        }
        if (c.properties) {
            Json properties = Json::object();
            for (const ParameterInfo& info : allParameters()) {
                if (!c.properties->isDefault(info.id)) {
                    properties[std::string(info.key)] = parameterJson((*c.properties)[info.id]);
                }
            }
            if (!properties.empty()) {
                j["properties"] = std::move(properties);
            }
        }
        if (c.channels) {
            j["channels"] = channelsJson(*c.channels);
        }
        if (!c.effects.empty()) {
            j["effects"] = effectsJson(c.effects, notes);
        }
        if (c.title) {
            j["title"] = titleJson(*c.title);
        }
        if (c.sequence < m_doc.sequences.size()) {
            j["sequence"] = sequenceKey(c.sequence);
            if (!c.angles.empty()) {
                j["angles"] = c.angles;
            }
        }
        return j;
    }

    static Json mediaMetadata(const IrMedia& m) {
        Json j;
        j["key"] = m.key;
        j["name"] = m.name;
        j["hasVideo"] = m.hasVideo;
        j["stillImage"] = m.stillImage;
        j["width"] = m.width;
        j["height"] = m.height;
        if (m.frameRate) {
            j["frameRate"] = rateJson(*m.frameRate);
        }
        j["sampleRate"] = rateJson(m.sampleRate);
        j["audioChannels"] = m.audioChannels;
        if (m.duration) {
            j["duration"] = m.duration->value;
        }
        if (m.startTimecode) {
            j["startTimecode"] = m.startTimecode->value;
        }
        if (!m.markers.empty()) { // X.1: the source's markers (Premiere's clip markers), media time
            Json markers = Json::array();
            for (const IrMarker& k : m.markers) {
                Json marker;
                marker["name"] = k.marker.name;
                marker["comment"] = k.marker.comment;
                marker["start"] = k.marker.start.value;
                marker["duration"] = k.marker.duration.value;
                marker["colorIndex"] = k.marker.colorIndex;
                markers.push_back(std::move(marker));
            }
            j["markers"] = std::move(markers);
        }
        return j;
    }

    // ---- sequences and tracks ------------------------------------------------------------------------------------

    Json writeMarkers(std::size_t index, bool defining) {
        const IrSequence& s = m_doc.sequences[index];
        Json list = Json::array();
        for (const IrMarker& m : s.markers) {
            const Marker& marker = m.marker;
            Json meta;
            meta["name"] = marker.name;
            meta["comment"] = marker.comment;
            meta["start"] = marker.start.value;
            meta["duration"] = marker.duration.value;
            meta["colorIndex"] = marker.colorIndex;
            if (marker.type != MarkerType::Comment) {
                meta["type"] = std::string(markerTypeName(marker.type));
            }
            if (!marker.url.empty()) {
                meta["url"] = marker.url;
            }
            if (!marker.target.empty()) {
                meta["target"] = marker.target;
            }
            if (marker.highlight) { // C.15: lossless here
                meta["highlight"] = Json{{"score", marker.highlight->score},
                                         {"confidence", std::string(toString(marker.highlight->confidence))},
                                         {"reasons", marker.highlight->reasons},
                                         {"peak", marker.highlight->peak.value},
                                         {"source", marker.highlight->source}};
            }
            const bool palette = marker.colorIndex >= 0 && marker.colorIndex < 8;
            Json j;
            j["OTIO_SCHEMA"] = "Marker.2";
            j["metadata"] = wabcutMetadata("marker", std::move(meta));
            j["name"] = marker.name;
            j["color"] = kMarkerColors[palette ? static_cast<std::size_t>(marker.colorIndex) : 0];
            j["marked_range"] = timeRange(marker.start, marker.duration, m_rates[index]);
            j["comment"] = marker.comment;
            list.push_back(std::move(j));
            if (defining) {
                std::string reason = "marker '" + marker.name + "'";
                if (marker.type != MarkerType::Comment) {
                    reason += " (a " + std::string(markerTypeName(marker.type)) +
                              " marker: other editors see a plain marker; Wabcut reads its type)";
                }
                m_ctx.add(ReportOutcome::Carried, ReportCategory::Marker, std::move(reason), m.location, s.name);
            }
        }
        return list;
    }

    /// Whether a multicam clip's copy of its source shows track `index` of `kind`: a track of its active angle(s), of
    /// the clip's own kind.
    static bool isActive(const IrSequence& source, const IrClip& via, TrackKind kind, std::size_t index) {
        if (!source.multicam) {
            return true;
        }
        if (streamKindOf(kind) != via.stream.kind) {
            return false;
        }
        for (const std::size_t a : via.angles) {
            if (a >= source.multicam->angles.size()) {
                continue;
            }
            const IrMulticamAngle& angle = source.multicam->angles[a];
            if (kind == TrackKind::Video ? angle.videoTrack == index
                                         : std::find(angle.audioTracks.begin(), angle.audioTracks.end(), index) !=
                                               angle.audioTracks.end()) {
                return true;
            }
        }
        return false;
    }

    /// The tracks of sequence `index`, V1..Vn then A1..An. `defining`: this copy defines the sequence (its elements
    /// are reported here, and the nests it defines are defined in it). `via` is the multicam clip a copy is written
    /// for: the defining copy has every track with only the active ones' output on, other copies only the active ones.
    Json writeTracks(std::size_t index, const IrClip* via, bool defining, int depth) {
        const IrSequence& s = m_doc.sequences[index];
        Json list = Json::array();
        for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
            const std::vector<IrTrack>& tracks = kind == TrackKind::Video ? s.videoTracks : s.audioTracks;
            for (std::size_t i = 0; i < tracks.size(); ++i) {
                std::optional<bool> enabled;
                if (via && s.multicam) {
                    const bool active = isActive(s, *via, kind, i);
                    if (!defining && !active) {
                        continue;
                    }
                    enabled = active;
                }
                list.push_back(writeTrack(index, tracks[i], kind, i, enabled, defining, depth));
            }
        }
        return list;
    }

    Json writeTrack(std::size_t seqIndex, const IrTrack& track, TrackKind kind, std::size_t index,
                    std::optional<bool> enabled, bool defining, int depth) {
        const IrSequence& s = m_doc.sequences[seqIndex];
        const bool muted = kind == TrackKind::Audio && track.muted;
        Json j;
        j["OTIO_SCHEMA"] = "Track.1";
        j["metadata"] = wabcutMetadata("track", trackMetadata(track));
        j["name"] = track.name;
        j["source_range"] = nullptr;
        j["effects"] = Json::array();
        j["markers"] = Json::array();
        // Other editors have no mute: a muted track is written with its output off (Wabcut reads both flags).
        j["enabled"] = enabled.value_or(track.enabled && !muted);
        j["children"] = writeItems(seqIndex, track, kind, index, defining, depth);
        j["kind"] = kind == TrackKind::Video ? "Video" : "Audio";
        if (defining) {
            std::string reason = (kind == TrackKind::Video ? "V" : "A") + std::to_string(index + 1) +
                                 (track.name.empty() ? std::string() : " '" + track.name + "'");
            if (muted && track.enabled) {
                reason += " (muted: other editors get it with its output off)";
            }
            m_ctx.add(ReportOutcome::Carried, ReportCategory::Track, std::move(reason), track.location, s.name);
        }
        return j;
    }

    /// The track's items in time order: clips, the gaps between them (and up to the sequence's end), and each
    /// transition between the items that meet at its cut (a gap is cut in two where a fade meets it).
    Json writeItems(std::size_t seqIndex, const IrTrack& track, TrackKind kind, std::size_t index, bool defining,
                    int depth) {
        const IrSequence& s = m_doc.sequences[seqIndex];
        const Rate rate = m_rates[seqIndex];
        const std::vector<const IrClip*> clips = clipsInOrder(track);
        std::vector<const IrTransition*> cuts;
        for (const IrTransition& t : track.transitions) {
            cuts.push_back(&t);
        }
        std::stable_sort(cuts.begin(), cuts.end(),
                         [](const IrTransition* a, const IrTransition* b) { return a->cut < b->cut; });

        Json list = Json::array();
        std::size_t next = 0; // the first transition not written yet
        const auto transitionsAt = [&](Ticks at) {
            for (; next < cuts.size() && cuts[next]->cut <= at; ++next) {
                if (cuts[next]->cut == at) {
                    list.push_back(writeTransition(seqIndex, *cuts[next], defining));
                } else if (defining) {
                    m_ctx.add(ReportOutcome::Dropped, ReportCategory::Transition,
                              "'" + cuts[next]->name + "' is not at the edge of a clip; it was not written",
                              cuts[next]->location, s.name);
                }
            }
        };
        const auto gapTo = [&](Ticks from, Ticks to) {
            transitionsAt(from);
            while (next < cuts.size() && cuts[next]->cut < to) {
                const Ticks cut = cuts[next]->cut;
                list.push_back(gapJson(cut - from, rate));
                from = cut;
                transitionsAt(from);
            }
            list.push_back(gapJson(to - from, rate));
        };
        Ticks cursor{};
        for (const IrClip* c : clips) {
            if (c->start > cursor) {
                gapTo(cursor, c->start);
                cursor = c->start;
            }
            transitionsAt(cursor);
            list.push_back(writeClip(seqIndex, *c, defining, depth));
            cursor = std::max(cursor, c->start + c->duration);
        }
        const Ticks end = std::max(cursor, s.duration.value_or(Ticks{}));
        if (end > cursor) {
            gapTo(cursor, end);
        }
        transitionsAt(end);
        for (; next < cuts.size(); ++next) {
            if (defining) {
                const std::string label = (kind == TrackKind::Video ? "V" : "A") + std::to_string(index + 1);
                m_ctx.add(ReportOutcome::Dropped, ReportCategory::Transition,
                          "'" + cuts[next]->name + "' lies past the end of " + label + "; it was not written",
                          cuts[next]->location, s.name);
            }
        }
        return list;
    }

    Json writeTransition(std::size_t seqIndex, const IrTransition& t, bool defining) {
        const IrSequence& s = m_doc.sequences[seqIndex];
        const bool dissolve = t.kind == TransitionKind::Dissolve || t.kind == TransitionKind::AudioCrossfade;
        Json meta;
        meta["kind"] = std::string(transitionKindName(t.kind));
        meta["name"] = t.name;
        meta["cut"] = t.cut.value;
        meta["before"] = t.before.value;
        meta["after"] = t.after.value;
        if (!t.effect.empty()) {
            meta["effect"] = t.effect; // the model's transition effect, exactly
        }
        Json j;
        j["OTIO_SCHEMA"] = "Transition.1";
        j["metadata"] = wabcutMetadata("transition", std::move(meta));
        j["name"] = t.name;
        j["in_offset"] = rationalTime(t.before, m_rates[seqIndex]);
        j["out_offset"] = rationalTime(t.after, m_rates[seqIndex]);
        j["transition_type"] = dissolve ? "SMPTE_Dissolve" : "Custom_Transition";
        if (defining) {
            m_ctx.add(ReportOutcome::Carried, ReportCategory::Transition,
                      "'" + t.name + "'" +
                          (dissolve ? std::string()
                                    : " (other editors see a custom transition of that name; Wabcut reads its kind)"),
                      t.location, s.name);
        }
        return j;
    }

    // ---- clips ---------------------------------------------------------------------------------------------------

    Json writeClip(std::size_t seqIndex, const IrClip& c, bool defining, int depth) {
        const IrSequence& s = m_doc.sequences[seqIndex];
        if (c.sequence != kNoSequence) {
            return writeNested(seqIndex, c, defining, depth);
        }
        if (!c.title && c.media >= m_doc.media.size()) {
            if (defining) {
                m_ctx.add(ReportOutcome::Dropped, ReportCategory::Clip,
                          "'" + c.name + "' has no media to refer to; its time is left as a gap", c.location, s.name);
            }
            return gapJson(c.duration, m_rates[seqIndex]);
        }
        const OtioTiming timing = timingOf(c);
        // Source times count from the file's start timecode where its available_range says so.
        Ticks origin{};
        if (!c.title) {
            const IrMedia& m = m_doc.media[c.media];
            if (m.duration) {
                origin = m.startTimecode.value_or(Ticks{});
            }
        }
        std::vector<std::string> notes;
        Json references;
        references["DEFAULT_MEDIA"] = c.title ? titleReference(*c.title) : writeMediaReference(c.media, seqIndex);
        Json j;
        j["OTIO_SCHEMA"] = "Clip.2";
        j["metadata"] = wabcutMetadata("clip", clipMetadata(c, notes));
        j["name"] = c.name;
        j["source_range"] = timeRange(origin + timing.start, c.duration, m_rates[seqIndex]);
        j["effects"] = timing.effects;
        j["markers"] = Json::array();
        j["enabled"] = c.enabled;
        j["media_references"] = std::move(references);
        j["active_media_reference_key"] = "DEFAULT_MEDIA";
        if (defining) {
            reportClip(s, c, timing, notes);
        }
        return j;
    }

    Json writeMediaReference(std::size_t index, std::size_t seqIndex) {
        const IrMedia& m = m_doc.media[index];
        const bool file = !m.reference.path.empty();
        Json j;
        j["OTIO_SCHEMA"] = file ? "ExternalReference.1" : "MissingReference.1";
        j["metadata"] = wabcutMetadata("media", mediaMetadata(m));
        j["name"] = m.name;
        if (m.duration) {
            const Rate rate = m.frameRate ? otioRate(*m.frameRate) : m_rates[seqIndex];
            j["available_range"] = timeRange(m.startTimecode.value_or(Ticks{}), *m.duration, rate);
        } else {
            j["available_range"] = nullptr;
        }
        j["available_image_bounds"] = nullptr;
        if (file) {
            j["target_url"] = pathToFileUrl(m.reference.path, FileUrlStyle::Triple);
        }
        if (m_mediaReported.insert(index).second) {
            const std::string where =
                file ? " (" + m.reference.path + ")" : std::string(" (no file path: other editors see it offline)");
            m_ctx.add(file ? ReportOutcome::Carried : ReportOutcome::Approximated, ReportCategory::Media,
                      "'" + m.name + "'" + where, m.location, {});
        }
        return j;
    }

    /// A nested or multicam clip: a Stack of the sequence it plays (the file comment says which copy defines it).
    Json writeNested(std::size_t seqIndex, const IrClip& c, bool defining, int depth) {
        const IrSequence& s = m_doc.sequences[seqIndex];
        if (c.sequence >= m_doc.sequences.size() || depth + 1 >= kMaxDepth) {
            if (defining) {
                m_ctx.add(ReportOutcome::Dropped, ReportCategory::Clip,
                          "'" + c.name + "' plays a sequence nested more than " + std::to_string(kMaxDepth) +
                              " deep (or a missing one); its time is left as a gap",
                          c.location, s.name);
            }
            return gapJson(c.duration, m_rates[seqIndex]);
        }
        const bool complete =
            defining && m_definingClip[c.sequence] == &c && m_defined.insert(c.sequence).second;
        bool elided = false;
        if (!complete) {
            const std::uint64_t size = copySize(c.sequence, &c, depth + 1);
            if (size > m_budget) {
                elided = true;
                ++m_elided[c.sequence];
            } else {
                m_budget -= size;
            }
        }
        const OtioTiming timing = timingOf(c);
        std::vector<std::string> notes;
        Json data;
        data["version"] = kMetadataVersion;
        data["clip"] = clipMetadata(c, notes);
        if (complete) {
            data["sequence"] = sequenceMetadata(c.sequence);
        }
        Json meta;
        meta["wabcut"] = std::move(data);
        Json j;
        j["OTIO_SCHEMA"] = "Stack.1";
        j["metadata"] = std::move(meta);
        j["name"] = c.name;
        j["source_range"] = timeRange(timing.start, c.duration, m_rates[seqIndex]);
        j["effects"] = timing.effects;
        j["markers"] = elided ? Json::array() : writeMarkers(c.sequence, complete);
        j["enabled"] = c.enabled;
        j["children"] = elided ? Json::array() : writeTracks(c.sequence, &c, complete, depth + 1);
        if (defining) {
            reportClip(s, c, timing, notes);
        }
        if (complete) {
            reportSequence(c.sequence);
        }
        return j;
    }

    // ---- the repeat budget ---------------------------------------------------------------------------------------

    std::uint64_t trackSize(const IrTrack& t, int depth) {
        // The track, its clips and transitions, and at most a gap before each clip and transition plus a trailing one.
        std::uint64_t total = 2 + 2 * static_cast<std::uint64_t>(t.clips.size()) +
                              2 * static_cast<std::uint64_t>(t.transitions.size());
        for (const IrClip& c : t.clips) {
            if (c.sequence < m_doc.sequences.size()) {
                total = addSizes(total, sequenceSize(c.sequence, depth + 1));
            }
        }
        return total;
    }

    /// Items one copy of sequence `index` writes (a multicam clip's other copy: its active tracks only), counting the
    /// nests inside it in full.
    std::uint64_t copySize(std::size_t index, const IrClip* via, int depth) {
        const IrSequence& s = m_doc.sequences[index];
        std::uint64_t total = 1;
        for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
            const std::vector<IrTrack>& tracks = kind == TrackKind::Video ? s.videoTracks : s.audioTracks;
            for (std::size_t i = 0; i < tracks.size(); ++i) {
                if (!via || isActive(s, *via, kind, i)) {
                    total = addSizes(total, trackSize(tracks[i], depth));
                }
            }
        }
        return total;
    }

    /// The full size of sequence `index` (memoised; a cycle, which the IR never has, counts nothing).
    std::uint64_t sequenceSize(std::size_t index, int depth) {
        if (m_sizes[index]) {
            return *m_sizes[index];
        }
        if (m_sizing[index] || depth > kMaxDepth) {
            return 0;
        }
        m_sizing[index] = true;
        const std::uint64_t total = copySize(index, nullptr, depth);
        m_sizing[index] = false;
        m_sizes[index] = total;
        return total;
    }

    // ---- reports -------------------------------------------------------------------------------------------------

    void reportSequence(std::size_t index) {
        const IrSequence& s = m_doc.sequences[index];
        const std::string size = std::to_string(s.width) + "x" + std::to_string(s.height);
        const std::string audio = std::to_string(s.audioRate.num / std::max(s.audioRate.den, 1)) + " Hz audio";
        m_ctx.add(ReportOutcome::Carried, ReportCategory::SequenceSetting,
                  rateLabel(s.frameRate) + " fps" + (s.dropFrame ? " drop-frame" : "") + ", " + size + ", " + audio +
                      " (OpenTimelineIO has no frame size or audio rate: other editors use their own; Wabcut "
                      "reads them)",
                  s.location, s.name);
        if (s.markIn || s.markOut) {
            m_ctx.add(ReportOutcome::Carried, ReportCategory::SequenceSetting,
                      "In/Out marks (other editors do not read them; Wabcut does)", s.location, s.name);
        }
        if (s.multicam) {
            m_ctx.add(ReportOutcome::Carried, ReportCategory::SequenceSetting,
                      "multicam source with " + std::to_string(s.multicam->angles.size()) + " angles (" +
                          std::string(toString(s.multicam->sync)) +
                          " sync; other editors see each multicam clip as a nested stack of its active angle)",
                      s.location, s.name);
        }
    }

    void reportClip(const IrSequence& s, const IrClip& c, const OtioTiming& timing,
                    const std::vector<std::string>& notes) {
        const std::string who = "'" + c.name + "'";
        std::string what = "clip " + who;
        ReportOutcome outcome = ReportOutcome::Carried;
        if (c.title) {
            what = "title " + who + " (other editors see a Text generator; Wabcut reads its text and style)";
            if (!c.title->approximation.empty()) {
                what += "; " + c.title->approximation;
                outcome = ReportOutcome::Approximated;
            }
        } else if (c.sequence < m_doc.sequences.size()) {
            const IrSequence& nested = m_doc.sequences[c.sequence];
            what = std::string(nested.multicam ? "multicam clip " : "nested clip ") + who + " of '" + nested.name + "'";
        }
        m_ctx.add(outcome, ReportCategory::Clip, std::move(what), c.location, s.name);
        if (c.remap) {
            m_ctx.add(ReportOutcome::Carried, ReportCategory::Speed,
                      who + " variable speed (" + std::to_string(c.remap->curve.keys.size()) +
                          " keys): OpenTimelineIO has no time remap, so other editors see a constant " + timing.speed +
                          "; Wabcut reads the exact curve",
                      c.location, s.name);
        } else if (c.speed) {
            m_ctx.add(ReportOutcome::Carried, ReportCategory::Speed, who + " " + timing.speed, c.location, s.name);
        }
        if (c.properties) {
            std::string list;
            for (const ParameterInfo& info : allParameters()) {
                if (!c.properties->isDefault(info.id)) {
                    list += (list.empty() ? "" : ", ") + std::string(info.displayName);
                }
            }
            if (!list.empty()) {
                m_ctx.add(ReportOutcome::Carried, ReportCategory::Effect,
                          who + " " + list + " (other editors do not read them; Wabcut does)", c.location, s.name);
            }
        }
        if (c.channels) {
            m_ctx.add(ReportOutcome::Carried, ReportCategory::AudioMapping,
                      who + " channel map (" + std::string(toString(c.channels->format)) +
                          "; other editors play the stream as it is; Wabcut reads the map)",
                      c.location, s.name);
        }
        if (!c.effects.empty()) {
            std::string list;
            for (const EffectInstance& e : c.effects) {
                list += (list.empty() ? "" : ", ") + e.effect;
            }
            std::string reason = who + " effects " + list + " (other editors do not see them; Wabcut does)";
            for (const std::string& n : notes) {
                reason += "; " + n;
            }
            m_ctx.add(notes.empty() ? ReportOutcome::Carried : ReportOutcome::Approximated, ReportCategory::Effect,
                      std::move(reason), c.location, s.name);
        }
    }

    void reportElided() {
        for (const auto& [index, count] : m_elided) {
            const IrSequence& s = m_doc.sequences[index];
            m_ctx.add(ReportOutcome::Approximated, ReportCategory::Clip,
                      "'" + s.name + "' is played by more clips than the file can repeat it for: " +
                          std::to_string(count) +
                          " of its copies are written empty (other editors show those clips empty; Wabcut reads the "
                          "sequence from its defining copy)",
                      s.location, s.name);
        }
    }

    const IrDocument& m_doc;
    const ExportContext& m_ctx;
    /// The RationalTime rate of each sequence (otioRate).
    std::vector<Rate> m_rates;
    /// Sequences whose defining copy is written.
    std::set<std::size_t> m_defined;
    std::set<std::size_t> m_mediaReported;
    /// Other copies written empty, per sequence.
    std::map<std::size_t, std::uint64_t> m_elided;
    std::uint64_t m_budget = kRepeatBudget;
    std::vector<std::optional<std::uint64_t>> m_sizes;
    std::vector<bool> m_sizing;
    /// Per sequence, the clip whose copy defines it (findDefiningClips); null for the exported one.
    std::vector<const IrClip*> m_definingClip;
};

} // namespace

std::string writeOtio(const IrDocument& document, const ExportContext& context) {
    OtioWriter writer(document, context);
    return writer.finish();
}

} // namespace wabcut::interchange::detail
