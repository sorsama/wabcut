#include <wabcut/model/ProjectIO.h>

#include <wabcut/model/Nesting.h>
#include <wabcut/model/RedactionMedia.h>

#include "JsonIo.h"
#include "ProjectChunks.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace wabcut::io {

namespace fs = std::filesystem;

namespace {

using nlohmann::json;
using detail::EnumName;
using detail::FormatError;
using detail::Reader;

// --- Enum names ---------------------------------------------------------------------------------------


constexpr EnumName<ColorMatrix> kColorMatrices[] = {
    {ColorMatrix::Unknown, "unknown"},          {ColorMatrix::BT601, "bt601"},
    {ColorMatrix::BT709, "bt709"},              {ColorMatrix::BT2020NonConstant, "bt2020nc"},
    {ColorMatrix::BT2020Constant, "bt2020c"},   {ColorMatrix::SMPTE240M, "smpte240m"},
    {ColorMatrix::RGB, "rgb"},
};
constexpr EnumName<ColorRange> kColorRanges[] = {
    {ColorRange::Unknown, "unknown"}, {ColorRange::Limited, "limited"}, {ColorRange::Full, "full"}};
constexpr EnumName<ColorPrimaries> kColorPrimaries[] = {
    {ColorPrimaries::Unknown, "unknown"},     {ColorPrimaries::BT709, "bt709"},
    {ColorPrimaries::BT601_525, "bt601-525"}, {ColorPrimaries::BT601_625, "bt601-625"},
    {ColorPrimaries::BT2020, "bt2020"},       {ColorPrimaries::DCIP3, "dci-p3"},
    {ColorPrimaries::DisplayP3, "display-p3"},
};
constexpr EnumName<TransferFunction> kTransfers[] = {
    {TransferFunction::Unknown, "unknown"}, {TransferFunction::BT709, "bt709"},
    {TransferFunction::SRGB, "srgb"},       {TransferFunction::Linear, "linear"},
    {TransferFunction::Gamma22, "gamma22"}, {TransferFunction::Gamma28, "gamma28"},
    {TransferFunction::PQ, "pq"},           {TransferFunction::HLG, "hlg"},
};
constexpr EnumName<ChromaLocation> kChromaLocations[] = {
    {ChromaLocation::Unknown, "unknown"}, {ChromaLocation::Left, "left"},
    {ChromaLocation::Center, "center"},   {ChromaLocation::TopLeft, "top-left"},
    {ChromaLocation::Top, "top"},         {ChromaLocation::BottomLeft, "bottom-left"},
    {ChromaLocation::Bottom, "bottom"},
};
constexpr EnumName<AlphaMode> kAlphaModes[] = {
    {AlphaMode::None, "none"}, {AlphaMode::Straight, "straight"}, {AlphaMode::Premultiplied, "premultiplied"}};

template <class E, std::size_t N>
const char* enumToName(const EnumName<E> (&table)[N], E value) {
    for (const auto& entry : table) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return table[0].name;
}

// --- Writing (current schema) -------------------------------------------------------------------------

json rateJson(Rate r) { return json{{"num", r.num}, {"den", r.den}}; }
json optionalTicks(const std::optional<Ticks>& t) { return t ? json(t->value) : json(nullptr); }

json fingerprintJson(const SourceFingerprint& f) {
    return json{{"size", f.size}, {"mtimeNs", f.mtimeNs}, {"sampleHash", f.sampleHash}};
}

json locatorJson(const MediaLocator& l) {
    json proxy = nullptr;
    if (l.proxy) {
        proxy = json{{"path", l.proxy->path},
                     {"provenance", json{{"generator", l.proxy->provenance.generator},
                                         {"settings", l.proxy->provenance.settings},
                                         {"source", fingerprintJson(l.proxy->provenance.source)},
                                         {"generation", l.proxy->provenance.generation}}}};
    }
    return json{{"path", l.path},
                {"relativePath", l.relativePath},
                {"fingerprint", fingerprintJson(l.fingerprint)},
                {"offline", l.offline},
                {"proxy", std::move(proxy)}};
}

json colorJson(const ColorInfo& c) {
    return json{{"matrix", enumToName(kColorMatrices, c.matrix)},
                {"range", enumToName(kColorRanges, c.range)},
                {"primaries", enumToName(kColorPrimaries, c.primaries)},
                {"transfer", enumToName(kTransfers, c.transfer)},
                {"chromaLocation", enumToName(kChromaLocations, c.chromaLocation)}};
}

json mediaInfoJson(const MediaInfo& info) {
    json video = json::array();
    for (const VideoStreamInfo& v : info.video) {
        video.push_back(json{{"index", v.index},
                             {"codec", v.codec},
                             {"profile", v.profile},
                             {"width", v.width},
                             {"height", v.height},
                             {"frameRate", rateJson(v.frameRate)},
                             {"variableFrameRate", v.variableFrameRate},
                             {"sampleAspect", rateJson(v.sampleAspect)},
                             {"pixelFormat", v.pixelFormat},
                             {"bitDepth", v.bitDepth},
                             {"chromaSubsampling", v.chromaSubsampling},
                             {"color", colorJson(v.color)},
                             {"alpha", enumToName(kAlphaModes, v.alpha)},
                             {"rotationDegrees", v.rotationDegrees},
                             {"duration", v.duration.value},
                             {"frameCount", v.frameCount}});
    }
    json audio = json::array();
    for (const AudioStreamInfo& a : info.audio) {
        json stream{{"index", a.index},
                    {"codec", a.codec},
                    {"sampleRate", rateJson(a.sampleRate)},
                    {"channels", a.channels},
                    {"channelLayout", a.channelLayout},
                    {"duration", a.duration.value}};
        if (!a.title.empty()) { // A.10, schema 15: only when the file names the stream
            stream["title"] = a.title;
        }
        audio.push_back(std::move(stream));
    }
    json out{{"path", info.path},
             {"container", info.container},
             {"duration", info.duration.value},
             {"fileSize", info.fileSize},
             {"modifiedTime", info.modifiedTime},
             {"isStillImage", info.isStillImage},
             {"video", std::move(video)},
             {"audio", std::move(audio)}};
    if (info.startTimecode) { // C.8: only when the file has one
        out["startTimecode"] = info.startTimecode->value;
    }
    return out;
}

json handleJson(const BezierHandle& h) { return json{{"slope", h.slope}, {"influence", h.influence}}; }

/// {"value": x} for a constant, {"keys": [{"t": ticks, "v": value, "i": interpolation, "in", "out"}]} for a
/// curve ("i" omitted for linear keys, handles omitted when they are the defaults).
json paramValueJson(const ParamValue& v) {
    const Curve* curve = v.curve();
    if (!curve) {
        return json{{"value", v.constant()}};
    }
    json keys = json::array();
    for (const Keyframe& k : curve->keys) {
        json key{{"t", k.time.value}, {"v", k.value}};
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
    return json{{"keys", std::move(keys)}};
}

json parametersJson(const ClipProperties& p) {
    // Only parameters that differ from their default (a constant or a curve) are written.
    json out = json::object();
    for (const ParameterInfo& info : allParameters()) {
        if (!p.isDefault(info.id)) {
            out[std::string(info.key)] = paramValueJson(p[info.id]);
        }
    }
    return out;
}

json effectParamsJson(const std::vector<EffectParam>& params) {
    json out = json::object();
    for (const EffectParam& p : params) {
        out[p.key] = paramValueJson(p.value);
    }
    return out;
}

/// Effect data (I.5): {"key": {"points": [[x, y], ...]} or {"text": "..."}}.
json effectDataJson(const std::vector<EffectData>& data) {
    json out = json::object();
    for (const EffectData& d : data) {
        if (const std::vector<EffectPoint>* points = d.points()) {
            json list = json::array();
            for (const EffectPoint& p : *points) {
                list.push_back(json::array({p.x, p.y}));
            }
            out[d.key] = json{{"points", std::move(list)}};
        } else {
            out[d.key] = json{{"text", *d.text()}};
        }
    }
    return out;
}

/// A mask (I.6): shape, mode, flags, keyframable params, and the path keys as [x, y, inX, inY, outX, outY] rows.
json maskJson(const Mask& m) {
    json params = json::object();
    for (const MaskParam& p : m.params) {
        params[p.key] = paramValueJson(p.value);
    }
    json out{{"id", m.id},
             {"name", m.name},
             {"shape", std::string(toString(m.shape))},
             {"mode", std::string(toString(m.mode))},
             {"inverted", m.inverted},
             {"enabled", m.enabled},
             {"params", std::move(params)}};
    if (!m.path.empty()) {
        json keys = json::array();
        for (const MaskPathKey& k : m.path) {
            json vertices = json::array();
            for (const MaskVertex& v : k.vertices) {
                vertices.push_back(json::array({v.x, v.y, v.inX, v.inY, v.outX, v.outY}));
            }
            keys.push_back(json{{"t", k.time.value}, {"i", std::string(toString(k.interpolation))}, {"v", std::move(vertices)}});
        }
        out["path"] = std::move(keys);
    }
    if (m.matte) {
        out["matte"] = json{{"track", m.matte->track},
                            {"media", m.matte->media.value},
                            {"stream", m.matte->stream},
                            {"source", json{{"start", m.matte->source.start.value}, {"duration", m.matte->source.duration.value}}},
                            {"model", m.matte->model}};
    }
    return out;
}

json masksJson(const std::vector<Mask>& masks) {
    json out = json::array();
    for (const Mask& m : masks) {
        out.push_back(maskJson(m));
    }
    return out;
}

json redactionJson(const Redaction& r) {
    json ranges = json::array();
    for (const TimeRange& t : r.audioRanges) {
        ranges.push_back(json::array({t.start.value, t.duration.value}));
    }
    json exceptions = json::array();
    for (const ClipId c : r.exceptions) {
        exceptions.push_back(c.value);
    }
    return json{{"id", r.id.value},
                {"media", r.media.value},
                {"name", r.name},
                {"enabled", r.enabled},
                {"source", json{{"start", r.source.start.value}, {"duration", r.source.duration.value}}},
                {"region", maskJson(r.region)},
                {"style", std::string(toString(r.style))},
                {"strength", r.strength},
                {"color", json::array({r.colorR, r.colorG, r.colorB})},
                {"audio", std::string(toString(r.audio))},
                {"audioRanges", std::move(ranges)},
                {"exceptions", std::move(exceptions)}};
}

/// Every instance is written as it is, known to the registry or not.
json effectStackJson(const EffectStack& stack) {
    json out = json::array();
    for (const EffectInstance& e : stack) {
        json item{{"id", e.id.value},
                  {"effect", e.effect},
                  {"version", e.version},
                  {"enabled", e.enabled},
                  {"params", effectParamsJson(e.params)}};
        if (!e.mask.empty()) {
            item["mask"] = e.mask;
        }
        if (!e.data.empty()) {
            item["data"] = effectDataJson(e.data); // only when present: other projects stay byte-identical
        }
        if (!e.masks.empty()) {
            item["masks"] = masksJson(e.masks); // I.6, likewise only when present
        }
        out.push_back(std::move(item));
    }
    return out;
}

/// A clip's "source" object (C.8): a nested sequence or multicam source with its angles; an unknown source kind as
/// it was read.
json clipSourceJson(const Clip& c) {
    if (c.hasUnknownSource()) {
        json raw = json::parse(c.content().unknownSource, nullptr, false);
        return raw.is_object() ? raw : json::object();
    }
    json out{{"type", "sequence"}, {"sequence", c.sequence.value}};
    if (!c.angles.empty()) {
        json angles = json::array();
        for (std::size_t i = 0; i < kMaxMulticamAngles; ++i) {
            if (c.angles.contains(i)) {
                angles.push_back(i);
            }
        }
        out["angles"] = std::move(angles);
    }
    return out;
}

/// A.9: a channel map; a route's muted flag and volume are written only when set.
json channelMapJson(const AudioChannelMap& m) {
    json channels = json::array();
    for (const ChannelRoute& route : m.channels) {
        json c{{"sources", route.sources}};
        if (route.muted) {
            c["muted"] = true;
        }
        if (!(route.gainDb == ParamValue())) {
            c["gainDb"] = paramValueJson(route.gainDb);
        }
        channels.push_back(std::move(c));
    }
    return json{{"format", std::string(toString(m.format))}, {"channels", std::move(channels)}};
}

json clipJson(const Clip& c) {
    json out{{"id", c.id.value},
                {"stream", json{{"kind", c.stream.kind == StreamKind::Video ? "video" : "audio"},
                                {"index", c.stream.index}}},
                {"start", c.timelineRange.start.value},
                {"duration", c.timelineRange.duration.value},
                {"sourceIn", c.sourceIn.value},
                {"speed", json{{"num", c.speed.num}, {"den", c.speed.den}}},
                {"enabled", c.enabled},
                {"linkGroup", c.linkGroup.value},
                {"name", c.name()},
                {"colorIndex", c.colorIndex},
                {"parameters", parametersJson(c.properties())}};
    if (c.reversed) {
        out["reversed"] = true;
    }
    if (c.maintainPitch) {
        out["maintainPitch"] = true;
    }
    if (!c.effects().empty()) {
        out["effects"] = effectStackJson(c.effects());
    }
    if (const Curve* remap = c.timeRemap()) {
        out["timeRemap"] = paramValueJson(ParamValue(*remap));
    }
    if (const AudioChannelMap* map = c.channelMap()) { // A.9: only when set
        out["channels"] = channelMapJson(*map);
    }
    if (const Graphic* graphic = c.graphic()) {
        out["graphic"] = detail::graphicToJsonValue(*graphic);
    }
    if (c.blendMode() != BlendMode::Normal) { // C.15: only when set
        out["blendMode"] = std::string(toString(c.blendMode()));
    }
    if (c.isNested() || c.hasUnknownSource()) {
        out["source"] = clipSourceJson(c); // instead of "media"
    } else {
        out["media"] = c.media.value;
    }
    return out;
}

json trackJson(const Track& t) {
    json clips = json::array();
    for (const Clip& c : t.clips) {
        clips.push_back(clipJson(c));
    }
    json transitions = json::array();
    for (const Transition& tr : t.transitions) {
        transitions.push_back(json{{"id", tr.id.value},
                                   {"outgoing", tr.outgoing.value},
                                   {"incoming", tr.incoming.value},
                                   {"before", tr.before.value},
                                   {"after", tr.after.value},
                                   {"alignment", std::string(toString(tr.alignment))},
                                   {"effect", tr.effect}});
        if (!tr.params.empty()) {
            transitions.back()["params"] = effectParamsJson(tr.params);
        }
    }
    json out{{"id", t.id.value},          {"name", t.name},         {"enabled", t.enabled},
             {"locked", t.locked},        {"syncLocked", t.syncLocked}, {"muted", t.muted},
             {"solo", t.solo},            {"targeted", t.targeted},     {"clips", std::move(clips)},
             {"transitions", std::move(transitions)}};
    if (t.recordArmed) { // A.8: written only when set
        out["recordArmed"] = true;
    }
    if (t.format != ChannelFormat::Stereo) { // A.9: only when not stereo
        out["format"] = std::string(toString(t.format));
    }
    if (!t.surround.isDefault()) { // A.9: only when moved
        out["surround"] = json{{"x", paramValueJson(t.surround.x)},
                               {"y", paramValueJson(t.surround.y)},
                               {"lfeDb", paramValueJson(t.surround.lfeDb)}};
    }
    if (t.role != TrackRole::None) {
        out["role"] = std::string(toString(t.role));
    }
    if (!t.effects.empty()) {
        out["effects"] = effectStackJson(t.effects);
    }
    // A.3: the track fader and panner, written only when not 0 dB / centre: other projects stay byte-identical.
    if (!(t.volume == ParamValue())) {
        out["volume"] = paramValueJson(t.volume);
    }
    if (!(t.pan == ParamValue())) {
        out["pan"] = paramValueJson(t.pan);
    }
    return out;
}

json beatGridJson(const std::optional<BeatGrid>& grid) {
    if (!grid) {
        return nullptr;
    }
    json beats = json::array();
    for (const Ticks t : grid->beats) {
        beats.push_back(t.value);
    }
    return json{{"bpmMilli", grid->bpmMilli},
                {"phase", grid->phase.value},
                {"beatsPerBar", grid->beatsPerBar},
                {"beats", std::move(beats)}};
}

json rgbJson(const CaptionRgb& c) { return json::array({c[0], c[1], c[2]}); }

json captionStyleFieldsJson(const CaptionStyle& s);

json captionStyleJson(const CaptionStyle& s) {
    json out = captionStyleFieldsJson(s);
    if (!s.brand.empty()) {
        json brand = json::object();
        for (const auto& [key, value] : {std::pair{"fill", &s.brand.fill}, std::pair{"outline", &s.brand.outline},
                                         std::pair{"shadow", &s.brand.shadow}, std::pair{"box", &s.brand.box},
                                         std::pair{"highlight", &s.brand.highlight}, std::pair{"font", &s.brand.font}}) {
            if (!value->empty()) {
                brand[key] = *value;
            }
        }
        out["brand"] = std::move(brand);
    }
    return out;
}

json captionStyleFieldsJson(const CaptionStyle& s) {
    return json{{"fontFamily", s.fontFamily},
                {"fontWeight", s.fontWeight},
                {"fontSize", s.fontSize},
                {"fill", rgbJson(s.fill)},
                {"outlineWidth", s.outlineWidth},
                {"outline", rgbJson(s.outline)},
                {"shadowOpacity", s.shadowOpacity},
                {"shadowColor", rgbJson(s.shadowColor)},
                {"shadowDistance", s.shadowDistance},
                {"shadowAngle", s.shadowAngle},
                {"shadowSoftness", s.shadowSoftness},
                {"box", s.box},
                {"boxColor", rgbJson(s.boxColor)},
                {"boxOpacity", s.boxOpacity},
                {"boxPadding", s.boxPadding},
                {"boxRadius", s.boxRadius},
                {"placement", std::string(toString(s.placement))},
                {"safeArea", s.safeArea},
                {"positionX", paramValueJson(s.positionX)},
                {"positionY", paramValueJson(s.positionY)},
                {"alignment", std::string(toString(s.alignment))},
                {"maxWidth", s.maxWidth},
                {"lineSpacing", s.lineSpacing},
                {"highlight", rgbJson(s.highlight)},
                {"animation", std::string(toString(s.animation))},
                {"showSpeakers", s.showSpeakers},
                {"allowOverlap", s.allowOverlap}};
}

json captionTracksJson(const std::vector<CaptionTrack>& tracks) {
    json out = json::array();
    for (const CaptionTrack& t : tracks) {
        json cues = json::array();
        for (const CaptionCue& c : t.cues) {
            cues.push_back(json{{"start", c.start.value}, {"duration", c.duration.value}, {"text", c.text}});
            if (c.speaker.isValid()) {
                cues.back()["speaker"] = c.speaker.value;
            }
        }
        out.push_back(json{{"id", t.id.value},
                           {"name", t.name},
                           {"language", t.language},
                           {"enabled", t.enabled},
                           {"cues", std::move(cues)},
                           {"style", captionStyleJson(t.style)}});
    }
    return out;
}

/// A marker; the M5i type and web-link fields only when set, so older builds read the file unchanged.
json markerJson(const Marker& m) {
    json out{{"start", m.start.value},
             {"duration", m.duration.value},
             {"name", m.name},
             {"comment", m.comment},
             {"colorIndex", m.colorIndex}};
    if (m.type != MarkerType::Comment) {
        out["type"] = std::string(markerTypeName(m.type));
    }
    if (!m.url.empty()) {
        out["url"] = m.url;
    }
    if (!m.target.empty()) {
        out["target"] = m.target;
    }
    if (m.highlight) { // C.15: only when set
        out["highlight"] = json{{"score", m.highlight->score},
                                {"confidence", std::string(toString(m.highlight->confidence))},
                                {"reasons", m.highlight->reasons},
                                {"peak", m.highlight->peak.value},
                                {"source", m.highlight->source}};
    }
    return out;
}

/// A sequence; `trackOut` (history chunks, C.9) writes each track in place of trackJson.
json sequenceJson(const Sequence& s, const std::function<json(const TrackPtr&)>& trackOut = {}) {
    const auto track = [&](const TrackPtr& t) { return trackOut ? trackOut(t) : trackJson(*t); };
    json video = json::array();
    for (const TrackPtr& t : s.videoTracks) {
        video.push_back(track(t));
    }
    json audio = json::array();
    for (const TrackPtr& t : s.audioTracks) {
        audio.push_back(track(t));
    }
    json markers = json::array();
    for (const Marker& m : s.markers) {
        markers.push_back(markerJson(m));
    }
    json workArea = nullptr;
    if (s.workArea) {
        workArea = json{{"start", s.workArea->start.value}, {"duration", s.workArea->duration.value}};
    }
    json out{{"id", s.id.value},
             {"bin", s.bin.value},
             {"name", s.name},
             {"frameRate", rateJson(s.frameRate)},
             {"width", s.width},
             {"height", s.height},
             {"audioRate", rateJson(s.audioRate)},
             {"dropFrame", s.dropFrame},
             {"duration", s.duration.value},
             {"workArea", std::move(workArea)},
             {"markIn", optionalTicks(s.markIn)},
             {"markOut", optionalTicks(s.markOut)},
             {"markers", std::move(markers)},
             {"videoTracks", std::move(video)},
             {"audioTracks", std::move(audio)},
             {"effects", effectStackJson(s.effects)},
             {"captionTracks", captionTracksJson(s.captionTracks)},
             {"beatGrid", beatGridJson(s.beatGrid)}};
    if (!s.audioEffects.empty()) {
        out["audioEffects"] = effectStackJson(s.audioEffects);
    }
    if (!(s.masterVolume == ParamValue())) { // A.3: only when not 0 dB, likewise
        out["masterVolume"] = paramValueJson(s.masterVolume);
    }
    if (s.masterFormat != ChannelFormat::Stereo) { // A.9: only when not stereo
        out["masterFormat"] = std::string(toString(s.masterFormat));
    }
    if (s.startTimecode.value != 0) { // X.1: only when set
        out["startTimecode"] = s.startTimecode.value;
    }
    if (s.derivedFrom) { // written only when set: other projects stay byte-identical
        json params = json::parse(s.derivedFrom->params, nullptr, false);
        out["derivedFrom"] = json{{"source", s.derivedFrom->source.value},
                                  {"generator", s.derivedFrom->generator},
                                  {"params", params.is_object() ? params : json::object()}};
    }
    if (s.multicam) { // C.8: only on multicam sources
        json angles = json::array();
        for (const MulticamAngle& a : s.multicam->angles) {
            json angleAudio = json::array();
            for (const TrackId t : a.audio) {
                angleAudio.push_back(t.value);
            }
            angles.push_back(json{{"name", a.name},
                                  {"video", a.video.value},
                                  {"audio", std::move(angleAudio)},
                                  {"offset", a.offset.value}});
        }
        out["multicam"] = json{{"sync", std::string(toString(s.multicam->sync))}, {"angles", std::move(angles)}};
    }
    return out;
}

/// The project; `sequenceOut` and `mediaOut` (history chunks, C.9) write each sequence and the media list in place of
/// their JSON.
json projectJson(const ProjectState& state, const std::function<json(const SequencePtr&)>& sequenceOut = {},
                 const std::function<json(json)>& mediaOut = {}) {
    json bins = json::array();
    for (const Bin& b : state.bins) {
        bins.push_back(json{{"id", b.id.value}, {"parent", b.parent.value}, {"name", b.name}});
    }
    json media = json::array();
    for (const MediaPtr& m : state.media) {
        json item{{"id", m->id.value},
                  {"bin", m->bin.value},
                  {"name", m->name},
                  {"locator", locatorJson(m->locator)},
                  {"info", mediaInfoJson(m->info)}};
        if (m->generator) {
            item["generator"] = json{{"kind", m->generator->kind},
                                     {"text", m->generator->text},
                                     {"fontFamily", m->generator->fontFamily},
                                     {"fontStyle", m->generator->fontStyle}};
            if (!m->generator->params.empty()) { // C.15: only when set
                item["generator"]["params"] = effectParamsJson(m->generator->params);
            }
        }
        // Source Monitor marks: only when set, so older builds read the file unchanged.
        if (m->markIn) {
            item["markIn"] = m->markIn->value;
        }
        if (m->markOut) {
            item["markOut"] = m->markOut->value;
        }
        // Clip markers (M5i): only when there are any.
        if (!m->markers.empty()) {
            json markers = json::array();
            for (const Marker& marker : m->markers) {
                markers.push_back(markerJson(marker));
            }
            item["markers"] = std::move(markers);
        }
        // AI provenance (AI-10): only for generated media, so other projects stay byte-identical.
        if (m->generated) {
            const GeneratedProvenance& g = *m->generated;
            item["generated"] = json{{"provider", g.provider}, {"model", g.model},     {"promptHash", g.promptHash},
                                     {"createdAt", g.createdAt}, {"kind", g.kind},     {"operation", g.operation},
                                     {"source", g.source.value}};
        }
        // Library metadata (M5k): each only when set, so other projects stay byte-identical and older readers ignore it.
        if (m->colorIndex != 0) {
            item["colorIndex"] = m->colorIndex;
        }
        if (!m->comment.empty()) {
            item["comment"] = m->comment;
        }
        if (!m->description.empty()) {
            item["description"] = m->description;
        }
        if (m->posterFrame) {
            item["posterFrame"] = m->posterFrame->value;
        }
        if (m->subclip) {
            const SubclipSpec& sub = *m->subclip;
            item["subclip"] = json{{"master", sub.master.value},
                                   {"start", sub.range.start.value},
                                   {"duration", sub.range.duration.value},
                                   {"restricted", sub.restricted}};
        }
        // A.4: offline audio takes and derived-media provenance, only when there are any.
        if (!m->audioTakes.empty()) {
            json takes = json::array();
            for (const AudioTake& t : m->audioTakes) {
                takes.push_back(json{{"stream", t.stream},     {"kind", t.kind},
                                     {"path", t.path},         {"model", t.model},
                                     {"source", fingerprintJson(t.source)}, {"generation", t.generation}});
            }
            item["audioTakes"] = std::move(takes);
        }
        if (m->derived) {
            const DerivedProvenance& d = *m->derived;
            item["derived"] = json{{"source", d.source.value}, {"operation", d.operation}, {"model", d.model},
                                   {"part", d.part},           {"createdAt", d.createdAt}};
        }
        if (m->channels) { // A.9: Modify > Audio Channels, only when set
            item["channels"] = channelMapJson(*m->channels);
        }
        if (m->recording) { // A.8: voice-over takes, only when set
            const RecordingProvenance& r = *m->recording;
            item["recording"] = json{{"sequence", r.sequence.value},
                                     {"track", r.track.value},
                                     {"start", r.range.start.value},
                                     {"duration", r.range.duration.value},
                                     {"take", r.take},
                                     {"device", r.device},
                                     {"latencyCompensation", r.latencyCompensation.value},
                                     {"createdAt", r.createdAt}};
        }
        media.push_back(std::move(item));
    }
    json sequences = json::array();
    for (const SequencePtr& s : state.sequences) {
        sequences.push_back(sequenceOut ? sequenceOut(s) : sequenceJson(*s));
    }
    json doc{{"format", std::string(kFormatName)},
             {"schemaVersion", kSchemaVersion},
             {"nextId", state.ids.next},
             {"bins", std::move(bins)},
             {"media", mediaOut ? mediaOut(std::move(media)) : std::move(media)},
             {"sequences", std::move(sequences)}};
    if (state.aiNoCloud) {
        // Optional and only written when set: unlocked projects stay byte-identical, older readers ignore it.
        doc["aiNoCloud"] = true;
    }
    if (!state.assets.empty()) {
        // Files besides media (LUTs, I.5): only written when there are some, like aiNoCloud.
        json assets = json::array();
        for (const ProjectAsset& a : state.assets) {
            assets.push_back(json{{"id", a.id.value}, {"kind", a.kind}, {"name", a.name}, {"locator", locatorJson(a.locator)}});
        }
        doc["assets"] = std::move(assets);
    }
    if (!state.redactions.empty()) {
        // Source-anchored redactions (I.6): only written when there are some.
        json redactions = json::array();
        for (const Redaction& r : state.redactions) {
            redactions.push_back(redactionJson(r));
        }
        doc["redactions"] = std::move(redactions);
    }
    // C.7: speakers, transcript edits and caption presets, only written when there are some.
    if (!state.speakers.empty()) {
        json speakers = json::array();
        for (const Speaker& s : state.speakers) {
            speakers.push_back(json{{"id", s.id.value}, {"name", s.name}, {"color", s.color}});
        }
        doc["speakers"] = std::move(speakers);
    }
    if (!state.transcriptEdits.empty()) {
        json edits = json::array();
        for (const TranscriptEdits& e : state.transcriptEdits) {
            json labels = json::array();
            for (const SpeakerLabel& l : e.labels) {
                labels.push_back(json::array({l.label, l.speaker.value}));
            }
            json spans = json::array();
            for (const SpeakerSpan& s : e.speakerSpans) {
                spans.push_back(json{{"start", s.source.start.value}, {"duration", s.source.duration.value},
                                     {"speaker", s.speaker.value}});
            }
            json corrections = json::array();
            for (const WordCorrection& c : e.corrections) {
                corrections.push_back(json{{"start", c.source.start.value}, {"duration", c.source.duration.value},
                                           {"original", c.original}, {"text", c.text}});
            }
            edits.push_back(json{{"media", e.media.value},
                                 {"audioStream", e.audioStream},
                                 {"labels", std::move(labels)},
                                 {"speakerSpans", std::move(spans)},
                                 {"corrections", std::move(corrections)}});
        }
        doc["transcriptEdits"] = std::move(edits);
    }
    if (!state.captionPresets.empty()) {
        json presets = json::array();
        for (const CaptionPreset& p : state.captionPresets) {
            presets.push_back(json{{"name", p.name}, {"style", captionStyleJson(p.style)}});
        }
        doc["captionPresets"] = std::move(presets);
    }
    if (!state.reviewNotes.empty()) { // C.9: only when there are some
        json notes = json::array();
        for (const ReviewNote& n : state.reviewNotes) {
            json replies = json::array();
            for (const ReviewReply& r : n.replies) {
                replies.push_back(json{{"author", r.author}, {"text", r.text}, {"createdAt", r.createdAt}});
            }
            json note{{"id", n.id.value},
                      {"sequence", n.sequence.value},
                      {"start", n.start.value},
                      {"duration", n.duration.value},
                      {"author", n.author},
                      {"text", n.text},
                      {"createdAt", n.createdAt},
                      {"status", std::string(toString(n.status))},
                      {"replies", std::move(replies)}};
            if (n.mark) {
                note["mark"] = json{{"x", n.mark->x}, {"y", n.mark->y}, {"width", n.mark->width}, {"height", n.mark->height}};
            }
            notes.push_back(std::move(note));
        }
        doc["reviewNotes"] = std::move(notes);
    }
    if (state.brandKit) {
        doc["brandKit"] = detail::brandKitToJsonValue(*state.brandKit);
    }
    if (!state.templates.empty()) {
        json templates = json::array();
        for (const GraphicTemplate& t : state.templates) {
            templates.push_back(detail::templateToJsonValue(t));
        }
        doc["templates"] = std::move(templates);
    }
    return doc;
}

// --- Migrations ---------------------------------------------------------------------------------------


json unknownFingerprintJson() { return json{{"size", -1}, {"mtimeNs", 0}, {"sampleHash", ""}}; }

/// Schema 1 -> 2: media locators, explicit sequence durations, clip parameters, transitions.
void migrate1To2(json& doc) {
    if (doc.contains("media") && doc["media"].is_array()) {
        for (json& m : doc["media"]) {
            if (!m.is_object()) {
                continue;
            }
            json proxy = nullptr;
            if (m.contains("proxyPath") && m["proxyPath"].is_string()) {
                proxy = json{{"path", m["proxyPath"]},
                             {"provenance", json{{"generator", ""},
                                                 {"settings", ""},
                                                 {"source", unknownFingerprintJson()},
                                                 {"generation", 0}}}};
            }
            m["locator"] = json{{"path", m.contains("path") ? m["path"] : json("")},
                                {"relativePath", ""},
                                {"fingerprint", unknownFingerprintJson()},
                                {"offline", false},
                                {"proxy", std::move(proxy)}};
            m.erase("path");
            m.erase("proxyPath");
        }
    }
    struct ScalarRename {
        const char* from;
        const char* to;
    };
    struct VectorRename {
        const char* from;
        const char* toX;
        const char* toY;
    };
    static constexpr ScalarRename kScalars[] = {{"scale", "motion.scale"},
                                                {"rotation", "motion.rotation"},
                                                {"opacity", "opacity.opacity"},
                                                {"volumeDb", "volume.level"},
                                                {"pan", "panner.balance"}};
    static constexpr VectorRename kVectors[] = {{"position", "motion.position.x", "motion.position.y"},
                                                {"anchor", "motion.anchor.x", "motion.anchor.y"}};
    if (!doc.contains("sequences") || !doc["sequences"].is_array()) {
        return;
    }
    for (json& s : doc["sequences"]) {
        if (!s.is_object()) {
            continue;
        }
        std::int64_t end = 0;
        for (const char* list : {"videoTracks", "audioTracks"}) {
            if (!s.contains(list) || !s[list].is_array()) {
                continue;
            }
            for (json& t : s[list]) {
                if (!t.is_object() || !t.contains("clips") || !t["clips"].is_array()) {
                    continue;
                }
                t["transitions"] = json::array();
                for (json& c : t["clips"]) {
                    if (!c.is_object()) {
                        continue;
                    }
                    if (c.contains("start") && c.contains("duration") && c["start"].is_number_integer() &&
                        c["duration"].is_number_integer()) {
                        const auto clipEnd =
                            checkedAdd(c["start"].get<std::int64_t>(), c["duration"].get<std::int64_t>());
                        end = std::max(end, clipEnd.value_or(std::numeric_limits<std::int64_t>::max()));
                    }
                    json parameters = json::object();
                    if (c.contains("properties") && c["properties"].is_object()) {
                        const json& p = c["properties"];
                        for (const ScalarRename& r : kScalars) {
                            if (p.contains(r.from) && p[r.from].is_object() && p[r.from].contains("value")) {
                                parameters[r.to] = json{{"value", p[r.from]["value"]}};
                            }
                        }
                        for (const VectorRename& r : kVectors) {
                            if (p.contains(r.from) && p[r.from].is_object() && p[r.from].contains("value") &&
                                p[r.from]["value"].is_array() && p[r.from]["value"].size() == 2) {
                                parameters[r.toX] = json{{"value", p[r.from]["value"][0]}};
                                parameters[r.toY] = json{{"value", p[r.from]["value"][1]}};
                            }
                        }
                    }
                    c["parameters"] = std::move(parameters);
                    c.erase("properties");
                }
            }
        }
        s["duration"] = end;
        s["workArea"] = nullptr;
    }
}

/// Schema 3 only adds optional fields (curves, effect stacks, reversed/maintainPitch, freeze frames, transition
/// params): every schema 2 file is already a valid schema 3 file, so there is nothing to rewrite.
void migrate2To3(json&) {}

/// Schema 4 only adds sequence caption tracks.
void migrate3To4(json&) {}

/// Schema 5 only adds media "generator" (synthetic items such as titles).
void migrate4To5(json&) {}

/// Schema 6 only adds the sequence beat grid.
void migrate5To6(json&) {}

/// Schema 7 only adds optional fields (track "role", sequence "audioEffects"): absent means none.
void migrate6To7(json&) {}
/// Schema 8 stores each transition's alignment (derived here from its sides and the sequence's frame) and adds
/// optional clip time remaps.
/// Schema 9 only adds optional fields (caption styles, cue speakers, speakers, transcript edits, caption presets):
/// a caption track without a style keeps the look captions had before.
void migrate8To9(json&) {}
/// Schema 10 only adds optional fields (clip "source" for nested and multicam clips, sequence "multicam", media
/// "startTimecode"): older clips keep their "media".
void migrate9To10(json&) {}
/// Schema 11 only adds the optional project "reviewNotes" (C.9).
void migrate10To11(json&) {}
/// Schema 12 only adds matte masks (AI-14: mask shape "matte" with its "matte" track). An older build refuses a schema
/// 12 file as "saved by a newer version" instead of failing on the unknown shape.
void migrate11To12(json&) {}

/// Schema 13 only adds graphics (C.13): the optional clip "graphic", project "brandKit" and "templates", and caption
/// style "brand" references. Every schema 12 file is a valid schema 13 file.
void migrate12To13(json&) {}

/// Schema 14 only adds the sequence "startTimecode" (X.1), written only when set. Every schema 13 file is a valid schema
/// 14 file.
void migrate13To14(json&) {}

/// Schema 15 only adds C.15's optional clip "blendMode" and generator "params" (and new generator kinds).
void migrate14To15(json&) {}

/// Schema 16 only adds the media audio stream "title" (A.10: the stream's name in the file, for multi-track ingest),
/// written only when set. Every schema 15 file is a valid schema 16 file: its streams read as untitled.
void migrate15To16(json&) {}

void migrate7To8(json& doc) {
    if (!doc.contains("sequences") || !doc["sequences"].is_array()) {
        return;
    }
    const auto ticks = [](const json& o, const char* key) {
        return Ticks{o.contains(key) && o[key].is_number_integer() ? o[key].get<std::int64_t>() : 0};
    };
    for (json& s : doc["sequences"]) {
        if (!s.is_object()) {
            continue;
        }
        // One frame at the sequence rate, from an untrusted file: integers in range, checked arithmetic (the reader
        // rejects a bad rate afterwards; the alignment then compares the sides to one tick).
        Ticks frame{1};
        if (s.contains("frameRate") && s["frameRate"].is_object()) {
            const json& r = s["frameRate"];
            const auto part = [&](const char* key) -> std::int64_t {
                return r.contains(key) && r[key].is_number_integer() ? r[key].get<std::int64_t>() : 0;
            };
            const std::int64_t num = part("num");
            const std::int64_t den = part("den");
            constexpr std::int64_t kMax = std::numeric_limits<std::int32_t>::max();
            if (num > 0 && den > 0 && num <= kMax && den <= kMax) {
                const auto one = framesToTicks(1, Rate{static_cast<std::int32_t>(num), static_cast<std::int32_t>(den)},
                                                 Rounding::Floor);
                if (one && one->value > 0) {
                    frame = *one;
                }
            }
        }
        for (const char* list : {"videoTracks", "audioTracks"}) {
            if (!s.contains(list) || !s[list].is_array()) {
                continue;
            }
            for (json& t : s[list]) {
                if (!t.is_object() || !t.contains("transitions") || !t["transitions"].is_array()) {
                    continue;
                }
                for (json& tr : t["transitions"]) {
                    if (tr.is_object() && !tr.contains("alignment")) {
                        tr["alignment"] = std::string(toString(alignmentOf(ticks(tr, "before"), ticks(tr, "after"), frame)));
                    }
                }
            }
        }
    }
}

using Migration = void (*)(json&);
/// kMigrations[v - kOldestSchemaVersion] upgrades schema v to v + 1.
constexpr std::array<Migration, kSchemaVersion - kOldestSchemaVersion> kMigrations{&migrate1To2, &migrate2To3,
                                                                                    &migrate3To4, &migrate4To5,
                                                                                    &migrate5To6, &migrate6To7,
                                                                                    &migrate7To8, &migrate8To9,
                                                                                    &migrate9To10, &migrate10To11,
                                                                                    &migrate11To12, &migrate12To13,
                                                                                    &migrate13To14, &migrate14To15,
                                                                                    &migrate15To16};

// --- Reading (current schema) -------------------------------------------------------------------------


ColorInfo readColor(const Reader& r) {
    ColorInfo c;
    c.matrix = r.enumeration("matrix", kColorMatrices);
    c.range = r.enumeration("range", kColorRanges);
    c.primaries = r.enumeration("primaries", kColorPrimaries);
    c.transfer = r.enumeration("transfer", kTransfers);
    c.chromaLocation = r.enumeration("chromaLocation", kChromaLocations);
    return c;
}

MediaInfo readMediaInfo(const Reader& r) {
    MediaInfo info;
    info.path = r.string("path");
    info.container = r.string("container");
    info.duration = r.nonNegativeTicks("duration", Ticks{});
    info.fileSize = r.int64In("fileSize", 0, 0, std::numeric_limits<std::int64_t>::max());
    info.modifiedTime = r.int64("modifiedTime", 0);
    info.isStillImage = r.boolean("isStillImage", false);
    if (r.has("startTimecode")) {
        info.startTimecode = r.nonNegativeTicks("startTimecode", Ticks{});
    }
    if (r.has("video")) {
        const Reader list = r.child("video");
        for (std::size_t i = 0; i < list.arraySize(); ++i) {
            const Reader v = list.element(i);
            VideoStreamInfo s;
            s.index = v.int32In("index", -1, -1, std::numeric_limits<std::int32_t>::max());
            s.codec = v.string("codec");
            s.profile = v.string("profile");
            s.width = v.int32In("width", 0, 0, std::numeric_limits<std::int32_t>::max());
            s.height = v.int32In("height", 0, 0, std::numeric_limits<std::int32_t>::max());
            s.frameRate = v.probedRate("frameRate", Rate{});
            s.variableFrameRate = v.boolean("variableFrameRate", false);
            s.sampleAspect = v.probedRate("sampleAspect", Rate{1, 1});
            s.pixelFormat = v.string("pixelFormat");
            s.bitDepth = v.int32In("bitDepth", 8, 1, 64);
            s.chromaSubsampling = v.string("chromaSubsampling");
            if (v.has("color")) {
                s.color = readColor(v.child("color"));
            }
            s.alpha = v.enumeration("alpha", kAlphaModes);
            s.rotationDegrees = v.int32("rotationDegrees", 0);
            if (s.rotationDegrees % 90 != 0 || s.rotationDegrees < 0 || s.rotationDegrees >= 360) {
                v.child("rotationDegrees").fail("rotation must be 0, 90, 180 or 270");
            }
            s.duration = v.nonNegativeTicks("duration", Ticks{});
            s.frameCount = v.int64In("frameCount", -1, -1, std::numeric_limits<std::int64_t>::max());
            info.video.push_back(std::move(s));
        }
    }
    if (r.has("audio")) {
        const Reader list = r.child("audio");
        for (std::size_t i = 0; i < list.arraySize(); ++i) {
            const Reader a = list.element(i);
            AudioStreamInfo s;
            s.index = a.int32In("index", -1, -1, std::numeric_limits<std::int32_t>::max());
            s.codec = a.string("codec");
            s.sampleRate = a.probedRate("sampleRate", kRate48kHz);
            s.channels = a.int32In("channels", 0, 0, 1024);
            s.channelLayout = a.string("channelLayout");
            s.duration = a.nonNegativeTicks("duration", Ticks{});
            s.title = a.string("title"); // A.10 (schema 15); absent: untitled
            info.audio.push_back(std::move(s));
        }
    }
    return info;
}

SourceFingerprint readFingerprint(const Reader& r) {
    SourceFingerprint f;
    f.size = r.int64In("size", -1, -1, std::numeric_limits<std::int64_t>::max());
    f.mtimeNs = r.int64("mtimeNs", 0);
    f.sampleHash = r.string("sampleHash");
    const bool hex = std::all_of(f.sampleHash.begin(), f.sampleHash.end(),
                                 [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    if (!f.sampleHash.empty() && (f.sampleHash.size() != 16 || !hex)) {
        r.child("sampleHash").fail("expected 16 lowercase hex digits");
    }
    return f;
}

MediaLocator readLocator(const Reader& r) {
    MediaLocator l;
    l.path = r.string("path");
    l.relativePath = r.string("relativePath");
    if (r.has("fingerprint")) {
        l.fingerprint = readFingerprint(r.child("fingerprint"));
    }
    l.offline = r.boolean("offline", false);
    if (r.has("proxy")) {
        const Reader p = r.child("proxy");
        ProxyMedia proxy;
        proxy.path = p.child("path").asString();
        if (proxy.path.empty()) {
            p.child("path").fail("a proxy needs a path");
        }
        if (p.has("provenance")) {
            const Reader prov = p.child("provenance");
            proxy.provenance.generator = prov.string("generator");
            proxy.provenance.settings = prov.string("settings");
            if (prov.has("source")) {
                proxy.provenance.source = readFingerprint(prov.child("source"));
            }
            proxy.provenance.generation = prov.uint64("generation", 0);
        }
        l.proxy = std::move(proxy);
    }
    return l;
}

BezierHandle readHandle(const Reader& r) {
    BezierHandle h;
    h.slope = r.has("slope") ? r.child("slope").asDouble() : h.slope;
    h.influence = r.has("influence") ? r.child("influence").asDouble() : h.influence;
    return h;
}

ParamValue readParamValue(const Reader& r) {
    if (!r.has("keys")) {
        return ParamValue(r.child("value").asDouble());
    }
    const Reader list = r.child("keys");
    Curve curve;
    for (std::size_t i = 0; i < list.arraySize(); ++i) {
        const Reader k = list.element(i);
        Keyframe key;
        key.time = k.ticks("t");
        key.value = k.child("v").asDouble();
        if (k.has("i")) {
            const auto interpolation = interpolationFromString(k.child("i").asString());
            if (!interpolation) {
                k.child("i").fail("unknown interpolation");
            }
            key.interpolation = *interpolation;
        }
        if (k.has("in")) {
            key.in = readHandle(k.child("in"));
        }
        if (k.has("out")) {
            key.out = readHandle(k.child("out"));
        }
        curve.keys.push_back(key);
    }
    if (!curve.isWellFormed()) {
        list.fail("a curve needs keys at increasing times with finite handles");
    }
    return ParamValue(std::move(curve));
}

ClipProperties readParameters(const Reader& r) {
    ClipProperties props;
    if (!r.value().is_object()) {
        r.fail("expected an object of parameters");
    }
    for (const auto& item : r.value().items()) {
        const std::string& key = item.key();
        const auto id = parameterByKey(key);
        if (!id) {
            r.fail("unknown parameter \"" + key + "\"");
        }
        props[*id] = readParamValue(r.child(key.c_str()));
    }
    return props;
}

std::vector<EffectParam> readEffectParams(const Reader& r) {
    if (!r.value().is_object()) {
        r.fail("expected an object of parameters");
    }
    std::vector<EffectParam> out; // JSON objects iterate in key order, so this is sorted
    for (const auto& item : r.value().items()) {
        out.push_back(EffectParam{item.key(), readParamValue(r.child(item.key().c_str()))});
    }
    return out;
}

/// Effect data, bounded (project files are untrusted): kMaxEffectDataEntries entries, kMaxEffectPoints finite points
/// strictly increasing in x, kMaxEffectText bytes of text. Anything else fails the load with its path.
std::vector<EffectData> readEffectData(const Reader& r) {
    if (!r.value().is_object()) {
        r.fail("expected an object");
    }
    if (r.value().size() > kMaxEffectDataEntries) {
        r.fail("more than " + std::to_string(kMaxEffectDataEntries) + " data entries");
    }
    std::vector<EffectData> out; // JSON objects iterate in key order, so this is sorted
    for (const auto& item : r.value().items()) {
        const Reader entry = r.child(item.key().c_str());
        EffectData d;
        d.key = item.key();
        if (entry.has("points")) {
            const Reader list = entry.child("points");
            if (list.arraySize() > kMaxEffectPoints) {
                list.fail("more than " + std::to_string(kMaxEffectPoints) + " points");
            }
            std::vector<EffectPoint> points;
            for (std::size_t i = 0; i < list.arraySize(); ++i) {
                const Reader p = list.element(i);
                if (p.arraySize() != 2) {
                    p.fail("a point is [x, y]");
                }
                points.push_back({p.element(0).asDouble(), p.element(1).asDouble()});
            }
            d.value = std::move(points);
        } else {
            d.value = entry.child("text").asString();
        }
        if (const auto problem = effectDataProblem(d)) {
            entry.fail(*problem);
        }
        out.push_back(std::move(d));
    }
    return out;
}

/// A mask, bounded (project files are untrusted): kMaxMaskPathKeys keys of at most kMaxMaskVertices finite vertices;
/// unknown shapes, modes and interpolations fail the load; then maskProblem().
Mask readMask(const Reader& r) {
    Mask m;
    m.id = static_cast<std::uint32_t>(r.int64In("id", 0, 0, std::numeric_limits<std::uint32_t>::max()));
    m.name = r.string("name");
    const auto shape = maskShapeFromString(r.string("shape", "ellipse"));
    if (!shape) {
        r.child("shape").fail("unknown mask shape");
    }
    m.shape = *shape;
    const auto mode = maskModeFromString(r.string("mode", "add"));
    if (!mode) {
        r.child("mode").fail("unknown mask mode");
    }
    m.mode = *mode;
    m.inverted = r.boolean("inverted", false);
    m.enabled = r.boolean("enabled", true);
    if (r.has("params")) {
        const Reader params = r.child("params");
        if (!params.value().is_object()) {
            params.fail("expected an object of parameters");
        }
        for (const auto& item : params.value().items()) { // JSON objects iterate in key order: sorted
            m.params.push_back(MaskParam{item.key(), readParamValue(params.child(item.key().c_str()))});
        }
    }
    if (r.has("path")) {
        const Reader keys = r.child("path");
        if (keys.arraySize() > kMaxMaskPathKeys) {
            keys.fail("more than " + std::to_string(kMaxMaskPathKeys) + " path keys");
        }
        for (std::size_t i = 0; i < keys.arraySize(); ++i) {
            const Reader k = keys.element(i);
            MaskPathKey key;
            key.time = k.ticks("t");
            const auto interpolation = interpolationFromString(k.string("i", "linear"));
            if (!interpolation) {
                k.child("i").fail("unknown interpolation");
            }
            key.interpolation = *interpolation;
            const Reader vertices = k.child("v");
            if (vertices.arraySize() > kMaxMaskVertices) {
                vertices.fail("more than " + std::to_string(kMaxMaskVertices) + " vertices");
            }
            for (std::size_t j = 0; j < vertices.arraySize(); ++j) {
                const Reader v = vertices.element(j);
                if (v.arraySize() != 6) {
                    v.fail("a vertex is [x, y, inX, inY, outX, outY]");
                }
                key.vertices.push_back({v.element(0).asDouble(), v.element(1).asDouble(), v.element(2).asDouble(),
                                        v.element(3).asDouble(), v.element(4).asDouble(), v.element(5).asDouble()});
            }
            m.path.push_back(std::move(key));
        }
    }
    if (r.has("matte")) {
        const Reader matte = r.child("matte");
        MaskMatte mm;
        mm.track = matte.string("track");
        mm.media = MediaId{matte.uint64("media")};
        mm.stream = static_cast<int>(matte.int64In("stream", 0, 0, 64));
        const Reader source = matte.child("source");
        mm.source = TimeRange{source.ticks("start"), source.ticks("duration")};
        mm.model = matte.string("model");
        m.matte = std::move(mm);
    }
    if (const auto problem = maskProblem(m)) {
        r.fail(*problem);
    }
    return m;
}

std::vector<Mask> readMasks(const Reader& r) {
    if (r.arraySize() > kMaxMasks) {
        r.fail("more than " + std::to_string(kMaxMasks) + " masks");
    }
    std::vector<Mask> out;
    for (std::size_t i = 0; i < r.arraySize(); ++i) {
        out.push_back(readMask(r.element(i)));
    }
    if (const auto problem = masksProblem(out)) {
        r.fail(*problem);
    }
    return out;
}

Redaction readRedaction(const Reader& r) {
    Redaction red;
    red.id = RedactionId{r.uint64("id")};
    red.media = MediaId{r.uint64("media")};
    red.name = r.string("name");
    red.enabled = r.boolean("enabled", true);
    const Reader source = r.child("source");
    red.source = TimeRange{source.ticks("start"), source.ticks("duration")};
    red.region = readMask(r.child("region"));
    const auto style = redactionStyleFromString(r.string("style", "blur"));
    if (!style) {
        r.child("style").fail("unknown redaction style");
    }
    red.style = *style;
    if (r.has("strength")) {
        red.strength = r.child("strength").asDouble();
    }
    if (r.has("color")) {
        const Reader c = r.child("color");
        if (c.arraySize() != 3) {
            c.fail("a colour is [r, g, b]");
        }
        red.colorR = c.element(0).asDouble();
        red.colorG = c.element(1).asDouble();
        red.colorB = c.element(2).asDouble();
    }
    const auto audio = redactionAudioFromString(r.string("audio", "none"));
    if (!audio) {
        r.child("audio").fail("unknown redaction audio treatment");
    }
    red.audio = *audio;
    if (r.has("audioRanges")) {
        const Reader ranges = r.child("audioRanges");
        if (ranges.arraySize() > kMaxRedactionRanges) {
            ranges.fail("more than " + std::to_string(kMaxRedactionRanges) + " audio ranges");
        }
        for (std::size_t i = 0; i < ranges.arraySize(); ++i) {
            const Reader t = ranges.element(i);
            if (t.arraySize() != 2) {
                t.fail("an audio range is [start, duration]");
            }
            red.audioRanges.push_back(TimeRange{Ticks{t.element(0).asInt64()}, Ticks{t.element(1).asInt64()}});
        }
    }
    if (r.has("exceptions")) {
        const Reader list = r.child("exceptions");
        if (list.arraySize() > kMaxRedactionRanges) {
            list.fail("more than " + std::to_string(kMaxRedactionRanges) + " exceptions");
        }
        for (std::size_t i = 0; i < list.arraySize(); ++i) {
            red.exceptions.push_back(ClipId{list.element(i).asUInt64()});
        }
    }
    if (const auto problem = redactionProblem(red)) {
        r.fail(*problem);
    }
    return red;
}

EffectStack readEffectStack(const Reader& r) {
    EffectStack stack;
    for (std::size_t i = 0; i < r.arraySize(); ++i) {
        const Reader e = r.element(i);
        EffectInstance instance;
        instance.id = EffectInstanceId{e.uint64("id")};
        instance.effect = e.child("effect").asString();
        instance.version = e.int32("version", 1);
        instance.enabled = e.boolean("enabled", true);
        if (e.has("params")) {
            instance.params = readEffectParams(e.child("params"));
        }
        instance.mask = e.string("mask");
        if (e.has("data")) {
            instance.data = readEffectData(e.child("data"));
        }
        if (e.has("masks")) {
            instance.masks = readMasks(e.child("masks"));
        }
        stack.push_back(std::move(instance));
    }
    return stack;
}

AudioChannelMap readChannelMap(const Reader& r); // A.9, below

Clip readClip(const Reader& r, const ProjectState& state) {
    Clip c;
    c.id = ClipId{r.uint64("id")};
    std::string unknownSource;
    if (r.has("source")) {
        // C.8: a typed source instead of "media". A kind this build does not know is kept as it is.
        const Reader source = r.child("source");
        if (!source.value().is_object()) {
            source.fail("expected an object");
        }
        const std::string type = source.string("type");
        if (type == "sequence") {
            c.sequence = SequenceId{source.uint64("sequence")};
            if (c.sequence.value == 0) {
                source.child("sequence").fail("expected a sequence id");
            }
            if (source.has("angles")) {
                const Reader angles = source.child("angles");
                for (std::size_t i = 0; i < angles.arraySize(); ++i) {
                    const std::int64_t angle = angles.element(i).asInt64();
                    if (angle < 0 || angle >= static_cast<std::int64_t>(kMaxMulticamAngles) ||
                        c.angles.contains(static_cast<std::size_t>(angle))) {
                        angles.element(i).fail("expected a distinct angle from 0 to " +
                                               std::to_string(kMaxMulticamAngles - 1));
                    }
                    c.angles.bits |= AngleSet::of(static_cast<std::size_t>(angle)).bits;
                }
            }
        } else {
            unknownSource = source.value().dump(-1, ' ', false, json::error_handler_t::replace);
            if (type.empty() || unknownSource.size() > kMaxUnknownSourceBytes) {
                source.fail(type.empty() ? "a source needs a type" : "an unknown source is too large to keep");
            }
        }
    } else {
        c.media = MediaId{r.uint64("media")};
    }
    const Reader stream = r.child("stream");
    const std::string kind = stream.string("kind");
    if (kind == "video") {
        c.stream.kind = StreamKind::Video;
    } else if (kind == "audio") {
        c.stream.kind = StreamKind::Audio;
    } else {
        stream.fail("stream kind must be \"video\" or \"audio\"");
    }
    c.stream.index = stream.int32In("index", 0, 0, std::numeric_limits<std::int32_t>::max());
    c.timelineRange = {r.ticks("start"), r.ticks("duration")};
    c.sourceIn = r.ticks("sourceIn");
    if (r.has("speed")) {
        const Reader speed = r.child("speed");
        c.speed = {speed.int32("num"), speed.int32("den")};
    }
    c.enabled = r.boolean("enabled", true);
    c.linkGroup = LinkGroupId{r.uint64("linkGroup", 0)};
    c.colorIndex = r.int32("colorIndex", 0);
    c.reversed = r.boolean("reversed", false);
    c.maintainPitch = r.boolean("maintainPitch", false);
    ClipContent content;
    content.name = r.string("name");
    if (r.has("parameters")) {
        content.properties = readParameters(r.child("parameters"));
    }
    if (r.has("effects")) {
        content.effects = readEffectStack(r.child("effects"));
    }
    if (r.has("timeRemap")) {
        const Reader remap = r.child("timeRemap");
        ParamValue value = readParamValue(remap);
        if (!value.isCurve()) {
            remap.fail("a time remap needs keys");
        }
        if (const auto problem = remapProblem(*value.curve())) {
            remap.fail("the time remap " + *problem);
        }
        content.timeRemap = *value.curve();
    }
    if (r.has("channels")) { // A.9
        content.channels = readChannelMap(r.child("channels"));
    }
    if (r.has("graphic")) {
        content.graphic = std::make_shared<const Graphic>(detail::graphicFromJsonValue(r.child("graphic")));
    }
    if (r.has("blendMode")) { // C.15
        const std::string name = r.string("blendMode");
        const auto mode = blendModeFromString(name);
        if (!mode) {
            r.fail("unknown blend mode '" + name.substr(0, 64) + "'");
        }
        content.blendMode = *mode;
    }
    content.unknownSource = std::move(unknownSource);
    // Clips that still carry their media's defaults share its block (cheap copies, less memory).
    const MediaItem* media = state.findMedia(c.media);
    if (media && media->clipDefaults && content == *media->clipDefaults) {
        c.setContent(media->clipDefaults);
    } else if (!(content == ClipContent{})) {
        c.setContent(std::move(content));
    }
    return c;
}

/// A.9: a channel map, bounded (kMaxClipChannels channels, kMaxRouteSources sources each) and validated.
AudioChannelMap readChannelMap(const Reader& r) {
    AudioChannelMap map;
    const auto format = channelFormatFromString(r.child("format").asString());
    if (!format) {
        r.child("format").fail("unknown channel format");
    }
    map.format = *format;
    const Reader list = r.child("channels");
    const std::size_t n = list.arraySize();
    if (n > static_cast<std::size_t>(kMaxClipChannels)) {
        list.fail("more than " + std::to_string(kMaxClipChannels) + " channels");
    }
    for (std::size_t i = 0; i < n; ++i) {
        const Reader c = list.element(i);
        ChannelRoute route;
        if (c.has("sources")) {
            const Reader sources = c.child("sources");
            const std::size_t k = sources.arraySize();
            if (k > static_cast<std::size_t>(kMaxRouteSources)) {
                sources.fail("more than " + std::to_string(kMaxRouteSources) + " source channels");
            }
            for (std::size_t j = 0; j < k; ++j) {
                route.sources.push_back(sources.element(j).asInt32());
            }
        }
        route.muted = c.boolean("muted", false);
        if (c.has("gainDb")) {
            route.gainDb = readParamValue(c.child("gainDb"));
        }
        map.channels.push_back(std::move(route));
    }
    if (const auto problem = channelMapProblem(map)) {
        r.fail(*problem);
    }
    return map;
}

ReviewNote readReviewNote(const Reader& r) {
    ReviewNote n;
    n.id = ReviewNoteId{r.uint64("id")};
    n.sequence = SequenceId{r.uint64("sequence")};
    n.start = r.nonNegativeTicks("start", Ticks{});
    n.duration = r.nonNegativeTicks("duration", Ticks{});
    n.author = r.string("author", "");
    n.text = r.string("text", "");
    n.createdAt = r.string("createdAt", "");
    const auto status = reviewStatusFromString(r.string("status", "open"));
    if (!status) {
        r.child("status").fail("unknown review status");
    }
    n.status = *status;
    if (r.has("mark")) {
        const Reader m = r.child("mark");
        const auto number = [&](const char* key) { return m.has(key) ? m.child(key).asDouble() : 0.0; };
        n.mark = ReviewMark{m.child("x").asDouble(), m.child("y").asDouble(), number("width"), number("height")};
    }
    if (r.has("replies")) {
        const Reader replies = r.child("replies");
        if (replies.arraySize() > kMaxReviewReplies) {
            replies.fail("more than " + std::to_string(kMaxReviewReplies) + " replies");
        }
        for (std::size_t i = 0; i < replies.arraySize(); ++i) {
            const Reader e = replies.element(i);
            n.replies.push_back(ReviewReply{e.string("author", ""), e.string("text", ""), e.string("createdAt", "")});
        }
    }
    if (const auto problem = reviewNoteProblem(n)) {
        r.fail(*problem);
    }
    return n;
}

Marker readMarker(const Reader& m) {
    Marker marker{m.ticks("start"), m.nonNegativeTicks("duration", Ticks{}), m.string("name"), m.string("comment"),
                  m.int32("colorIndex", 0)};
    if (m.has("type")) {
        const auto type = markerTypeFromName(m.string("type"));
        if (!type) {
            m.child("type").fail("unknown marker type");
        }
        marker.type = *type;
    }
    marker.url = m.string("url", "");
    marker.target = m.string("target", "");
    if (m.has("highlight")) { // C.15
        const Reader h = m.child("highlight");
        MarkerHighlight highlight;
        highlight.score = h.child("score").asDouble();
        const auto confidence = highlightConfidenceFromString(h.string("confidence"));
        if (!confidence) {
            h.child("confidence").fail("unknown highlight confidence");
        }
        highlight.confidence = *confidence;
        if (h.has("reasons")) {
            const Reader reasons = h.child("reasons");
            if (reasons.arraySize() > MarkerHighlight::kMaxReasons) {
                reasons.fail("more than " + std::to_string(MarkerHighlight::kMaxReasons) + " reasons");
            }
            for (std::size_t i = 0; i < reasons.arraySize(); ++i) {
                highlight.reasons.push_back(reasons.element(i).asString());
            }
        }
        highlight.peak = h.ticks("peak");
        highlight.source = h.string("source");
        marker.highlight = std::move(highlight);
        if (const auto problem = markerProblem(marker)) {
            h.fail(*problem);
        }
    }
    return marker;
}

std::vector<Marker> readMarkers(const Reader& markers) {
    std::vector<Marker> out;
    const std::size_t count = markers.arraySize();
    if (count > kMaxMarkers) {
        markers.fail("more than " + std::to_string(kMaxMarkers) + " markers");
    }
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(readMarker(markers.element(i)));
    }
    return out;
}

TrackPtr readTrack(const Reader& r, TrackKind kind, const ProjectState& state) {
    auto t = std::make_shared<Track>();
    t->id = TrackId{r.uint64("id")};
    t->kind = kind;
    t->name = r.string("name");
    t->enabled = r.boolean("enabled", true);
    t->locked = r.boolean("locked", false);
    t->syncLocked = r.boolean("syncLocked", true);
    t->muted = r.boolean("muted", false);
    t->solo = r.boolean("solo", false);
    t->recordArmed = r.boolean("recordArmed", false); // A.8
    if (r.has("format")) { // A.9
        const auto format = channelFormatFromString(r.child("format").asString());
        if (!format) {
            r.child("format").fail("unknown channel format");
        }
        t->format = *format;
    }
    if (r.has("surround")) { // A.9
        const Reader s = r.child("surround");
        if (s.has("x")) {
            t->surround.x = readParamValue(s.child("x"));
        }
        if (s.has("y")) {
            t->surround.y = readParamValue(s.child("y"));
        }
        if (s.has("lfeDb")) {
            t->surround.lfeDb = readParamValue(s.child("lfeDb"));
        }
    }
    t->targeted = r.boolean("targeted", false);
    if (r.has("role")) {
        const auto role = trackRoleFromString(r.child("role").asString());
        if (!role) {
            r.child("role").fail("unknown track role");
        }
        t->role = *role;
    }
    if (r.has("clips")) {
        const Reader clips = r.child("clips");
        const std::size_t n = clips.arraySize();
        for (std::size_t i = 0; i < n; ++i) {
            t->clips.push_back(readClip(clips.element(i), state));
        }
    }
    if (r.has("transitions")) {
        const Reader list = r.child("transitions");
        for (std::size_t i = 0; i < list.arraySize(); ++i) {
            const Reader tr = list.element(i);
            Transition transition;
            transition.id = TransitionId{tr.uint64("id")};
            transition.outgoing = ClipId{tr.uint64("outgoing", 0)};
            transition.incoming = ClipId{tr.uint64("incoming", 0)};
            transition.before = tr.nonNegativeTicks("before", Ticks{});
            transition.after = tr.nonNegativeTicks("after", Ticks{});
            const auto alignment = transitionAlignmentFromString(tr.string("alignment"));
            if (!alignment) {
                tr.fail("a transition needs an alignment: \"center\", \"startAtCut\", \"endAtCut\" or \"custom\"");
            }
            transition.alignment = *alignment;
            transition.effect = tr.string("effect", "crossDissolve");
            if (tr.has("params")) {
                transition.params = readEffectParams(tr.child("params"));
            }
            t->transitions.push_back(std::move(transition));
        }
    }
    if (r.has("effects")) {
        t->effects = readEffectStack(r.child("effects"));
    }
    if (r.has("volume")) { // A.3; absent = 0 dB
        t->volume = readParamValue(r.child("volume"));
    }
    if (r.has("pan")) { // absent = centre
        t->pan = readParamValue(r.child("pan"));
    }
    return t;
}

CaptionRgb readRgb(const Reader& r) {
    if (r.arraySize() != 3) {
        r.fail("a colour is [red, green, blue]");
    }
    return {r.element(0).asDouble(), r.element(1).asDouble(), r.element(2).asDouble()};
}

/// A caption style; every field optional (the default look), the result checked (captionStyleProblem).
CaptionStyle readCaptionStyle(const Reader& r) {
    CaptionStyle s;
    const auto number = [&](const char* key, double& out) {
        if (r.has(key)) {
            out = r.child(key).asDouble();
        }
    };
    const auto rgb = [&](const char* key, CaptionRgb& out) {
        if (r.has(key)) {
            out = readRgb(r.child(key));
        }
    };
    const auto named = [&](const char* key, auto parse, auto& out) {
        if (r.has(key)) {
            const auto value = parse(r.child(key).asString());
            if (!value) {
                r.child(key).fail("unknown value");
            }
            out = *value;
        }
    };
    s.fontFamily = r.string("fontFamily");
    s.fontWeight = r.int32("fontWeight", s.fontWeight);
    number("fontSize", s.fontSize);
    rgb("fill", s.fill);
    number("outlineWidth", s.outlineWidth);
    rgb("outline", s.outline);
    number("shadowOpacity", s.shadowOpacity);
    rgb("shadowColor", s.shadowColor);
    number("shadowDistance", s.shadowDistance);
    number("shadowAngle", s.shadowAngle);
    number("shadowSoftness", s.shadowSoftness);
    s.box = r.boolean("box", s.box);
    rgb("boxColor", s.boxColor);
    number("boxOpacity", s.boxOpacity);
    number("boxPadding", s.boxPadding);
    number("boxRadius", s.boxRadius);
    named("placement", [](const std::string& v) { return captionPlacementFromString(v); }, s.placement);
    number("safeArea", s.safeArea);
    if (r.has("positionX")) {
        s.positionX = readParamValue(r.child("positionX"));
    }
    if (r.has("positionY")) {
        s.positionY = readParamValue(r.child("positionY"));
    }
    named("alignment", [](const std::string& v) { return captionAlignFromString(v); }, s.alignment);
    number("maxWidth", s.maxWidth);
    number("lineSpacing", s.lineSpacing);
    rgb("highlight", s.highlight);
    named("animation", [](const std::string& v) { return captionAnimationFromString(v); }, s.animation);
    s.showSpeakers = r.boolean("showSpeakers", s.showSpeakers);
    s.allowOverlap = r.boolean("allowOverlap", s.allowOverlap);
    if (r.has("brand")) {
        const Reader b = r.child("brand");
        s.brand.fill = b.string("fill");
        s.brand.outline = b.string("outline");
        s.brand.shadow = b.string("shadow");
        s.brand.box = b.string("box");
        s.brand.highlight = b.string("highlight");
        s.brand.font = b.string("font");
    }
    if (const auto problem = captionStyleProblem(s)) {
        r.fail("the caption style " + *problem);
    }
    return s;
}

SequencePtr readSequence(const Reader& r, const ProjectState& state) {
    auto s = std::make_shared<Sequence>();
    s->id = SequenceId{r.uint64("id")};
    s->bin = BinId{r.uint64("bin", 0)};
    s->name = r.string("name");
    s->frameRate = r.rate("frameRate");
    s->width = r.int32("width");
    s->height = r.int32("height");
    s->audioRate = r.has("audioRate") ? r.rate("audioRate") : kRate48kHz;
    s->dropFrame = r.boolean("dropFrame", false);
    s->startTimecode = r.nonNegativeTicks("startTimecode", Ticks{}); // X.1; validate() bounds it
    s->duration = r.nonNegativeTicks("duration", Ticks{});
    if (r.has("workArea")) {
        const Reader w = r.child("workArea");
        s->workArea = TimeRange{w.ticks("start"), w.ticks("duration")};
    }
    s->markIn = r.optionalTicks("markIn");
    s->markOut = r.optionalTicks("markOut");
    if (r.has("effects")) {
        s->effects = readEffectStack(r.child("effects"));
    }
    if (r.has("audioEffects")) {
        s->audioEffects = readEffectStack(r.child("audioEffects"));
    }
    if (r.has("masterVolume")) { // A.3; absent = 0 dB
        s->masterVolume = readParamValue(r.child("masterVolume"));
    }
    if (r.has("masterFormat")) { // A.9; absent = stereo
        const auto format = channelFormatFromString(r.child("masterFormat").asString());
        if (!format) {
            r.child("masterFormat").fail("unknown channel format");
        }
        s->masterFormat = *format;
    }
    if (r.has("beatGrid")) {
        const Reader g = r.child("beatGrid");
        BeatGrid grid;
        grid.bpmMilli = g.int64("bpmMilli");
        grid.phase = g.ticks("phase");
        grid.beatsPerBar = g.int32("beatsPerBar", 4);
        if (g.has("beats")) {
            const Reader beats = g.child("beats");
            for (std::size_t i = 0; i < beats.arraySize(); ++i) {
                grid.beats.push_back(Ticks{beats.element(i).asInt64()});
            }
        }
        s->beatGrid = std::move(grid);
    }
    if (r.has("multicam")) {
        const Reader m = r.child("multicam");
        MulticamInfo multicam;
        const auto sync = multicamSyncFromString(m.string("sync"));
        if (!sync) {
            m.child("sync").fail("unknown multicam sync method");
        }
        multicam.sync = *sync;
        const Reader angles = m.child("angles");
        if (angles.arraySize() > kMaxMulticamAngles) {
            angles.fail("more than " + std::to_string(kMaxMulticamAngles) + " angles");
        }
        for (std::size_t i = 0; i < angles.arraySize(); ++i) {
            const Reader a = angles.element(i);
            MulticamAngle angle;
            angle.name = a.string("name");
            angle.video = TrackId{a.uint64("video", 0)};
            if (a.has("audio")) {
                const Reader audio = a.child("audio");
                for (std::size_t k = 0; k < audio.arraySize(); ++k) {
                    angle.audio.push_back(TrackId{audio.element(k).asUInt64()});
                }
            }
            angle.offset = a.nonNegativeTicks("offset", Ticks{});
            multicam.angles.push_back(std::move(angle));
        }
        s->multicam = std::move(multicam);
    }
    if (r.has("derivedFrom")) {
        const Reader d = r.child("derivedFrom");
        SequenceDerivation derivation;
        derivation.source = SequenceId{d.uint64("source")};
        derivation.generator = d.string("generator");
        if (derivation.generator.size() > SequenceDerivation::kMaxGeneratorBytes) {
            d.fail("generator is too long");
        }
        const Reader params = d.child("params");
        if (!params.value().is_object()) {
            params.fail("expected an object");
        }
        derivation.params = params.value().dump(-1, ' ', false, json::error_handler_t::replace);
        if (derivation.params.size() > SequenceDerivation::kMaxParamsBytes) {
            params.fail("too large");
        }
        s->derivedFrom = std::move(derivation);
    }
    if (r.has("captionTracks")) {
        const Reader list = r.child("captionTracks");
        for (std::size_t i = 0; i < list.arraySize(); ++i) {
            const Reader t = list.element(i);
            CaptionTrack track;
            track.id = CaptionTrackId{t.uint64("id")};
            track.name = t.string("name");
            track.language = t.string("language");
            track.enabled = t.boolean("enabled", true);
            if (t.has("cues")) {
                const Reader cues = t.child("cues");
                if (cues.arraySize() > kMaxCaptionCues) {
                    cues.fail("more than " + std::to_string(kMaxCaptionCues) + " cues");
                }
                for (std::size_t k = 0; k < cues.arraySize(); ++k) {
                    const Reader c = cues.element(k);
                    track.cues.push_back(CaptionCue{c.ticks("start"), c.ticks("duration"), c.string("text"),
                                                    SpeakerId{c.uint64("speaker", 0)}});
                }
            }
            if (t.has("style")) {
                track.style = readCaptionStyle(t.child("style"));
            }
            s->captionTracks.push_back(std::move(track));
        }
    }
    if (r.has("markers")) {
        s->markers = readMarkers(r.child("markers"));
    }
    for (const auto& [key, kind] :
         {std::pair{"videoTracks", TrackKind::Video}, std::pair{"audioTracks", TrackKind::Audio}}) {
        if (!r.has(key)) {
            continue;
        }
        const Reader tracks = r.child(key);
        auto& list = kind == TrackKind::Video ? s->videoTracks : s->audioTracks;
        for (std::size_t i = 0; i < tracks.arraySize(); ++i) {
            list.push_back(readTrack(tracks.element(i), kind, state));
        }
    }
    return s;
}

ProjectState readProject(const Reader& root) {
    ProjectState state;
    state.ids.next = root.uint64("nextId");
    state.aiNoCloud = root.boolean("aiNoCloud", false);
    if (root.has("bins")) {
        const Reader bins = root.child("bins");
        for (std::size_t i = 0; i < bins.arraySize(); ++i) {
            const Reader b = bins.element(i);
            state.bins.push_back(Bin{BinId{b.uint64("id")}, BinId{b.uint64("parent", 0)}, b.string("name")});
        }
    }
    if (root.has("media")) {
        const Reader media = root.child("media");
        for (std::size_t i = 0; i < media.arraySize(); ++i) {
            const Reader m = media.element(i);
            MediaItem item;
            item.id = MediaId{m.uint64("id")};
            item.bin = BinId{m.uint64("bin", 0)};
            item.name = m.string("name");
            if (m.has("locator")) {
                item.locator = readLocator(m.child("locator"));
            }
            if (m.has("info")) {
                item.info = readMediaInfo(m.child("info"));
            }
            if (m.has("generator")) {
                const Reader g = m.child("generator");
                item.generator = GeneratorSpec{g.string("kind"), g.string("text", ""), g.string("fontFamily", ""),
                                               g.string("fontStyle", ""), {}};
                if (g.has("params")) { // C.15
                    item.generator->params = readEffectParams(g.child("params"));
                }
            }
            item.markIn = m.optionalTicks("markIn");
            item.markOut = m.optionalTicks("markOut");
            if (m.has("markers")) {
                item.markers = readMarkers(m.child("markers"));
            }
            if (m.has("generated")) {
                const Reader g = m.child("generated");
                GeneratedProvenance provenance;
                provenance.provider = g.string("provider");
                provenance.model = g.string("model");
                provenance.promptHash = g.string("promptHash");
                provenance.createdAt = g.string("createdAt");
                provenance.kind = g.string("kind");
                provenance.operation = g.string("operation");
                provenance.source = MediaId{g.uint64("source", 0)}; // weak: may name media that is gone
                if (provenance.provider.size() > GeneratedProvenance::kMaxNameBytes ||
                    provenance.model.size() > GeneratedProvenance::kMaxNameBytes) {
                    g.fail("provider or model is too long");
                }
                item.generated = std::move(provenance);
            }
            // Library metadata (M5k), bounded (validate checks the rest).
            item.colorIndex = m.int32In("colorIndex", 0, 0, 15);
            item.comment = m.string("comment");
            item.description = m.string("description");
            if (item.comment.size() > MediaItem::kMaxTextBytes || item.description.size() > MediaItem::kMaxTextBytes) {
                m.fail("comment or description is too long");
            }
            if (m.has("posterFrame")) {
                item.posterFrame = m.nonNegativeTicks("posterFrame", Ticks{});
            }
            if (m.has("subclip")) {
                const Reader sub = m.child("subclip");
                SubclipSpec spec;
                spec.master = MediaId{sub.uint64("master", 0)}; // weak: may name media that is gone
                spec.range = {sub.nonNegativeTicks("start", Ticks{}), sub.nonNegativeTicks("duration", Ticks{})};
                spec.restricted = sub.boolean("restricted", true);
                // A range past the end of a shorter relinked file loads as it is (subclipRangeWarnings).
                item.subclip = spec;
            }
            if (m.has("audioTakes")) {
                const Reader takes = m.child("audioTakes");
                for (std::size_t t = 0; t < takes.arraySize(); ++t) {
                    const Reader r = takes.element(t);
                    AudioTake take;
                    take.stream = r.int32("stream");
                    take.kind = r.string("kind");
                    take.path = r.string("path");
                    take.model = r.string("model", "");
                    if (r.has("source")) {
                        take.source = readFingerprint(r.child("source"));
                    }
                    take.generation = r.uint64("generation", 0);
                    if (take.kind.size() > AudioTake::kMaxNameBytes || take.model.size() > AudioTake::kMaxNameBytes) {
                        r.fail("kind or model is too long");
                    }
                    item.audioTakes.push_back(std::move(take));
                }
            }
            if (m.has("derived")) {
                const Reader d = m.child("derived");
                DerivedProvenance derived;
                derived.source = MediaId{d.uint64("source", 0)}; // weak: may name media that is gone
                derived.operation = d.string("operation");
                derived.model = d.string("model", "");
                derived.part = d.string("part", "");
                derived.createdAt = d.string("createdAt", "");
                if (derived.operation.size() > DerivedProvenance::kMaxNameBytes ||
                    derived.model.size() > DerivedProvenance::kMaxNameBytes || derived.part.size() > DerivedProvenance::kMaxNameBytes) {
                    d.fail("operation, model or part is too long");
                }
                item.derived = std::move(derived);
            }
            if (m.has("channels")) { // A.9
                item.channels = readChannelMap(m.child("channels"));
            }
            if (m.has("recording")) {
                const Reader rec = m.child("recording");
                RecordingProvenance recording;
                recording.sequence = SequenceId{rec.uint64("sequence", 0)}; // weak: may name a sequence that is gone
                recording.track = TrackId{rec.uint64("track", 0)};
                recording.range = TimeRange{Ticks{rec.int64("start")}, Ticks{rec.int64("duration")}};
                recording.take = static_cast<std::int32_t>(rec.int64("take", 1));
                recording.device = rec.string("device", "");
                recording.latencyCompensation = Ticks{rec.int64("latencyCompensation", 0)};
                recording.createdAt = rec.string("createdAt", "");
                if (recording.device.size() > RecordingProvenance::kMaxDeviceBytes) {
                    rec.fail("the input device name is too long");
                }
                item.recording = std::move(recording);
            }
            item.clipDefaults = std::make_shared<const ClipContent>(ClipContent{item.name, {}});
            state.media.push_back(std::make_shared<const MediaItem>(std::move(item)));
        }
    }
    if (root.has("sequences")) {
        const Reader sequences = root.child("sequences");
        for (std::size_t i = 0; i < sequences.arraySize(); ++i) {
            state.sequences.push_back(readSequence(sequences.element(i), state));
        }
    }
    if (root.has("assets")) {
        const Reader assets = root.child("assets");
        for (std::size_t i = 0; i < assets.arraySize(); ++i) {
            const Reader a = assets.element(i);
            ProjectAsset asset;
            asset.id = AssetId{a.uint64("id")};
            asset.kind = a.string("kind");
            asset.name = a.string("name");
            if (a.has("locator")) {
                asset.locator = readLocator(a.child("locator"));
            }
            state.assets.push_back(std::move(asset));
        }
    }
    if (root.has("redactions")) {
        const Reader redactions = root.child("redactions");
        if (redactions.arraySize() > kMaxRedactions) {
            redactions.fail("more than " + std::to_string(kMaxRedactions) + " redactions");
        }
        for (std::size_t i = 0; i < redactions.arraySize(); ++i) {
            state.redactions.push_back(readRedaction(redactions.element(i)));
        }
    }
    if (root.has("speakers")) {
        const Reader list = root.child("speakers");
        if (list.arraySize() > kMaxSpeakers) {
            list.fail("more than " + std::to_string(kMaxSpeakers) + " speakers");
        }
        for (std::size_t i = 0; i < list.arraySize(); ++i) {
            const Reader s = list.element(i);
            const std::int64_t color = s.int64In("color", 0, 0, 0xFFFFFF);
            state.speakers.push_back(Speaker{SpeakerId{s.uint64("id")}, s.string("name"), static_cast<std::uint32_t>(color)});
        }
    }
    if (root.has("transcriptEdits")) {
        const Reader list = root.child("transcriptEdits");
        if (list.arraySize() > kMaxTranscriptEdits) {
            list.fail("more than " + std::to_string(kMaxTranscriptEdits) + " transcript edit records");
        }
        for (std::size_t i = 0; i < list.arraySize(); ++i) {
            const Reader r = list.element(i);
            TranscriptEdits e;
            e.media = MediaId{r.uint64("media")};
            e.audioStream = r.int32In("audioStream", 0, 0, std::numeric_limits<std::int32_t>::max());
            const auto bounded = [&](const char* key) {
                const Reader items = r.child(key);
                if (items.arraySize() > kMaxTranscriptEditEntries) {
                    items.fail("more than " + std::to_string(kMaxTranscriptEditEntries) + " entries");
                }
                return items;
            };
            if (r.has("labels")) {
                const Reader labels = bounded("labels");
                for (std::size_t k = 0; k < labels.arraySize(); ++k) {
                    const Reader pair = labels.element(k);
                    if (pair.arraySize() != 2) {
                        pair.fail("a speaker label is [label, speaker]");
                    }
                    e.labels.push_back(SpeakerLabel{pair.element(0).asInt32(), SpeakerId{pair.element(1).asUInt64()}});
                }
            }
            if (r.has("speakerSpans")) {
                const Reader spans = bounded("speakerSpans");
                for (std::size_t k = 0; k < spans.arraySize(); ++k) {
                    const Reader s = spans.element(k);
                    e.speakerSpans.push_back(
                        SpeakerSpan{TimeRange{s.ticks("start"), s.ticks("duration")}, SpeakerId{s.uint64("speaker")}});
                }
            }
            if (r.has("corrections")) {
                const Reader corrections = bounded("corrections");
                for (std::size_t k = 0; k < corrections.arraySize(); ++k) {
                    const Reader c = corrections.element(k);
                    e.corrections.push_back(WordCorrection{TimeRange{c.ticks("start"), c.ticks("duration")},
                                                           c.string("original"), c.string("text")});
                }
            }
            state.transcriptEdits.push_back(std::move(e));
        }
    }
    if (root.has("captionPresets")) {
        const Reader list = root.child("captionPresets");
        if (list.arraySize() > kMaxCaptionPresets) {
            list.fail("more than " + std::to_string(kMaxCaptionPresets) + " caption presets");
        }
        for (std::size_t i = 0; i < list.arraySize(); ++i) {
            const Reader p = list.element(i);
            state.captionPresets.push_back(CaptionPreset{p.string("name"), readCaptionStyle(p.child("style"))});
        }
    }
    if (root.has("reviewNotes")) {
        const Reader list = root.child("reviewNotes");
        if (list.arraySize() > kMaxReviewNotes) {
            list.fail("more than " + std::to_string(kMaxReviewNotes) + " review notes");
        }
        for (std::size_t i = 0; i < list.arraySize(); ++i) {
            state.reviewNotes.push_back(readReviewNote(list.element(i)));
        }
    }
    if (root.has("brandKit")) {
        state.brandKit = detail::brandKitFromJsonValue(root.child("brandKit"));
    }
    if (root.has("templates")) {
        const Reader list = root.child("templates");
        if (list.arraySize() > kMaxProjectTemplates) {
            list.fail("more than " + std::to_string(kMaxProjectTemplates) + " graphic templates");
        }
        for (std::size_t i = 0; i < list.arraySize(); ++i) {
            state.templates.push_back(detail::templateFromJsonValue(list.element(i)));
        }
    }
    return state;
}

Error ioError(const fs::path& file, const std::string& what) { return {ErrorCode::IoError, what + ": " + pathToUtf8(file)}; }

} // namespace

namespace {

/// What of a media item decides its pixels (renderKeyJson).
json mediaRenderJson(const ProjectState& state, const MediaItem& m) {
    json out{{"info", mediaInfoJson(m.info)}};
    if (m.locator.fingerprint.isKnown()) {
        out["file"] = fingerprintJson(m.locator.fingerprint); // the same content wherever it is
    } else {
        out["file"] = m.locator.path;
    }
    if (m.generator) {
        out["generator"] = json{{"kind", m.generator->kind},
                                {"text", m.generator->text},
                                {"fontFamily", m.generator->fontFamily},
                                {"fontStyle", m.generator->fontStyle},
                                {"params", effectParamsJson(m.generator->params)}};
    }
    if (m.subclip) {
        out["subclip"] = json{{"start", m.subclip->range.start.value},
                              {"duration", m.subclip->range.duration.value},
                              {"restricted", m.subclip->restricted}};
    }
    json redactions = json::array();
    for (const Redaction* r : redactionsCovering(state, m.id)) {
        redactions.push_back(redactionJson(*r));
    }
    if (!redactions.empty()) {
        out["redactions"] = std::move(redactions);
    }
    return out;
}

/// A clip as saved, without what does not change pixels, its start relative to `origin`.
json clipRenderJson(const Clip& c, Ticks origin) {
    json out = clipJson(c);
    out.erase("id");
    out.erase("name");
    out.erase("colorIndex");
    out.erase("linkGroup");
    out["start"] = (c.start() - origin).value;
    return out;
}

void sequenceRenderJson(const ProjectState& state, const Sequence& seq, TimeRange range, int depth, json& media, json& nests,
                        json& out) {
    out = json{{"frameRate", rateJson(seq.frameRate)},
               {"width", seq.width},
               {"height", seq.height},
               {"effects", effectStackJson(seq.effects)}};
    if (seq.multicam) {
        json angles = json::array();
        for (const MulticamAngle& a : seq.multicam->angles) {
            angles.push_back(json{{"video", a.video.value}, {"offset", a.offset.value}});
        }
        out["multicam"] = std::move(angles);
    }
    json tracks = json::array();
    for (const TrackPtr& t : seq.videoTracks) {
        json clips = json::array();
        std::vector<ClipId> inTransitions;
        json transitions = json::array();
        for (const Transition& tr : t->transitions) {
            const auto cut = transitionEditPoint(*t, tr);
            if (!cut || *cut - tr.before >= range.end() || *cut + tr.after <= range.start) {
                continue;
            }
            transitions.push_back(json{{"cut", (*cut - range.start).value},
                                       {"before", tr.before.value},
                                       {"after", tr.after.value},
                                       {"alignment", std::string(toString(tr.alignment))},
                                       {"effect", tr.effect},
                                       {"params", effectParamsJson(tr.params)}});
            inTransitions.push_back(tr.outgoing);
            inTransitions.push_back(tr.incoming);
        }
        for (const Clip& c : t->clips) {
            const bool over = c.start() < range.end() && c.end() > range.start;
            if (!over && std::find(inTransitions.begin(), inTransitions.end(), c.id) == inTransitions.end()) {
                continue;
            }
            clips.push_back(clipRenderJson(c, range.start));
            if (c.isNested()) {
                const std::string key = std::to_string(c.sequence.value);
                const Sequence* nested = state.findSequence(c.sequence);
                if (!nests.contains(key)) {
                    nests[key] = nullptr; // a cycle or a repeat stops here
                    if (nested && depth < 8) {
                        json inner;
                        sequenceRenderJson(state, *nested, TimeRange{Ticks{}, std::max(nested->duration, Ticks{1})},
                                           depth + 1, media, nests, inner);
                        nests[key] = std::move(inner);
                    }
                }
            } else if (const MediaItem* m = state.findMedia(c.media)) {
                const std::string key = std::to_string(m->id.value);
                if (!media.contains(key)) {
                    media[key] = mediaRenderJson(state, *m);
                }
            }
        }
        tracks.push_back(json{{"enabled", t->enabled},
                              {"effects", effectStackJson(t->effects)},
                              {"clips", std::move(clips)},
                              {"transitions", std::move(transitions)}});
    }
    out["videoTracks"] = std::move(tracks);
}

} // namespace

std::string renderKeyJson(const ProjectState& state, SequenceId sequence, TimeRange range) {
    const Sequence* seq = state.findSequence(sequence);
    if (!seq) {
        return {};
    }
    try {
        json media = json::object();
        json nests = json::object();
        json top;
        sequenceRenderJson(state, *seq, range, 0, media, nests, top);
        return json{{"sequence", std::move(top)},
                    {"duration", range.duration.value},
                    {"media", std::move(media)},
                    {"nests", std::move(nests)}}
            .dump();
    } catch (const json::exception&) {
        return {};
    }
}

std::expected<std::string, Error> toJson(const ProjectState& state) {
    try {
        return projectJson(state).dump(2);
    } catch (const json::exception& e) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, std::string("cannot serialize project: ") + e.what()});
    }
}

