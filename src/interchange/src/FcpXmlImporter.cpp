// FCPXML 1.9-1.11 (Final Cut Pro 10.4.9+, DaVinci Resolve), also read leniently for 1.5-1.8 and 1.12+.
//
//   <fcpxml version="1.10">
//     <resources> <format id frameDuration="1001/24000s" width height/>
//                 <asset id name start duration hasVideo hasAudio audioSources audioChannels audioRate
//                 format>
//                   <media-rep kind="original-media" src="file:///..."/></asset>        (1.8: asset
//                   src="...")
//                 <effect id name uid/>  <media id name><sequence>...</sequence></media>  (compound clips)
//     <library><event><project name><sequence format duration tcStart tcFormat audioRate>
//       <spine> asset-clip | clip | gap | transition | title | ref-clip | sync-clip | mc-clip | audition ...
//
// Times are rational seconds ("1001/30000s"). Each element's `offset` is in its parent's local time and its
// `start` opens its own local time for anchored children: sequenceTime(child) = sequenceTime(parent) +
// (child.offset - parent.start). Top-level spine offsets count from the sequence's tcStart. Spine items
// are back to back; transitions overlap the cut between them and take no time.
// Connected clips (lane > 0 above, < 0 below) become tracks: lanes in order, packed onto extra tracks
// where clips of one lane (anchored to different parents) overlap.

#include "ImporterCommon.h"
#include "Importers.h"
#include "TimeMath.h"
#include "XmlUtil.h"

#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace wabcut::interchange::detail {

namespace {

bool nameIs(const pugi::xml_node& n, const char* name) {
    return std::strcmp(n.name(), name) == 0;
}

/// Maps an element's local time to sequence time: sequence = base + (local - localStart).
struct TimeContext {
    Ticks base;
    Ticks localStart;
    /// Absolute lane of the parent (0 = primary storyline).
    int lane = 0;

    Ticks map(Ticks local) const { return base + (local - localStart); }
};

struct Asset {
    std::string id;
    std::string name;
    Ticks start;
    std::optional<Ticks> duration;
    bool hasVideo = false;
    bool hasAudio = false;
    std::size_t media = kNoMedia;
};

struct Format {
    std::optional<Rate> rate;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

/// Clips, transitions and gaps of one absolute lane, before packing onto tracks.
struct Lane {
    std::vector<IrClip> clips;
    std::vector<IrTransition> transitions;
    std::vector<IrGap> gaps;
};

class FcpXmlParser {
public:
    FcpXmlParser(std::string_view data, const ParseContext& context, IrDocument& doc)
        : m_ctx(context), m_doc(doc), m_media(doc), m_loc(data, context.fileLabel) {}

    std::expected<void, Error> parse(const pugi::xml_document& xml);

private:
    struct SequenceState {
        std::string name;
        Rate rate = kRate30;
        bool dropFrame = false;
        /// The frame height: FCP measures position and anchor in percent of it.
        std::int32_t height = 1080;
        std::map<int, Lane> videoLanes;
        std::map<int, Lane> audioLanes;
        std::vector<IrMarker> markers;
    };

    void report(ReportOutcome o, ReportCategory c, std::string reason, const pugi::xml_node& node) {
        m_doc.report.add(o, c, std::move(reason), m_loc.locate(node),
                         m_state ? m_state->name : std::string());
    }
    std::optional<Ticks> time(const pugi::xml_node& node, const char* attribute) {
        const pugi::xml_attribute a = node.attribute(attribute);
        if (!a) {
            return std::nullopt;
        }
        const auto t = parseFcpxTime(a.value());
        if (!t) {
            return std::nullopt;
        }
        m_inexact = m_inexact || !t->exact;
        return Ticks{t->value};
    }

    void readResources(const pugi::xml_node& resources);
    std::size_t mediaForAsset(Asset& asset, const pugi::xml_node& node);
    /// A <media><multicam> resource as a multicam source sequence (C.8): one angle per <mc-angle>.
    void parseMulticam(const pugi::xml_node& media);
    std::expected<void, Error> parseProject(const pugi::xml_node& project, const pugi::xml_node& sequence,
                                            const std::string& name, int depth);
    /// Asset id -> the event whose browser clips use it (X.1: events become bins).
    std::unordered_map<std::string, std::string> m_assetEvents;
    void processSpine(const pugi::xml_node& spine, const TimeContext& ctx, int lane, int depth);
    void processItem(const pugi::xml_node& item, const TimeContext& ctx, int lane, int depth,
                     std::optional<TimeRange> window, std::int64_t linkGroup, bool insideClip);
    void processAnchored(const pugi::xml_node& item, const TimeContext& ctx, int depth,
                         bool clipContainer = false);
    void processMarkers(const pugi::xml_node& item, const TimeContext& ctx, std::optional<TimeRange> visible,
                        const std::string& clipName);
    void addMediaClip(const pugi::xml_node& node, const Asset& asset, bool video, bool audio, Ticks seqStart,
                      Ticks duration, Ticks localStart, int lane, std::optional<TimeRange> window,
                      std::int64_t linkGroup, const std::string& name);
    std::optional<ClipProperties> propertiesOf(const pugi::xml_node& node, const std::string& name);
    /// The keyframes of an adjustment's <param name="..."><keyframeAnimation> as a curve in the clip's own time (X.1),
    /// each value read by `value`; nullopt when there are none or they are unusable (reported).
    template <class Read>
    std::optional<ParamValue> keyframesOf(const pugi::xml_node& adjust, const char* param, ParamId target, Read value,
                                          const std::string& name);
    /// A <title> (Basic Title and the like) as a title clip: its text, font, size, colour and alignment (X.1).
    void addTitle(const pugi::xml_node& item, Ticks seqStart, Ticks duration, int lane, const std::string& name);
    /// A.9: the clip's <audio-channel-source> elements as a channel map (nullopt: none, the whole stream).
    std::optional<AudioChannelMap> channelMapOf(const pugi::xml_node& node, const std::string& name);
    std::optional<IrSpeed> speedOf(const pugi::xml_node& node, const std::string& name, Ticks localStart,
                                   Ticks& mappedStart, std::optional<IrRemap>& remap);
    IrSequence assembleSequence(SequenceState& state, const pugi::xml_node& node);