namespace {

/// Migrates and reads a parsed project document (fromJson, history chunks).
std::expected<ProjectState, Error> fromDocument(json root) {
    try {
        if (!root.is_object()) {
            return std::unexpected(Error{ErrorCode::ParseError, "a project file must contain a JSON object"});
        }
        const Reader reader(root, "");
        if (reader.string("format") != kFormatName) {
            return std::unexpected(Error{ErrorCode::ParseError, "this is not a Wabcut project file"});
        }
        const std::int64_t version = reader.int64("schemaVersion");
        if (version > kSchemaVersion) {
            return std::unexpected(Error{
                ErrorCode::VersionMismatch,
                "this project was saved by a newer version of Wabcut (project schema " + std::to_string(version) +
                    "); this version can open schema " + std::to_string(kSchemaVersion) + " and older"});
        }
        if (version < kOldestSchemaVersion) {
            return std::unexpected(Error{ErrorCode::ParseError, "invalid project schema version"});
        }
        for (auto v = version; v < kSchemaVersion; ++v) {
            kMigrations[static_cast<std::size_t>(v - kOldestSchemaVersion)](root);
            root["schemaVersion"] = v + 1;
        }
        ProjectState state = readProject(Reader(root, ""));
        if (auto valid = validate(state, Validation::Structure); !valid) {
            return std::unexpected(Error{ErrorCode::ParseError, "the project file is damaged: " + valid.error().message});
        }
        return state;
    } catch (const FormatError& e) {
        return std::unexpected(Error{ErrorCode::ParseError, "invalid project file at " + e.message});
    } catch (const json::exception& e) {
        return std::unexpected(Error{ErrorCode::ParseError, std::string("invalid project file: ") + e.what()});
    }
}

/// The key of a {"$chunk": key} reference, or nullopt.
std::optional<std::string> chunkKey(const json& j) {
    if (j.is_object() && j.size() == 1 && j.contains("$chunk") && j["$chunk"].is_string()) {
        return j["$chunk"].get<std::string>();
    }
    return std::nullopt;
}

json chunkRef(const std::string& key) { return json{{"$chunk", key}}; }

} // namespace

bool jsonNestingWithin(std::string_view text, std::size_t maxDepth) {
    std::size_t depth = 0;
    bool inString = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            if (c == '\\') {
                ++i; // the escaped character
            } else if (c == '"') {
                inString = false;
            }
        } else if (c == '"') {
            inString = true;
        } else if (c == '[' || c == '{') {
            if (++depth > maxDepth) {
                return false;
            }
        } else if ((c == ']' || c == '}') && depth > 0) {
            --depth;
        }
    }
    return true;
}

std::expected<ProjectState, Error> fromJson(std::string_view text) {
    if (!jsonNestingWithin(text)) {
        return std::unexpected(Error{ErrorCode::ParseError, "the project file is nested deeper than any project"});
    }
    json root = json::parse(text.begin(), text.end(), nullptr, false);
    if (root.is_discarded()) {
        return std::unexpected(Error{ErrorCode::ParseError, "the project file is not valid JSON"});
    }
    return fromDocument(std::move(root));
}

namespace detail {

std::expected<std::string, Error> writeChunks(const ProjectState& state, ChunkSink& sink) {
    try {
        const auto trackOut = [&](const TrackPtr& t) {
            const std::shared_ptr<const void> node = t;
            if (auto key = sink.known(node)) {
                return chunkRef(*key);
            }
            return chunkRef(sink.put(node, trackJson(*t).dump()));
        };
        const auto sequenceOut = [&](const SequencePtr& s) {
            const std::shared_ptr<const void> node = s;
            if (auto key = sink.known(node)) {
                return chunkRef(*key);
            }
            return chunkRef(sink.put(node, sequenceJson(*s, trackOut).dump()));
        };
        const auto mediaOut = [&](json media) { return chunkRef(sink.put(nullptr, media.dump())); };
        return sink.put(nullptr, projectJson(state, sequenceOut, mediaOut).dump());
    } catch (const json::exception& e) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, std::string("cannot serialize project: ") + e.what()});
    }
}