    [[maybe_unused]] const ParseContext& m_ctx; // not read yet (Clang: -Wunused-private-field)
    IrDocument& m_doc;
    MediaTable m_media;
    XmlLocator m_loc;
    std::unordered_map<std::string, Asset> m_assets;
    std::unordered_map<std::string, Format> m_formats;
    std::unordered_map<std::string, pugi::xml_node> m_effects;
    std::unordered_map<std::string, pugi::xml_node> m_compounds;
    std::unordered_set<std::string> m_compoundsImported;
    /// Compound clip <media> resources referenced by the current project, imported after it.
    std::vector<pugi::xml_node> m_pendingCompounds;
    SequenceState* m_state = nullptr;
    /// Name of the <clip> whose content is being read; its unnamed <video>/<audio> take it.
    std::string m_clipName;
    std::int64_t m_nextLink = 0;
    bool m_inexact = false;
};

void FcpXmlParser::readResources(const pugi::xml_node& resources) {
    for (const pugi::xml_node r : resources.children()) {
        const std::string id = r.attribute("id").value();
        if (id.empty()) {
            continue;
        }
        if (nameIs(r, "format")) {
            Format f;
            if (const auto fd = parseFcpxTime(r.attribute("frameDuration").value()); fd && fd->value > 0) {
                // frameDuration n/d s -> rate d/n frames per second.
                const std::string_view text = trim(r.attribute("frameDuration").value());
                const auto slash = text.find('/');
                if (slash != std::string_view::npos) {
                    const auto num = parseInt64(text.substr(0, slash));
                    std::string_view den = text.substr(slash + 1);
                    if (!den.empty() && den.back() == 's') {
                        den.remove_suffix(1);
                    }
                    const auto d = parseInt64(den);
                    if (num && d) {
                        f.rate = Rate::make(*d, *num); // frameDuration n/d s -> d/n frames per second
                    }
                } else if (const auto seconds = parseInt64(text.substr(0, text.find('s')));
                           seconds && *seconds == 1) {
                    f.rate = Rate{1, 1};
                }
            }
            f.width = static_cast<std::int32_t>(attrInt(r, "width").value_or(0));
            f.height = static_cast<std::int32_t>(attrInt(r, "height").value_or(0));
            m_formats[id] = f;
        } else if (nameIs(r, "asset")) {
            Asset a;
            a.id = id;
            a.name = r.attribute("name").value();
            a.start = time(r, "start").value_or(Ticks{});
            a.duration = time(r, "duration");
            a.hasVideo = attrInt(r, "hasVideo").value_or(0) != 0;
            a.hasAudio = attrInt(r, "hasAudio").value_or(0) != 0;
            a.media = mediaForAsset(a, r);
            m_assets[id] = std::move(a);
        } else if (nameIs(r, "effect")) {
            m_effects[id] = r;
        } else if (nameIs(r, "media")) {
            m_compounds[id] = r;
        }
    }
}

std::size_t FcpXmlParser::mediaForAsset(Asset& asset, const pugi::xml_node& node) {
    IrMedia media;
    media.key = "asset:" + asset.id;
    media.name = asset.name;
    media.location = m_loc.locate(node);
    // 1.9+: <media-rep kind="original-media" src>; up to 1.8: the asset's own src attribute.
    std::string src = node.attribute("src").value();
    for (const pugi::xml_node rep : node.children("media-rep")) {
        const std::string_view kind = attrText(rep, "kind");
        if (kind.empty() || kind == "original-media") {
            src = rep.attribute("src").value();
            break;
        }
    }
    bool decoded = false;
    media.reference = referenceFrom(src, asset.name, decoded);
    if (!src.empty() && !decoded) {
        report(ReportOutcome::Approximated, ReportCategory::Media,
               "the location of '" + asset.name + "' (" + src + ") is not a file path; the media is offline",
               node);
    }
    if (media.name.empty()) {
        media.name = media.reference.name;
    }
    media.hasVideo = asset.hasVideo;
    if (const auto it = m_formats.find(node.attribute("format").value()); it != m_formats.end()) {
        media.frameRate = it->second.rate;
        media.width = it->second.width;
        media.height = it->second.height;
    }
    if (asset.hasAudio) {
        const auto sources = std::clamp<std::int64_t>(attrInt(node, "audioSources").value_or(1), 1, 32);
        const auto channels = std::clamp<std::int64_t>(attrInt(node, "audioChannels").value_or(2), 1, 64);
        for (std::int64_t s = 0; s < sources; ++s) {
            media.audioChannels.push_back(
                static_cast<std::int32_t>(std::max<std::int64_t>(1, channels / sources)));
        }
        if (const auto rate = attrInt(node, "audioRate"); rate && *rate > 0 && *rate < 1'000'000) {
            media.sampleRate = Rate{static_cast<std::int32_t>(*rate), 1};
        }
    }
    media.duration = asset.duration;
    media.stillImage = asset.hasVideo && !asset.hasAudio &&
                       (isStillImageName(media.reference.path.empty() ? media.name : media.reference.path) ||
                        (asset.duration && asset.duration->value == 0));
    return m_media.add(std::move(media));
}

namespace {

/// "1, 2" -> {1, 2}; "L, R" -> {"L", "R"}.
std::vector<std::string> listOf(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::string_view part = trim(text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
        if (!part.empty()) {
            out.emplace_back(part);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

/// The 5.1 position of an FCPXML output channel name, or -1.
int surroundIndex(std::string_view name) {
    static constexpr std::string_view kNames[] = {"L", "R", "C", "LFE", "Ls", "Rs"};
    for (int i = 0; i < 6; ++i) {
        if (name == kNames[i]) {
            return i;
        }
    }
    return -1;
}

} // namespace

std::optional<AudioChannelMap> FcpXmlParser::channelMapOf(const pugi::xml_node& node, const std::string& name) {
    std::vector<pugi::xml_node> sources;
    for (const pugi::xml_node child : node.children("audio-channel-source")) {
        sources.push_back(child);
    }
    if (sources.empty()) {
        return std::nullopt;
    }
    const auto sourceChannels = [](const pugi::xml_node& n) {
        std::vector<std::int32_t> out;
        for (const std::string& s : listOf(attrText(n, "srcCh"))) {
            char* end = nullptr;
            const long v = std::strtol(s.c_str(), &end, 10);
            if (end != s.c_str() && *end == '\0' && v >= 1 && v <= kMaxSourceChannels) {
                out.push_back(static_cast<std::int32_t>(v - 1));
            }
        }
        return out;
    };
    const auto inactive = [](const pugi::xml_node& n) {
        return attrText(n, "active") == "0" || attrText(n, "enabled") == "0";
    };
    if (sources.size() > 1) {
        // Several components (e.g. each microphone of a file as its own mono component), mixed by Final Cut: one
        // mono clip channel of all the active ones.
        AudioChannelMap map = AudioChannelMap::mono(0);
        map.channels[0].sources.clear();
        for (const pugi::xml_node& s : sources) {
            if (inactive(s)) {
                continue;
            }
            for (const std::int32_t c : sourceChannels(s)) {
                if (map.channels[0].sources.size() < static_cast<std::size_t>(kMaxRouteSources) &&
                    std::find(map.channels[0].sources.begin(), map.channels[0].sources.end(), c) == map.channels[0].sources.end()) {
                    map.channels[0].sources.push_back(c);
                }
            }
        }
        report(ReportOutcome::Approximated, ReportCategory::AudioMapping,
               "'" + name + "' has " + std::to_string(sources.size()) +
                   " audio components; they became one mono channel of their sources averaged",
               node);
        return map;
    }
    const pugi::xml_node s = sources.front();
    const std::vector<std::int32_t> src = sourceChannels(s);
    const std::vector<std::string> out = listOf(attrText(s, "outCh"));
    if (src.empty()) {
        report(ReportOutcome::Approximated, ReportCategory::AudioMapping,
               "'" + name + "' has an audio component without source channels; it plays the whole stream", node);
        return std::nullopt;
    }
    AudioChannelMap map;
    const bool surround = out.size() == 6 && std::all_of(out.begin(), out.end(), [](const std::string& o) { return surroundIndex(o) >= 0; });
    if (surround && src.size() == 6) {
        map.format = ChannelFormat::Surround51;
        map.channels.resize(6);
        for (std::size_t i = 0; i < 6; ++i) {
            map.channels[static_cast<std::size_t>(surroundIndex(out[i]))].sources = {src[i]};
        }
    } else if (src.size() == 2 && (out.empty() || (out.size() == 2 && out[0] == "L" && out[1] == "R"))) {
        map.format = ChannelFormat::Stereo;
        map.channels = {ChannelRoute{{src[0]}, false, ParamValue(0.0)}, ChannelRoute{{src[1]}, false, ParamValue(0.0)}};
    } else if (src.size() == 2 && out.size() == 2 && out[0] == "R" && out[1] == "L") {
        map.format = ChannelFormat::Stereo;
        map.channels = {ChannelRoute{{src[1]}, false, ParamValue(0.0)}, ChannelRoute{{src[0]}, false, ParamValue(0.0)}};
    } else if (src.size() == 1) {
        map = AudioChannelMap::mono(src[0]);
    } else {
        map = AudioChannelMap::identity(static_cast<int>(src.size()));
        for (std::size_t i = 0; i < src.size() && i < map.channels.size(); ++i) {
            map.channels[i].sources = {src[i]};
        }
        if (!out.empty()) {
            report(ReportOutcome::Approximated, ReportCategory::AudioMapping,
                   "the output channels '" + std::string(attrText(s, "outCh")) + "' of '" + name +
                       "' became a " + std::string(toString(map.format)) + " clip in source order",
                   node);
        }
    }
    if (inactive(s)) {
        for (ChannelRoute& r : map.channels) {
            r.muted = true;
        }
    }
    report(ReportOutcome::Carried, ReportCategory::AudioMapping,
           "'" + name + "' plays source channels " + std::string(attrText(s, "srcCh")) + " as a " +
               std::string(toString(map.format)) + " clip",
           node);
    return map;
}

std::optional<ClipProperties> FcpXmlParser::propertiesOf(const pugi::xml_node& node,
                                                         const std::string& name) {
    ClipProperties props;
    bool any = false;
    for (const pugi::xml_node child : node.children()) {
        const std::string_view tag = child.name();
        if (tag == "adjust-volume") {
            const auto decibels = [](std::string_view text) -> std::optional<double> {
                if (iendsWith(text, "dB")) {
                    text.remove_suffix(2);
                }
                return parseDouble(text);
            };
            if (const auto db = decibels(attrText(child, "amount"));
                db && ParamValue(*db).isValid(parameterInfo(ParamId::VolumeDb).minValue, parameterInfo(ParamId::VolumeDb).maxValue)) {
                props.set(ParamId::VolumeDb, *db);
                any = true;
            }
            if (auto keyed = keyframesOf(child, "amount", ParamId::VolumeDb, decibels, name)) {
                props[ParamId::VolumeDb] = std::move(*keyed);
                any = true;
            }
        } else if (tag == "adjust-blend") {
            if (const auto amount = parseDouble(attrText(child, "amount"));
                amount && *amount >= 0.0 && *amount <= 1.0) {
                props.set(ParamId::Opacity, *amount);
                any = true;
            }
            if (auto keyed = keyframesOf(child, "amount", ParamId::Opacity, [](std::string_view s) { return parseDouble(s); }, name)) {
                props[ParamId::Opacity] = std::move(*keyed);
                any = true;
            }
        } else if (tag == "adjust-transform") {
            const auto scale = splitWords(attrText(child, "scale"));
            if (scale.size() == 2) {
                const auto sx = parseDouble(scale[0]);
                const auto sy = parseDouble(scale[1]);
                if (sx && sy && *sx == *sy && *sx >= 0.0) {
                    props.set(ParamId::Scale, *sx);
                    any = true;
                } else if (sx && sy) {
                    report(ReportOutcome::Approximated, ReportCategory::Effect,
                           "non-uniform scale of '" + name + "' imported as uniform scale " +
                               std::string(scale[0]),
                           child);
                    props.set(ParamId::Scale, std::max(0.0, *sx));
                    any = true;
                }
            }
            if (const auto rotation = parseDouble(attrText(child, "rotation"))) {
                props.set(ParamId::Rotation, -*rotation); // FCP rotates counter-clockwise
                any = true;
            }
            if (auto keyed = keyframesOf(child, "scale", ParamId::Scale,
                                         [](std::string_view s) -> std::optional<double> {
                                             const auto words = splitWords(s);
                                             return words.empty() ? std::nullopt : parseDouble(words.front());
                                         },
                                         name)) {
                props[ParamId::Scale] = std::move(*keyed);
                any = true;
            }
            if (auto keyed = keyframesOf(child, "rotation", ParamId::Rotation,
                                         [](std::string_view s) -> std::optional<double> {
                                             const auto v = parseDouble(s);
                                             return v ? std::optional<double>(-*v) : std::nullopt;
                                         },
                                         name)) {
                props[ParamId::Rotation] = std::move(*keyed);
                any = true;
            }
            // Position and anchor: percent of the frame height, y up (X.1).
            const double height = m_state ? m_state->height : 1080.0;
            for (const auto& [attribute, px, py] : {std::tuple{"position", ParamId::PositionX, ParamId::PositionY},
                                                    std::tuple{"anchor", ParamId::AnchorX, ParamId::AnchorY}}) {
                const auto words = splitWords(attrText(child, attribute));
                if (words.size() != 2) {
                    continue;
                }
                const auto x = parseDouble(words[0]);
                const auto y = parseDouble(words[1]);
                if (!x || !y || (*x == 0.0 && *y == 0.0)) {
                    continue;
                }
                const double vx = *x / 100.0 * height;
                const double vy = -*y / 100.0 * height;
                if (ParamValue(vx).isValid(parameterInfo(px).minValue, parameterInfo(px).maxValue) &&
                    ParamValue(vy).isValid(parameterInfo(py).minValue, parameterInfo(py).maxValue)) {
                    props.set(px, vx);
                    props.set(py, vy);
                    any = true;
                } else {
                    report(ReportOutcome::Dropped, ReportCategory::Effect,
                           std::string(attribute) + " of '" + name + "' is out of range and was not imported", child);
                }
            }
        } else if (tag == "filter-video" || tag == "filter-audio") {
            std::string effectName = child.attribute("name").value();
            if (effectName.empty()) {
                if (const auto it = m_effects.find(child.attribute("ref").value()); it != m_effects.end()) {
                    effectName = it->second.attribute("name").value();
                }
            }
            report(ReportOutcome::Dropped, ReportCategory::Effect,
                   "effect '" + effectName + "' on '" + name + "' is not supported yet", child);
        } else if (tag.starts_with("adjust-") && tag != "adjust-conform") {
            report(ReportOutcome::Dropped, ReportCategory::Effect,
                   std::string(tag) + " on '" + name + "' is not supported yet", child);
        }
    }
    if (!any) {
        return std::nullopt;
    }
    return props;
}

std::optional<IrSpeed> FcpXmlParser::speedOf(const pugi::xml_node& node, const std::string& name, Ticks localStart,
                                            Ticks& mappedStart, std::optional<IrRemap>& remap) {
    // <timeMap><timept time="adjusted" value="original"/>...: the clip's start is in adjusted time; the
    // media position is the interpolated original time. A straight map is a constant speed: exact, since the
    // points are rational times (value delta / time delta).
    mappedStart = localStart;
    const pugi::xml_node map = node.child("timeMap");
    if (!map) {
        return std::nullopt;
    }
    std::vector<std::pair<Ticks, Ticks>> points;
    std::vector<bool> smoothPoints;
    bool smooth = false;
    for (const pugi::xml_node p : map.children("timept")) {
        const auto t = parseFcpxTime(p.attribute("time").value());
        const auto v = parseFcpxTime(p.attribute("value").value());
        if (t && v) {
            points.emplace_back(Ticks{t->value}, Ticks{v->value});
            const std::string_view interp = p.attribute("interp").value();
            smoothPoints.push_back(!interp.empty() && interp != "linear");
            smooth = smooth || smoothPoints.back();
        }
    }
    if (points.size() < 2 || points.back().first <= points.front().first) {
        return std::nullopt;
    }
    // Untrusted numbers: nothing below is evaluated unless every difference fits (media times are not negative, the
    // time span fits in ticks) and the map is not huge (the remap would be refused anyway).
    const bool sane = points.size() <= kMaxRemapKeys &&
                      std::ranges::all_of(points, [](const auto& p) { return p.second.value >= 0; }) &&
                      checkedSub(points.back().first, points.front().first).has_value() &&
                      std::ranges::all_of(points, [&](const auto& p) {
                          return checkedSub(p.first, points.front().first).has_value() &&
                                 checkedSub(points.back().first, p.first).has_value();
                      });
    if (!sane) {
        report(ReportOutcome::Dropped, ReportCategory::Speed,
               "'" + name + "' has a time map out of range (negative media times, a span past the tick range or more "
                            "than " + std::to_string(kMaxRemapKeys) + " points); it plays at 100%",
               map);
        return std::nullopt;
    }
    const std::int64_t dt = (points.back().first - points.front().first).value;
    const std::int64_t dv = (points.back().second - points.front().second).value;
    const double speed = static_cast<double>(dv) / static_cast<double>(dt);
    bool linear = true;
    for (std::size_t i = 1; i + 1 < points.size(); ++i) {
        const double expected = static_cast<double>(points.front().second.value) +
                                static_cast<double>((points[i].first - points.front().first).value) * speed;
        linear = linear && std::abs(expected - static_cast<double>(points[i].second.value)) <
                               static_cast<double>(kTicksPerSecond) / 1000.0;
    }
    // Interpolate the media position of the clip's start.
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const auto& [t0, v0] = points[i];
        const auto& [t1, v1] = points[i + 1];
        if (localStart >= t0 && localStart <= t1 && t1 > t0) {
            const auto offset = mulDiv((localStart - t0).value, (v1 - v0).value, (t1 - t0).value, Rounding::Nearest);
            mappedStart = v0 + Ticks{offset.value_or(0)};
            break;
        }
    }
    if (!linear) {
        // A speed ramp: the time map is a time remap (adjusted time = curve time, original time = position);
        // rational points are exact unless they fall between ticks or the interpolation is smooth.
        IrRemap r;
        r.location = m_loc.locate(map);
        r.curveIn = localStart;
        std::vector<bool> smoothKeys;
        for (std::size_t p = 0; p < points.size(); ++p) {
            const auto& [t, v] = points[p];
            Keyframe k{t, static_cast<double>(v.value)};
            if (const auto i = r.curve.keyAt(t)) {
                r.curve.keys[*i] = k;
                smoothKeys[*i] = smoothPoints[p];
            } else {
                const std::size_t at = r.curve.insertionIndex(t);
                r.curve.keys.insert(r.curve.keys.begin() + static_cast<std::ptrdiff_t>(at), k);
                smoothKeys.insert(smoothKeys.begin() + static_cast<std::ptrdiff_t>(at), smoothPoints[p]);
            }
        }
        // X.1: smooth points become Bezier keys (the speed ramps through them) with monotone tangents.
        if (smooth && !smoothRemapKeys(r.curve, smoothKeys)) {
            r.approximation = "its smooth speed ramps were imported as straight segments";
        } else if (smooth) {
            r.approximation = "its smooth speed ramps were imported as Bezier keys with tangents through the "
                              "neighbouring points (FCPXML keeps no handles)";
        }
        mappedStart = remapSourceAt(r.curve, localStart);
        remap = std::move(r);
        return std::nullopt;
    }
    if (dv == dt) {
        return std::nullopt;
    }
    IrSpeed sp;
    sp.location = m_loc.locate(map);
    if (dv == 0) {
        sp.ratio = wabcut::Speed{0, 1};
        return sp;
    }
    sp.reversed = dv < 0;
    if (const auto exact = speedFromRatio(dv < 0 ? -dv : dv, dt)) {
        sp.ratio = *exact;
    } else if (const auto rounded = speedFromDouble(std::abs(speed))) {
        sp.ratio = rounded->speed;
        if (!rounded->exact) {
            sp.approximation = "the speed was rounded to " + speedLabel(rounded->speed);
        }
    } else {
        report(ReportOutcome::Approximated, ReportCategory::Speed,
               "'" + name + "' has an unusable speed; imported at 100%", map);
        return std::nullopt;
    }
    return sp;
}

void FcpXmlParser::processMarkers(const pugi::xml_node& item, const TimeContext& ctx,
                                  std::optional<TimeRange> visible, const std::string& clipName) {
    for (const pugi::xml_node m : item.children()) {
        if (!nameIs(m, "marker") && !nameIs(m, "chapter-marker")) {
            continue;
        }
        const std::string value = m.attribute("value").value();
        const auto start = time(m, "start");
        if (!start) {
            report(ReportOutcome::Dropped, ReportCategory::Marker, "marker '" + value + "' has no start", m);
            continue;
        }
        const Ticks at = ctx.map(*start);
        if (visible && !visible->contains(at)) {
            report(ReportOutcome::Dropped, ReportCategory::Marker,
                   "marker '" + value + "' on '" + clipName +
                       "' lies outside the part of the clip in the timeline",
                   m);
            continue;
        }
        IrMarker marker;
        marker.marker.start = at;
        marker.marker.duration = time(m, "duration").value_or(Ticks{});
        if (marker.marker.duration.value < 0) {
            marker.marker.duration = Ticks{};
        }
        marker.marker.name = value;
        marker.marker.comment = m.attribute("note").value();
        // To-do markers are red; completed to-dos green; chapter markers orange; standard markers blue (FCP's
        // colours).
        if (nameIs(m, "chapter-marker")) {
            marker.marker.colorIndex = markerColorFromName("orange");
        } else if (m.attribute("completed")) {
            marker.marker.colorIndex =
                markerColorFromName(attrInt(m, "completed").value_or(0) != 0 ? "green" : "red");
        } else {
            marker.marker.colorIndex = markerColorFromName("blue");
        }
        marker.approximation =
            "marker on '" + clipName + "' became a sequence marker (it no longer moves with the clip)";
        marker.location = m_loc.locate(m);
        m_state->markers.push_back(std::move(marker));
    }
}

void FcpXmlParser::addMediaClip(const pugi::xml_node& node, const Asset& asset, bool video, bool audio,
                                Ticks seqStart, Ticks duration, Ticks localStart, int lane,
                                std::optional<TimeRange> window, std::int64_t linkGroup,
                                const std::string& name) {
    Ticks mediaStart;
    std::optional<IrRemap> remap;
    const std::optional<IrSpeed> speed = speedOf(node, name, localStart, mediaStart, remap);
    // Media time for a stretch of timeline time at the clip's speed (a freeze uses none).
    const auto mediaFor = [&](Ticks t) { return speed ? toSourceTicks(t, speed->ratio) : t; };
    const bool reversed = speed && speed->reversed;
    TimeRange range{seqStart, duration};
    Ticks sourceIn = mediaStart - asset.start; // the media at the clip's first frame
    if (window) {
        // Content of a <clip> shows only inside the clip's own window.
        const Ticks begin = std::max(range.start, window->start);
        const Ticks end = std::min(range.end(), window->end());
        if (end <= begin) {
            return;
        }
        sourceIn += reversed ? -mediaFor(begin - range.start) : mediaFor(begin - range.start);
        if (remap) {
            remap->curveIn += begin - range.start;
            sourceIn = remapSourceAt(remap->curve, remap->curveIn) - asset.start;
        }
        range = TimeRange::fromStartEnd(begin, end);
    }
    if (reversed) {
        // Played backwards from its first frame: the media range starts that much earlier.
        sourceIn -= mediaFor(range.duration);
    }
    const auto properties = propertiesOf(node, name);
    const auto channels = audio ? channelMapOf(node, name) : std::nullopt; // A.9
    const bool enabled = attrInt(node, "enabled").value_or(1) != 0;
    auto make = [&](StreamKind kind) {
        IrClip c;
        c.name = name;
        c.media = asset.media;
        c.stream = {kind, 0};
        c.start = range.start;
        c.duration = range.duration;
        c.sourceIn = sourceIn;
        c.enabled = enabled;
        c.linkGroup = linkGroup;
        if (properties) {
            // One element carries the picture's and the sound's adjustments: each clip takes its own.
            ClipProperties own = *properties;
            const ClipProperties defaults;
            const bool video = kind == StreamKind::Video;
            for (const ParamId id : {ParamId::PositionX, ParamId::PositionY, ParamId::Scale, ParamId::Rotation, ParamId::AnchorX,
                                     ParamId::AnchorY, ParamId::Opacity}) {
                if (!video) {
                    own[id] = defaults[id];
                }
            }
            for (const ParamId id : {ParamId::VolumeDb, ParamId::Pan}) {
                if (video) {
                    own[id] = defaults[id];
                }
            }
            if (!(own == defaults)) {
                c.properties = std::move(own);
            }
        }
        c.speed = speed;
        if (kind == StreamKind::Audio) {
            c.channels = channels;
        }
        if (remap) {
            // Positions relative to the start of the media file, like sourceIn.
            c.remap = remap;
            for (Keyframe& k : c.remap->curve.keys) {
                k.value -= static_cast<double>(asset.start.value);
            }
        }
        c.location = m_loc.locate(node);
        return c;
    };
    if (video) {
        m_doc.media[asset.media].hasVideo = true;
        m_state->videoLanes[lane].clips.push_back(make(StreamKind::Video));
    }
    if (audio) {
        if (m_doc.media[asset.media].audioChannels.empty()) {
            m_doc.media[asset.media].audioChannels.push_back(2);
        }
        m_state->audioLanes[lane].clips.push_back(make(StreamKind::Audio));
        if (node.attribute("audioStart") || node.attribute("audioDuration")) {
            report(ReportOutcome::Approximated, ReportCategory::Clip,
                   "split edit (J/L cut) of '" + name + "': its audio was trimmed to the video's edit points",
                   node);
        }
    }
}

void FcpXmlParser::processAnchored(const pugi::xml_node& item, const TimeContext& ctx, int depth,
                                   bool clipContainer) {
    // Anchored (connected) items and secondary storylines: children with a non-zero lane. Inside a <clip>,
    // <audio> below it is the clip's own sound, handled with the clip.
    for (const pugi::xml_node child : item.children()) {
        const auto lane = attrInt(child, "lane");
        if (!lane || *lane == 0 || (clipContainer && nameIs(child, "audio") && *lane < 0)) {
            continue;
        }
        const int absolute = static_cast<int>(std::clamp<std::int64_t>(ctx.lane + *lane, -1000, 1000));
        if (nameIs(child, "spine")) {
            const auto offset = time(child, "offset").value_or(Ticks{});
            processSpine(child, TimeContext{ctx.map(offset), Ticks{}, absolute}, absolute, depth + 1);
        } else {
            processItem(child, ctx, absolute, depth + 1, std::nullopt, -1, false);
        }
    }
}

void FcpXmlParser::processItem(const pugi::xml_node& item, const TimeContext& ctx, int lane, int depth,
                               std::optional<TimeRange> window, std::int64_t linkGroup, bool insideClip) {
    if (depth > kMaxNesting) {
        report(ReportOutcome::Dropped, ReportCategory::Clip, "nesting too deep; the element was skipped",
               item);
        return;
    }
    const std::string_view tag = item.name();
    const std::string name = item.attribute("name").value();
    const auto offset = time(item, "offset").value_or(Ticks{});
    const auto duration = time(item, "duration");
    const Ticks localStart = time(item, "start").value_or(Ticks{});
    const Ticks seqStart = ctx.map(offset);
    // Children of this element are placed relative to its start.
    const TimeContext inner{seqStart, localStart, lane};

    if (tag == "asset-clip" || tag == "video" || tag == "audio") {
        const auto it = m_assets.find(item.attribute("ref").value());
        if (it == m_assets.end()) {
            const bool generator = m_effects.contains(item.attribute("ref").value());
            report(ReportOutcome::Dropped, ReportCategory::Clip,
                   generator ? "generator '" + name + "' (no media file) is not supported yet"
                             : "'" + name + "' refers to a missing asset",
                   item);
            processAnchored(item, inner, depth);
            return;
        }
        if (!duration || duration->value <= 0) {
            report(ReportOutcome::Dropped, ReportCategory::Clip, "'" + name + "' has no duration", item);
            return;
        }
        const Asset& asset = it->second;
        const std::string clipName =
            !name.empty() ? name : (insideClip && !m_clipName.empty() ? m_clipName : asset.name);
        // Without a start attribute the clip begins at its asset's first frame (the asset's own start).
        const Ticks mediaLocalStart = item.attribute("start") ? localStart : asset.start;
        const TimeContext mediaInner{seqStart, mediaLocalStart, lane};
        const std::string_view enable = attrText(item, "srcEnable");
        bool video = asset.hasVideo && tag != "audio" && enable != "audio";
        bool audio = asset.hasAudio && tag != "video" && enable != "video";
        if (tag == "asset-clip" && !asset.hasVideo && !asset.hasAudio) {
            video = true; // undeclared streams: assume a video clip
        }
        // A video+audio asset-clip is one linked A/V pair.
        std::int64_t group = linkGroup;
        if (video && audio && group < 0) {
            group = m_nextLink++;
        }
        // The audio of a clip inside a <clip> belongs to that clip, whatever its relative lane.
        addMediaClip(item, asset, video, audio, seqStart, *duration, mediaLocalStart, lane, window, group,
                     clipName);
        processMarkers(item, mediaInner, TimeRange{seqStart, *duration}, clipName);
        processAnchored(item, mediaInner, depth);
        return;
    }
    if (tag == "clip" || tag == "sync-clip") {
        if (!duration || duration->value <= 0) {
            report(ReportOutcome::Dropped, ReportCategory::Clip, "'" + name + "' has no duration", item);
            return;
        }
        // The clip's window; its own content (children on lane 0, or audio components below it) shows inside
        // it.
        const TimeRange own{seqStart, *duration};
        const std::int64_t group = linkGroup >= 0 ? linkGroup : m_nextLink++;
        bool any = false;
        const std::string outerName = std::exchange(m_clipName, name);
        for (const pugi::xml_node child : item.children()) {
            const std::string_view childTag = child.name();
            const auto childLane = attrInt(child, "lane").value_or(0);
            const bool content = childTag == "video" || childTag == "audio" || childTag == "asset-clip" ||
                                 childTag == "clip" || childTag == "spine" || childTag == "title";
            if (!content) {
                continue;
            }
            // Audio components of a clip sit at lane -1 relative to it but play as part of the clip.
            const bool component = childLane == 0 || (childTag == "audio" && childLane < 0);
            if (!component) {
                continue;
            }
            any = true;
            if (childTag == "spine") {
                processSpine(child, inner, lane, depth + 1);
            } else {
                processItem(child, inner, lane, depth + 1, own, group, true);
            }
        }
        m_clipName = outerName;
        if (!any) {
            report(ReportOutcome::Dropped, ReportCategory::Clip, "clip '" + name + "' has no media", item);
        }
        if (tag == "sync-clip") {
            report(ReportOutcome::Approximated, ReportCategory::Clip,
                   "synchronized clip '" + name + "' imported as its separate video and audio clips", item);
        }
        processMarkers(item, inner, own, name);
        processAnchored(item, inner, depth, true);
        return;
    }
    if (tag == "gap") {
        if (lane == 0 && duration) {
            m_state->videoLanes[lane].gaps.push_back({{seqStart, *duration}, m_loc.locate(item)});
        }
        processMarkers(item, inner, std::nullopt, "gap");
        processAnchored(item, inner, depth);
        return;
    }
    if (tag == "audition") {
        // The first child is the active pick.
        for (const pugi::xml_node child : item.children()) {
            if (child.type() == pugi::node_element) {
                report(ReportOutcome::Approximated, ReportCategory::Clip,
                       "audition: only the active pick '" + std::string(child.attribute("name").value()) +
                           "' was imported",
                       item);
                processItem(child, ctx, lane, depth + 1, window, linkGroup, insideClip);
                break;
            }
        }
        return;
    }
    if (tag == "ref-clip" || tag == "mc-clip") {
        // C.8: a compound clip is a nested clip of the compound's sequence, a multicam clip a clip of the multicam
        // source with its active angles. Both resources become sequences of their own (after the current one).
        const bool multicam = tag == "mc-clip";
        const std::string ref = item.attribute("ref").value();
        const auto it = m_compounds.find(ref);
        const pugi::xml_node body = it == m_compounds.end() ? pugi::xml_node() : it->second.child(multicam ? "multicam" : "sequence");
        const std::string what = std::string(multicam ? "multicam clip '" : "compound clip '") + name + "'";
        if (!body || depth >= kMaxNesting) {
            report(ReportOutcome::Dropped, ReportCategory::Clip,
                   what + (body ? " is nested too deep" : " refers to a missing resource") +
                       "; its time is left as a gap",
                   item);
            processAnchored(item, inner, depth);
            return;
        }
        if (!duration || duration->value <= 0) {
            report(ReportOutcome::Dropped, ReportCategory::Clip, what + " has no duration", item);
            return;
        }
        if (m_compoundsImported.insert(ref).second) {
            m_pendingCompounds.push_back(it->second);
        }
        if (item.child("timeMap") || item.child("conform-rate")) {
            report(ReportOutcome::Approximated, ReportCategory::Speed,
                   what + " is retimed; it was imported at 100 %", item);
        }
        IrClip c;
        c.name = !name.empty() ? name : std::string(it->second.attribute("name").value());
        c.sequenceKey = "media:" + ref;
        c.start = seqStart;
        c.duration = *duration;
        c.sourceIn = localStart - time(body, "tcStart").value_or(Ticks{});
        c.enabled = attrInt(item, "enabled").value_or(1) != 0;
        c.location = m_loc.locate(item);
        std::vector<std::size_t> videoAngles;
        std::vector<std::size_t> audioAngles;
        if (multicam) {
            std::vector<std::string> ids;
            for (const pugi::xml_node angle : body.children("mc-angle")) {
                ids.emplace_back(angle.attribute("angleID").value());
            }
            for (const pugi::xml_node source : item.children("mc-source")) {
                const auto at = std::find(ids.begin(), ids.end(), source.attribute("angleID").value());
                if (at == ids.end()) {
                    continue;
                }
                const auto index = static_cast<std::size_t>(at - ids.begin());
                const std::string_view enable = attrText(source, "srcEnable");
                if (enable != "audio") {
                    videoAngles = {index};
                }
                if (enable != "video") {
                    audioAngles.push_back(index);
                }
            }
            if (videoAngles.empty() && audioAngles.empty() && !ids.empty()) {
                videoAngles = {0};
                audioAngles = {0};
            }
        }
        const std::string_view enable = attrText(item, "srcEnable");
        const bool video = enable != "audio" && (!multicam || !videoAngles.empty());
        const bool audio = enable != "video" && (!multicam || !audioAngles.empty());
        const std::int64_t group = video && audio && linkGroup < 0 ? m_nextLink++ : linkGroup;
        if (video) {
            IrClip v = c;
            v.stream = {StreamKind::Video, 0};
            v.angles = videoAngles;
            v.linkGroup = group;
            m_state->videoLanes[lane].clips.push_back(std::move(v));
        }
        if (audio) {
            IrClip a = c;
            a.stream = {StreamKind::Audio, 0};
            a.angles = audioAngles;
            a.linkGroup = group;
            m_state->audioLanes[lane].clips.push_back(std::move(a));
        }
        processMarkers(item, inner, TimeRange{seqStart, *duration}, c.name);
        processAnchored(item, inner, depth);
        return;
    }
    if (tag == "title") {
        if (!duration || duration->value <= 0) {
            report(ReportOutcome::Dropped, ReportCategory::Clip, "title '" + name + "' has no duration", item);
        } else {
            addTitle(item, seqStart, *duration, lane, name);
        }
        processMarkers(item, inner, duration ? std::optional<TimeRange>(TimeRange{seqStart, *duration}) : std::nullopt, name);
        processAnchored(item, inner, depth);
        return;
    }
    if (tag == "live-drawing") {
        report(ReportOutcome::Dropped, ReportCategory::Clip, "drawing '" + name + "' is not supported yet", item);
        processAnchored(item, inner, depth);
        return;
    }
    if (tag == "spine") {
        processSpine(item, TimeContext{seqStart, Ticks{}, lane}, lane, depth + 1);
        return;
    }
    if (tag == "transition") {
        return; // handled by processSpine, which knows the neighbours
    }
    if (tag.starts_with("adjust-") || tag.starts_with("filter-") || tag == "marker" ||
        tag == "chapter-marker" || tag == "keyword" || tag == "rating" || tag == "note" ||
        tag == "metadata" || tag == "timeMap" || tag == "conform-rate" || tag == "audio-channel-source" ||
        tag == "audio-role-source" || tag == "analysis-marker") {
        return;
    }
    report(ReportOutcome::Dropped, ReportCategory::Clip, "unsupported element <" + std::string(tag) + ">",
           item);
}

template <class Read>
std::optional<ParamValue> FcpXmlParser::keyframesOf(const pugi::xml_node& adjust, const char* param, ParamId target, Read value,
                                                    const std::string& name) {
    for (const pugi::xml_node p : adjust.children("param")) {
        if (std::string_view(p.attribute("name").value()) != param) {
            continue;
        }
        const pugi::xml_node animation = p.child("keyframeAnimation");
        if (!animation) {
            continue;
        }
        Curve curve;
        bool eased = false;
        for (const pugi::xml_node key : animation.children("keyframe")) {
            const auto t = time(key, "time");
            const auto v = value(std::string_view(key.attribute("value").value()));
            if (!t || !v || curve.keys.size() >= 10'000) {
                continue;
            }
            Keyframe k{*t, *v};
            const std::string_view interp = key.attribute("interp").value();
            if (interp == "hold") {
                k.interpolation = Interpolation::Hold;
            } else if (!interp.empty() && interp != "linear") {
                k.interpolation = Interpolation::EaseInOut;
                eased = true;
            }
            if (const auto at = curve.keyAt(k.time)) {
                curve.keys[*at] = k;
            } else {
                curve.keys.insert(curve.keys.begin() + static_cast<std::ptrdiff_t>(curve.insertionIndex(k.time)), k);
            }
        }
        const ParameterInfo& info = parameterInfo(target);
        ParamValue result(curve);
        if (curve.keys.empty() || !result.isValid(info.minValue, info.maxValue)) {
            report(ReportOutcome::Dropped, ReportCategory::Effect,
                   std::string(param) + " keyframes of '" + name + "' are out of range or malformed; not imported", p);
            return std::nullopt;
        }
        if (eased) {
            report(ReportOutcome::Approximated, ReportCategory::Effect,
                   std::string(param) + " keyframes of '" + name + "' are eased; imported as ease in and out", p);
        }
        return result;
    }
    return std::nullopt;
}

void FcpXmlParser::addTitle(const pugi::xml_node& item, Ticks seqStart, Ticks duration, int lane, const std::string& name) {
    IrTitle title;
    for (const pugi::xml_node run : item.child("text").children("text-style")) {
        title.text += run.text().get();
    }
    // The style of the first run: its text-style-def, inside the title or among the resources' definitions.
    const std::string ref = item.child("text").child("text-style").attribute("ref").value();
    pugi::xml_node style;
    for (const pugi::xml_node def : item.children("text-style-def")) {
        if (ref.empty() || std::string_view(def.attribute("id").value()) == ref) {
            style = def.child("text-style");
            break;
        }
    }
    std::vector<std::string> notCarried;
    if (style) {
        title.fontFamily = style.attribute("font").value();
        title.fontStyle = style.attribute("fontFace").value();
        if (const auto size = parseDouble(attrText(style, "fontSize")); size && *size >= 1.0 && *size <= 2000.0) {
            title.style.push_back(EffectParam{"size", ParamValue(*size)});
        }
        const auto color = splitWords(attrText(style, "fontColor"));
        if (color.size() == 4) {
            const char* channels[] = {"fill.red", "fill.green", "fill.blue", "fill.alpha"};
            for (std::size_t i = 0; i < 4; ++i) {
                if (const auto c = parseDouble(color[i]); c && *c >= 0.0 && *c <= 1.0) {
                    title.style.push_back(EffectParam{channels[i], ParamValue(*c)});
                }
            }
        }
        const std::string_view align = attrText(style, "alignment");
        if (!align.empty()) {
            title.style.push_back(EffectParam{"alignment", ParamValue(align == "left" ? 0.0 : align == "right" ? 2.0 : 1.0)});
        }
        for (const pugi::xml_attribute a : style.attributes()) {
            const std::string_view n = a.name();
            if (n != "font" && n != "fontFace" && n != "fontSize" && n != "fontColor" && n != "alignment") {
                notCarried.emplace_back(n);
            }
        }
    }
    if (title.text.empty()) {
        title.text = name.empty() ? std::string("Title") : name;
    }
    if (!notCarried.empty()) {
        title.approximation = "not carried:";
        for (const std::string& n : notCarried) {
            title.approximation += " " + n;
        }
    }
    std::sort(title.style.begin(), title.style.end(), [](const EffectParam& a, const EffectParam& b) { return a.key < b.key; });
    IrClip c;
    // Untitled titles take their first line, as other importers' generators do.
    c.name = name == "Basic Title" || name == "Title" ? std::string() : name;
    c.stream = {StreamKind::Video, 0};
    c.start = seqStart;
    c.duration = duration;
    c.enabled = attrInt(item, "enabled").value_or(1) != 0;
    c.location = m_loc.locate(item);
    c.title = std::move(title);
    c.properties = propertiesOf(item, name);
    m_state->videoLanes[lane].clips.push_back(std::move(c));
}

void FcpXmlParser::processSpine(const pugi::xml_node& spine, const TimeContext& ctx, int lane, int depth) {
    if (depth > kMaxNesting) {
        report(ReportOutcome::Dropped, ReportCategory::Clip, "storylines nested too deep; skipped", spine);
        return;
    }
    // Transitions sit on the cut between their neighbours, which is where the previous item ends.
    std::optional<Ticks> previousEnd;
    std::vector<pugi::xml_node> transitions;
    for (const pugi::xml_node item : spine.children()) {
        if (item.type() != pugi::node_element) {
            continue;
        }
        if (nameIs(item, "transition")) {
            const auto offset = time(item, "offset");
            const auto duration = time(item, "duration");
            if (!offset || !duration || duration->value <= 0) {
                report(ReportOutcome::Dropped, ReportCategory::Transition,
                       "transition without offset/duration", item);
                continue;
            }
            const Ticks start = ctx.map(*offset);
            const Ticks end = start + *duration;
            IrTransition t;
            t.name = item.attribute("name").value();
            const std::string lower = toLowerAscii(t.name);
            t.kind = lower.find("black") != std::string::npos   ? TransitionKind::DipToBlack
                     : lower.find("white") != std::string::npos ? TransitionKind::DipToWhite
                     : lower.find("wipe") != std::string::npos  ? TransitionKind::Wipe
                     : lower.find("dissolve") != std::string::npos || lower.find("cross") != std::string::npos
                         ? TransitionKind::Dissolve
                         : TransitionKind::Other;
            // At the head of a storyline it fades in from the start; otherwise it straddles the previous cut.
            t.cut = previousEnd && *previousEnd >= start && *previousEnd <= end ? *previousEnd : start;
            t.before = t.cut - start;
            t.after = end - t.cut;
            t.location = m_loc.locate(item);
            m_state->videoLanes[lane].transitions.push_back(t);
            if (item.child("filter-audio")) {
                t.kind = TransitionKind::AudioCrossfade;
                m_state->audioLanes[lane].transitions.push_back(t);
            }
            continue;
        }
        if (attrInt(item, "lane").value_or(0) != 0) {
            processItem(item, ctx, lane + static_cast<int>(*attrInt(item, "lane")), depth + 1, std::nullopt,
                        -1, false);
            continue;
        }
        processItem(item, ctx, lane, depth + 1, std::nullopt, -1, false);
        const auto offset = time(item, "offset");
        const auto duration = time(item, "duration");
        if (offset && duration) {
            previousEnd = ctx.map(*offset) + *duration;
        }
    }
}

IrSequence FcpXmlParser::assembleSequence(SequenceState& state, const pugi::xml_node& node) {
    IrSequence seq;
    seq.name = state.name;
    seq.frameRate = state.rate;
    seq.dropFrame = state.dropFrame;
    seq.location = m_loc.locate(node);
    // Lanes in order become tracks; a lane whose clips overlap is spread over several tracks.
    auto pack = [&](std::map<int, Lane>& lanes, TrackKind kind, std::vector<IrTrack>& out,
                    const std::vector<int>& order) {
        for (const int laneNumber : order) {
            Lane& lane = lanes[laneNumber];
            std::stable_sort(lane.clips.begin(), lane.clips.end(),
                             [](const IrClip& a, const IrClip& b) { return a.start < b.start; });
            std::vector<IrTrack> tracks(1);
            std::vector<Ticks> ends(1, Ticks{std::numeric_limits<std::int64_t>::min()});
            for (IrClip& c : lane.clips) {
                std::size_t t = 0;
                while (t < tracks.size() && ends[t] > c.start) {
                    ++t;
                }
                if (t == tracks.size()) {
                    tracks.emplace_back();
                    ends.push_back(Ticks{std::numeric_limits<std::int64_t>::min()});
                }
                ends[t] = c.start + c.duration;
                tracks[t].clips.push_back(std::move(c));
            }
            tracks.front().transitions = std::move(lane.transitions);
            tracks.front().gaps = std::move(lane.gaps);
            if (tracks.size() > 1) {
                m_doc.report.add(ReportOutcome::Approximated, ReportCategory::Track,
                                 "connected clips of lane " + std::to_string(laneNumber) +
                                     " overlap; they were spread over " + std::to_string(tracks.size()) +
                                     " tracks",
                                 seq.location, state.name);
            }
            for (IrTrack& t : tracks) {
                t.kind = kind;
                t.location = seq.location;
                out.push_back(std::move(t));
            }
        }
    };
    std::vector<int> videoOrder;
    for (const auto& [laneNumber, lane] : state.videoLanes) {
        if (!lane.clips.empty() || laneNumber == 0) {
            videoOrder.push_back(
                laneNumber); // map order: below-primary lanes first, so they composite under it
        }
    }
    // Audio: the primary storyline first, then lanes below it, then the audio of connected clips above.
    std::vector<int> audioOrder;
    if (state.audioLanes.contains(0)) {
        audioOrder.push_back(0);
    }
    for (auto it = state.audioLanes.rbegin(); it != state.audioLanes.rend(); ++it) {
        if (it->first < 0 && !it->second.clips.empty()) {
            audioOrder.push_back(it->first);
        }
    }
    for (const auto& [laneNumber, lane] : state.audioLanes) {
        if (laneNumber > 0 && !lane.clips.empty()) {
            audioOrder.push_back(laneNumber);
        }
    }
    pack(state.videoLanes, TrackKind::Video, seq.videoTracks, videoOrder);
    pack(state.audioLanes, TrackKind::Audio, seq.audioTracks, audioOrder);
    seq.markers = std::move(state.markers);
    std::sort(seq.markers.begin(), seq.markers.end(),
              [](const IrMarker& a, const IrMarker& b) { return a.marker.start < b.marker.start; });
    return seq;
}

std::expected<void, Error> FcpXmlParser::parseProject(const pugi::xml_node& project,
                                                      const pugi::xml_node& sequence, const std::string& name,
                                                      int depth) {
    SequenceState state;
    state.name = name.empty() ? "Project " + std::to_string(m_doc.sequences.size() + 1) : name;
    m_state = &state;
    const auto format = m_formats.find(sequence.attribute("format").value());
    if (format != m_formats.end() && format->second.rate) {
        state.rate = *format->second.rate;
    } else {
        report(ReportOutcome::Approximated, ReportCategory::SequenceSetting,
               "the project's format has no frame rate; 30 fps assumed", sequence);
    }
    state.dropFrame = iequals(attrText(sequence, "tcFormat"), "DF") && supportsDropFrame(state.rate);
    if (format != m_formats.end() && format->second.height > 0) {
        state.height = format->second.height;
    }
    // X.1: the sequence keeps its start timecode (Sequence::startTimecode); times count from it.
    const Ticks tcStart = time(sequence, "tcStart").value_or(Ticks{});
    const pugi::xml_node spine = sequence.child("spine");
    if (!spine) {
        m_state = nullptr;
        return std::unexpected(parseError("project '" + state.name + "' has no spine"));
    }
    processSpine(spine, TimeContext{Ticks{}, tcStart, 0}, 0, depth);
    if (!state.videoLanes.contains(0)) {
        state.videoLanes[0];
    }
    if (!state.audioLanes.contains(0)) {
        state.audioLanes[0];
    }
    IrSequence seq = assembleSequence(state, project);
    if (format != m_formats.end()) {
        seq.width = format->second.width > 0 ? format->second.width : 1920;
        seq.height = format->second.height > 0 ? format->second.height : 1080;
    }
    std::string_view audioRate = attrText(sequence, "audioRate");
    if (!audioRate.empty()) {
        double khz = 0.0;
        if (iendsWith(audioRate, "k")) {
            khz = parseDouble(audioRate.substr(0, audioRate.size() - 1)).value_or(0.0);
        } else {
            khz = parseDouble(audioRate).value_or(0.0) / 1000.0;
        }
        if (khz > 0.0 && khz < 1000.0) {
            seq.audioRate = Rate{static_cast<std::int32_t>(std::lround(khz * 1000.0)), 1};
        }
    }
    if (const auto duration = time(sequence, "duration")) {
        seq.duration = *duration;
    }
    if (tcStart.value > 0) {
        seq.startTimecode = tcStart;
    }
    if (m_inexact) {
        report(ReportOutcome::Approximated, ReportCategory::Clip,
               "some times fell between ticks and were rounded to the nearest tick", sequence);
        m_inexact = false;
    }
    m_doc.sequences.push_back(std::move(seq));
    m_state = nullptr;
    return {};
}

void FcpXmlParser::parseMulticam(const pugi::xml_node& media) {
    const pugi::xml_node multicam = media.child("multicam");
    IrSequence seq;
    seq.name = media.attribute("name").value();
    if (seq.name.empty()) {
        seq.name = "Multicam " + std::to_string(m_doc.sequences.size() + 1);
    }
    seq.location = m_loc.locate(media);
    const auto format = m_formats.find(multicam.attribute("format").value());
    if (format != m_formats.end()) {
        if (format->second.rate) {
            seq.frameRate = *format->second.rate;
        }
        seq.width = format->second.width > 0 ? format->second.width : 1920;
        seq.height = format->second.height > 0 ? format->second.height : 1080;
    }
    const Ticks tcStart = time(multicam, "tcStart").value_or(Ticks{});
    // FCPXML places each angle's clips where they sync; the sync method itself is not recorded.
    IrMulticam info;
    info.sync = MulticamSync::Manual;
    Ticks end;
    for (const pugi::xml_node angle : multicam.children("mc-angle")) {
        SequenceState state;
        state.name = seq.name;
        state.rate = seq.frameRate;
        m_state = &state;
        processSpine(angle, TimeContext{Ticks{}, tcStart, 0}, 0, 1);
        m_state = nullptr;
        IrMulticamAngle a;
        a.name = angle.attribute("name").value();
        Ticks first = Ticks::max();
        for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
            auto& lanes = kind == TrackKind::Video ? state.videoLanes : state.audioLanes;
            for (auto& [number, lane] : lanes) {
                if (number != 0) {
                    if (!lane.clips.empty()) {
                        report(ReportOutcome::Dropped, ReportCategory::Clip,
                               "connected clips in angle '" + a.name + "' of multicam '" + seq.name +
                                   "' are not carried",
                               angle);
                    }
                    continue;
                }
                if (lane.clips.empty()) {
                    continue;
                }
                IrTrack track;
                track.kind = kind;
                track.name = a.name;
                track.location = m_loc.locate(angle);
                std::stable_sort(lane.clips.begin(), lane.clips.end(),
                                 [](const IrClip& x, const IrClip& y) { return x.start < y.start; });
                for (IrClip& c : lane.clips) {
                    c.linkGroup = -1; // links inside an angle are not needed: the angle plays its tracks together
                    first = std::min(first, c.start);
                    end = std::max(end, c.start + c.duration);
                    track.clips.push_back(std::move(c));
                }
                track.transitions = std::move(lane.transitions);
                if (kind == TrackKind::Video) {
                    a.videoTrack = seq.videoTracks.size();
                    seq.videoTracks.push_back(std::move(track));
                } else {
                    a.audioTracks.push_back(seq.audioTracks.size());
                    seq.audioTracks.push_back(std::move(track));
                }
            }
        }
        a.offset = first == Ticks::max() ? Ticks{} : std::max(first, Ticks{});
        info.angles.push_back(std::move(a));
    }
    if (info.angles.empty()) {
        report(ReportOutcome::Dropped, ReportCategory::Clip, "multicam '" + seq.name + "' has no angles", media);
        return;
    }
    seq.duration = end;
    seq.multicam = std::move(info);
    m_doc.sequences.push_back(std::move(seq));
}

std::expected<void, Error> FcpXmlParser::parse(const pugi::xml_document& xml) {
    const pugi::xml_node root = xml.document_element();
    if (!nameIs(root, "fcpxml")) {
        return std::unexpected(
            parseError("the root element is <" + std::string(root.name()) + ">, not <fcpxml>"));
    }
    readResources(root.child("resources"));
    // Projects can sit under <library>/<event>, under <event>, or directly under <fcpxml>.
    std::vector<pugi::xml_node> projects;
    std::vector<pugi::xml_node> events;
    std::vector<pugi::xml_node> stack{root};
    while (!stack.empty()) {
        const pugi::xml_node node = stack.back();
        stack.pop_back();
        for (pugi::xml_node child = node.last_child(); child; child = child.previous_sibling()) {
            if (nameIs(child, "project")) {
                projects.push_back(child);
            } else if (nameIs(child, "library") || nameIs(child, "event")) {
                stack.push_back(child);
                if (nameIs(child, "event")) {
                    events.push_back(child);
                }
            }
        }
    }
    std::reverse(projects.begin(), projects.end());
    std::reverse(events.begin(), events.end());
    // X.1: events are bins. An event's own clips (its browser) put their assets in its bin, or in the bin of their
    // first keyword collection inside it (an asset is in one bin: further keywords are reported).
    for (const pugi::xml_node& event : events) {
        const std::string eventName = event.attribute("name").value();
        if (eventName.empty()) {
            continue;
        }
        std::set<std::string> collections;
        for (const pugi::xml_node k : event.children("keyword-collection")) {
            collections.emplace(k.attribute("name").value());
        }
        for (const pugi::xml_node clip : event.children()) {
            const std::string ref = clip.attribute("ref").value();
            const auto asset = m_assets.find(ref);
            if (ref.empty() || !(nameIs(clip, "asset-clip") || nameIs(clip, "clip")) || asset == m_assets.end() ||
                asset->second.media == kNoMedia || m_assetEvents.contains(ref)) {
                continue;
            }
            m_assetEvents.emplace(ref, eventName);
            std::vector<std::string> keywords;
            for (const pugi::xml_node k : clip.children("keyword")) {
                std::string_view value = k.attribute("value").value();
                while (!value.empty()) {
                    const std::size_t comma = value.find(',');
                    const std::string word(trim(value.substr(0, comma)));
                    if (collections.contains(word) &&
                        std::find(keywords.begin(), keywords.end(), word) == keywords.end()) {
                        keywords.push_back(word);
                    }
                    value = comma == std::string_view::npos ? std::string_view() : value.substr(comma + 1);
                }
            }
            IrMedia& media = m_doc.media[asset->second.media];
            media.bin = {eventName};
            if (!keywords.empty()) {
                media.bin.push_back(keywords.front());
            }
            if (keywords.size() > 1) {
                report(ReportOutcome::Approximated, ReportCategory::Media,
                       "'" + media.name + "' is in the keyword collections of " + std::to_string(keywords.size()) +
                           " keywords; it went into the bin of the first ('" + keywords.front() + "')",
                       clip);
            }
        }
    }
    for (const pugi::xml_node& project : projects) {
        const pugi::xml_node sequence = project.child("sequence");
        if (!sequence) {
            report(ReportOutcome::Dropped, ReportCategory::SequenceSetting,
                   "project '" + std::string(project.attribute("name").value()) + "' has no sequence",
                   project);
            continue;
        }
        if (auto r = parseProject(project, sequence, project.attribute("name").value(), 0); !r) {
            return r;
        }
        if (const pugi::xml_node event = project.parent(); nameIs(event, "event") && !m_doc.sequences.empty()) {
            const std::string eventName = event.attribute("name").value();
            if (!eventName.empty()) {
                m_doc.sequences.back().bin = {eventName}; // X.1: the event is the project's bin
            }
        }
        // Compound clips and multicam sources used by this project become sequences of their own, named by their
        // resource id (the nested and multicam clips refer to them by that key).
        while (!m_pendingCompounds.empty()) {
            const pugi::xml_node media = m_pendingCompounds.front();
            m_pendingCompounds.erase(m_pendingCompounds.begin());
            const std::size_t before = m_doc.sequences.size();
            if (const pugi::xml_node inner = media.child("sequence")) {
                if (auto r = parseProject(media, inner, media.attribute("name").value(), 1); !r) {
                    return r;
                }
            } else if (media.child("multicam")) {
                parseMulticam(media);
            }
            if (m_doc.sequences.size() > before) {
                m_doc.sequences.back().key = "media:" + std::string(media.attribute("id").value());
            }
        }
    }
    if (m_doc.sequences.empty()) {
        return std::unexpected(parseError("the FCPXML contains no projects (timelines)"));
    }
    return {};
}

} // namespace

std::expected<IrDocument, Error> parseFcpXml(std::string_view data, const ParseContext& context) {
    pugi::xml_document xml;
    if (auto parsed = parseXml(data, xml, "the FCPXML file"); !parsed) {
        return std::unexpected(parsed.error());
    }
    IrDocument doc;
    FcpXmlParser parser(data, context, doc);
    if (auto r = parser.parse(xml); !r) {
        return std::unexpected(r.error());
    }
    return doc;
}

} // namespace wabcut::interchange::detail