std::expected<ProjectState, Error> readChunks(const std::string& rootKey, const ChunkSource& get) {
    const auto parse = [&](const std::string& key) -> std::expected<json, Error> {
        auto text = get(key);
        if (!text) {
            return std::unexpected(text.error());
        }
        if (!jsonNestingWithin(*text)) {
            return std::unexpected(Error{ErrorCode::ParseError, "history chunk " + key + " is nested too deep"});
        }
        json j = json::parse(text->begin(), text->end(), nullptr, false);
        if (j.is_discarded()) {
            return std::unexpected(Error{ErrorCode::ParseError, "history chunk " + key + " is not valid JSON"});
        }
        return j;
    };
    auto root = parse(rootKey);
    if (!root) {
        return std::unexpected(root.error());
    }
    if (!root->is_object()) {
        return std::unexpected(Error{ErrorCode::ParseError, "a history revision must be a JSON object"});
    }
    // Put the pieces back in place: the media list, each sequence and, inside, each track.
    const auto expand = [&](json& j) -> std::expected<void, Error> {
        if (const auto key = chunkKey(j)) {
            auto piece = parse(*key);
            if (!piece) {
                return std::unexpected(piece.error());
            }
            j = std::move(*piece);
        }
        return {};
    };
    if (root->contains("media")) {
        if (auto ok = expand((*root)["media"]); !ok) {
            return std::unexpected(ok.error());
        }
    }
    if (root->contains("sequences") && (*root)["sequences"].is_array()) {
        for (json& s : (*root)["sequences"]) {
            if (auto ok = expand(s); !ok) {
                return std::unexpected(ok.error());
            }
            for (const char* list : {"videoTracks", "audioTracks"}) {
                if (s.is_object() && s.contains(list) && s[list].is_array()) {
                    for (json& t : s[list]) {
                        if (auto ok = expand(t); !ok) {
                            return std::unexpected(ok.error());
                        }
                    }
                }
            }
        }
    }
    return fromDocument(std::move(*root));
}

std::expected<std::vector<std::string>, Error> chunkReferences(std::string_view text) {
    if (!jsonNestingWithin(text)) {
        return std::unexpected(Error{ErrorCode::ParseError, "a history chunk is nested too deep"});
    }
    const json j = json::parse(text.begin(), text.end(), nullptr, false);
    if (j.is_discarded()) {
        return std::unexpected(Error{ErrorCode::ParseError, "a history chunk is not valid JSON"});
    }
    std::vector<std::string> out;
    const auto take = [&](const json& v) {
        if (const auto key = chunkKey(v)) {
            out.push_back(*key);
        }
    };
    if (j.is_object()) {
        if (j.contains("media")) {
            take(j["media"]);
        }
        for (const char* list : {"sequences", "videoTracks", "audioTracks"}) {
            if (j.contains(list) && j[list].is_array()) {
                for (const json& v : j[list]) {
                    take(v);
                }
            }
        }
    }
    return out;
}

} // namespace detail

std::expected<void, Error> writeFileAtomically(const fs::path& file, std::string_view contents) {
    fs::path temp = file;
    temp += ".tmp";
#ifdef _WIN32
    std::FILE* f = _wfopen(temp.c_str(), L"wb");
#else
    std::FILE* f = std::fopen(temp.c_str(), "wb");
#endif
    if (!f) {
        return std::unexpected(ioError(temp, "cannot create"));
    }
    bool ok = std::fwrite(contents.data(), 1, contents.size(), f) == contents.size();
    ok = std::fflush(f) == 0 && ok;
#ifdef _WIN32
    ok = _commit(_fileno(f)) == 0 && ok;
#else
    ok = ::fsync(fileno(f)) == 0 && ok;
#endif
    ok = std::fclose(f) == 0 && ok;
    std::error_code ec;
    if (!ok) {
        fs::remove(temp, ec);
        return std::unexpected(ioError(temp, "cannot write"));
    }
    fs::rename(temp, file, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        return std::unexpected(ioError(file, "cannot replace (" + ec.message() + ")"));
    }
    return {};
}

namespace {

/// Project files this process saved or loaded, with their size and modification time then (P.1). saveProject only
/// rotates the current file into the backups when it loads; a file still exactly as we wrote or read it does, without
/// parsing it all again (that parse was a third of a 10,000-clip save). Anything else (a file another program
/// changed, or one we never read) is still loaded to decide.
struct FileStamp {
    std::uintmax_t size = 0;
    fs::file_time_type time;
    bool operator==(const FileStamp&) const = default;
};

std::optional<FileStamp> stampOf(const fs::path& file) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(file, ec);
    if (ec) {
        return std::nullopt;
    }
    const fs::file_time_type time = fs::last_write_time(file, ec);
    if (ec) {
        return std::nullopt;
    }
    return FileStamp{size, time};
}

fs::path stampKey(const fs::path& file) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(file, ec);
    return (ec ? file : absolute).lexically_normal();
}

std::mutex gLoadableMutex;
std::map<fs::path, FileStamp> gLoadable; // a few projects per session; cleared past 64 entries

/// Remembers `file` as loadable, as it is now; with `expected`, only when that is still the file that was read.
void rememberLoadable(const fs::path& file, const std::optional<FileStamp>& expected = std::nullopt) {
    const auto stamp = stampOf(file);
    if (!stamp || (expected && *expected != *stamp)) {
        return;
    }
    const std::lock_guard lock(gLoadableMutex);
    if (gLoadable.size() >= 64) {
        gLoadable.clear();
    }
    gLoadable[stampKey(file)] = *stamp;
}

bool knownLoadable(const fs::path& file) {
    const auto stamp = stampOf(file);
    if (!stamp) {
        return false;
    }
    const std::lock_guard lock(gLoadableMutex);
    const auto it = gLoadable.find(stampKey(file));
    return it != gLoadable.end() && it->second == *stamp;
}

} // namespace

fs::path backupPath(const fs::path& projectFile, int n) {
    fs::path out = projectFile;
    out += ".bak" + std::to_string(n);
    return out;
}

fs::path autosavePath(const fs::path& projectFile) {
    fs::path out = projectFile;
    out += ".autosave";
    return out;
}

std::expected<void, Error> saveProject(const ProjectState& state, const fs::path& file, SaveOptions options) {
    auto text = toJson(state);
    if (!text) {
        return std::unexpected(std::move(text.error()));
    }
    // Never replace a save with something that would not load again.
    if (auto check = fromJson(*text); !check) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "refusing to save a project that would not load: " +
                                                                     check.error().message});
    }

    std::error_code ec;
    if (options.backups > 0 && fs::is_regular_file(file, ec) && (knownLoadable(file) || loadProject(file))) {
        // Rotate the recovery copies, newest first: .bak4 -> .bak5, …, .bak1 -> .bak2, current -> .bak1.
        fs::remove(backupPath(file, options.backups), ec);
        for (int n = options.backups - 1; n >= 1; --n) {
            if (fs::exists(backupPath(file, n), ec)) {
                fs::rename(backupPath(file, n), backupPath(file, n + 1), ec);
                if (ec) {
                    return std::unexpected(ioError(backupPath(file, n), "cannot rotate the recovery copy"));
                }
            }
        }
        fs::copy_file(file, backupPath(file, 1), fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return std::unexpected(ioError(backupPath(file, 1), "cannot write the recovery copy"));
        }
    }
    auto written = writeFileAtomically(file, *text);
    if (written) {
        rememberLoadable(file); // it parsed above
    }
    return written;
}

std::expected<ProjectState, Error> loadProject(const fs::path& file) {
    const std::optional<FileStamp> before = stampOf(file);
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(ioError(file, "cannot open"));
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        return std::unexpected(ioError(file, "cannot read"));
    }
    auto state = fromJson(buffer.view());
    if (state && before) {
        rememberLoadable(file, before); // unless the file changed while it was read
    }
    return state;
}

std::expected<void, Error> writeAutosave(const ProjectState& state, const fs::path& projectFile) {
    return saveProject(state, autosavePath(projectFile), SaveOptions{0});
}

std::vector<RecoveryCandidate> recoveryCandidates(const fs::path& projectFile) {
    std::vector<RecoveryCandidate> out;
    std::error_code ec;
    const bool projectExists = fs::is_regular_file(projectFile, ec);
    const auto projectTime = projectExists ? fs::last_write_time(projectFile, ec) : fs::file_time_type{};
    const auto consider = [&](const fs::path& file, RecoveryCandidate::Kind kind, int index) {
        std::error_code e;
        if (!fs::is_regular_file(file, e)) {
            return;
        }
        RecoveryCandidate c;
        c.file = file;
        c.kind = kind;
        c.backupIndex = index;
        c.modified = fs::last_write_time(file, e);
        c.newerThanProject = !projectExists || c.modified > projectTime;
        c.loadable = loadProject(file).has_value();
        out.push_back(std::move(c));
    };
    consider(autosavePath(projectFile), RecoveryCandidate::Kind::Autosave, 0);
    for (int n = 1; n <= kDefaultBackupCount; ++n) {
        consider(backupPath(projectFile, n), RecoveryCandidate::Kind::Backup, n);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const RecoveryCandidate& a, const RecoveryCandidate& b) { return a.modified > b.modified; });
    return out;
}

std::expected<bool, Error> Autosaver::save(const std::shared_ptr<const ProjectState>& snapshot) {
    if (!snapshot) {
        return false;
    }
    if (m_lastSaved && (m_lastSaved == snapshot || isSameRevision(*m_lastSaved, *snapshot))) {
        return false;
    }
    if (auto written = writeAutosave(*snapshot, m_projectFile); !written) {
        return std::unexpected(std::move(written.error()));
    }
    m_lastSaved = snapshot;
    return true;
}

// --- Shared with the other JSON readers of the model (JsonIo.h) -----------------------------------------

namespace detail {

nlohmann::json paramValueToJson(const ParamValue& value) { return io::paramValueJson(value); }
ParamValue paramValueFromJson(const Reader& reader) { return io::readParamValue(reader); }
nlohmann::json maskToJson(const Mask& mask) { return io::maskJson(mask); }
Mask maskFromJson(const Reader& reader) { return io::readMask(reader); }

} // namespace detail

} // namespace wabcut::io
