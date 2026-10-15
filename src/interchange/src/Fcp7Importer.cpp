// Final Cut Pro 7 XML (xmeml v1-v5), the "Final Cut Pro XML" Premiere Pro and DaVinci Resolve export.
//
//   <xmeml version="4"><sequence id="..."> <name/> <rate><timebase>30</timebase><ntsc>TRUE</ntsc></rate>
//     <timecode><displayformat>DF</displayformat>...</timecode> <in/> <out/> <marker/>...
//     <media><video><format><samplecharacteristics><width/><height/>...</samplecharacteristics></format>
//              <track><enabled/><locked/>
//                <clipitem id=""> <name/> <enabled/> <rate/> <start/> <end/> <in/> <out/> [<pproTicksIn/>]
//                  <file id=""> <name/> <pathurl/> <rate/> <duration/> <media><video/><audio/>...</media>
//                  </file> <sourcetrack><mediatype/><trackindex/></sourcetrack>
//                  <link><linkclipref/>...</link>
//                  <filter><effect><effectid>timeremap</effectid><parameter>speed...</parameter></effect></filter>
//                <transitionitem> <start/> <end/> <alignment/> <effect><name/>...</effect>
//            <audio><track>... (Premiere writes a stereo track as two "exploded" mono tracks)
//
// Times are frames at the sequence's <rate>; clip in/out are frames at the clip's <rate>. A clip next to a
// transition has <start> or <end> -1: the transition's alignment then gives the edit point.

#include "EffectMap.h"
#include "ImporterCommon.h"
#include "Importers.h"
#include "TimeMath.h"
#include "XmlUtil.h"

#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace wabcut::interchange::detail {

namespace {

bool nameIs(const pugi::xml_node& n, const char* name) {
    return std::strcmp(n.name(), name) == 0;
}

/// Union-find over clipitem ids, for <link> groups.
class LinkSets {
public:
    std::size_t idOf(const std::string& key) {
        const auto [it, inserted] = m_ids.emplace(key, m_parent.size());
        if (inserted) {
            m_parent.push_back(m_parent.size());
        }
        return it->second;
    }
    std::size_t find(std::size_t x) {
        while (m_parent[x] != x) {
            m_parent[x] = m_parent[m_parent[x]];
            x = m_parent[x];
        }
        return x;
    }
    void unite(const std::string& a, const std::string& b) {
        const std::size_t ra = find(idOf(a));
        const std::size_t rb = find(idOf(b));
        if (ra != rb) {
            m_parent[rb] = ra;
        }
    }
    std::optional<std::size_t> rootOf(const std::string& key) {
        const auto it = m_ids.find(key);
        return it == m_ids.end() ? std::nullopt : std::optional<std::size_t>(find(it->second));
    }

private:
    std::unordered_map<std::string, std::size_t> m_ids;
    std::vector<std::size_t> m_parent;
};

/// One <clipitem>/<transitionitem>/<generatoritem> of a track and its resolved edit points.
struct TrackItem {
    enum class Kind { Clip, Transition, Generator } kind = Kind::Clip;
    pugi::xml_node node;
    std::int64_t start = -1; // sequence frames as written; -1 = given by an adjacent transition
    std::int64_t end = -1;
    // Resolved, in ticks: a clip's range between its edit points; a transition's extent and cut.
    std::optional<Ticks> startTicks;
    std::optional<Ticks> endTicks;
    std::optional<Ticks> cutTicks;
    /// For a clip whose head is in a transition: timeline time from the transition's start to the cut.
    /// The clip's <in> is the frame at the transition's start, so its media In moves on by this much.
    Ticks headHandle;
};

/// A keyframe of the Time Remap "graphdict" (time graph): at clip frame `when` (the frame space of the clip's
/// <in>/<out>), the media frame `value`.
struct GraphKey {
    double when = 0.0;
    double value = 0.0;
    bool smooth = false; ///< a curved (Bezier) key rather than a corner
};

struct Remap {
    double percent = 100.0;
    bool reverse = false;
    bool variable = false;
    /// Media frame of the clip's In from the speed graph's speedkfin keyframe (clip rate), if present.
    std::optional<std::int64_t> mediaInFrame;
    std::vector<GraphKey> graph;

    double factor() const { return percent > 0.0 ? percent / 100.0 : 1.0; }
};

class Fcp7Parser {
public:
    Fcp7Parser(std::string_view data, const ParseContext& context, IrDocument& doc)
        : m_ctx(context), m_doc(doc), m_media(doc), m_loc(data, context.fileLabel) {}

    std::expected<void, Error> parse(const pugi::xml_document& xml);

private:
    struct SequenceState {
        IrSequence seq;
        Rate rate = kRate30;
        bool dropFrame = false;
        LinkSets links;
        /// clipitem id -> (video?, track index, clip index) of the IR clip it became.
        std::map<std::string, std::tuple<bool, std::size_t, std::size_t>> clipsById;
        /// Audio clips playing one channel of a multichannel stream.
        struct ChannelClip {
            std::size_t track;
            std::size_t clip;
            int channel;
            int channels;
        };
        std::vector<ChannelClip> channelClips;
        std::string name;
    };

    void report(ReportOutcome o, ReportCategory c, std::string reason, const pugi::xml_node& node,
                const std::string& sequence) {
        m_doc.report.add(o, c, std::move(reason), m_loc.locate(node), sequence);
    }

    void collectFiles(const pugi::xml_node& root);
    std::vector<pugi::xml_node> topLevelSequences(const pugi::xml_node& root) const;
    std::optional<std::size_t> parseSequence(const pugi::xml_node& node, int depth,
                                             const std::vector<std::string>& bin = {});
    /// The bins an element sits in (<bin><name> of its ancestors), outermost first (X.1).
    static std::vector<std::string> binPathOf(const pugi::xml_node& node);
    /// `explodedChannels`: channel count of the Premiere multichannel track this XML track starts (1 = mono).
    void parseTrack(SequenceState& state, const pugi::xml_node& trackNode, TrackKind kind, std::size_t index,
                    int depth, IrTrack* mergeInto, std::int64_t explodedChannels = 1);
    /// A <multiclip> as a multicam source sequence (C.8), once per multiclip id: its slot.
    std::optional<std::size_t> parseMulticlip(const pugi::xml_node& multiclip, Rate rate, int depth);
    /// A nested-sequence or multiclip clipitem as an IR clip playing `slot` (C.8).
    void addNestedClip(SequenceState& state, const TrackItem& item, IrTrack& track, TrackKind kind, std::size_t index,
                       std::size_t slot, std::vector<std::size_t> angles, const std::string& name,
                       const std::string& label);
    std::optional<Ticks> frames(std::int64_t count, Rate rate) const {
        return framesToTicksChecked(count, rate);
    }
    std::size_t mediaForFile(const pugi::xml_node& fileRef, const pugi::xml_node& clipNode);
    std::optional<Rate> parseRate(const pugi::xml_node& rateNode) const;
    Remap speedOf(const pugi::xml_node& clip) const;
    std::optional<IrTitle> titleOf(const pugi::xml_node& generator, const std::string& name) const;
    /// Motion, opacity and audio levels as clip properties; Gaussian Blur, Crop, Drop Shadow and Wabcut's own
    /// effects into `effects` (X.1).
    std::optional<ClipProperties> propertiesOf(SequenceState& state, const pugi::xml_node& clip,
                                               const std::string& name, std::vector<EffectInstance>& effects);
    /// Sequence markers, or (`clipStart` set) a clip's markers: of its source media when `media` is known (X.1).
    void parseMarkers(SequenceState& state, const pugi::xml_node& owner, std::optional<Ticks> clipStart,
                      std::optional<Ticks> clipIn, std::optional<Ticks> clipDuration, Rate rate,
                      const std::string& clipName, std::size_t media = kNoMedia);
    StreamRef audioStreamFor(std::size_t mediaIndex, std::int64_t trackIndex, int& channel,
                             int& channelsInStream);
    void resolveEdges(std::vector<TrackItem>& items, SequenceState& state);
    void mergeStereoPairs(SequenceState& state);

    [[maybe_unused]] const ParseContext& m_ctx; // not read yet (Clang: -Wunused-private-field)
    IrDocument& m_doc;
    MediaTable m_media;
    XmlLocator m_loc;
    std::unordered_map<std::string, pugi::xml_node> m_files;
    /// File id -> the bins of the master clip (<bin>/<children>/<clip>) that plays it (X.1).
    std::unordered_map<std::string, std::vector<std::string>> m_fileBins;
    std::unordered_map<std::string, std::size_t> m_sequenceSlots;
    std::int64_t m_nextLink = 0;
};

void Fcp7Parser::collectFiles(const pugi::xml_node& root) {
    // Iterative walk: a <file id> with content defines the file; later <file id="..."/> refer to it.
    std::vector<pugi::xml_node> stack{root};
    while (!stack.empty()) {
        const pugi::xml_node node = stack.back();
        stack.pop_back();
        for (pugi::xml_node child = node.last_child(); child; child = child.previous_sibling()) {
            if (child.type() != pugi::node_element) {
                continue;
            }
            if (nameIs(child, "file")) {
                const std::string id = child.attribute("id").value();
                if (!id.empty() && child.first_child() && !m_files.contains(id)) {
                    m_files.emplace(id, child);
                }
                // A master clip in a bin: its file belongs there.
                if (!id.empty() && nameIs(node, "clip") && !m_fileBins.contains(id)) {
                    auto path = binPathOf(child);
                    if (!path.empty()) {
                        m_fileBins.emplace(id, std::move(path));
                    }
                }
            }
            stack.push_back(child);
        }
    }
}

std::vector<pugi::xml_node> Fcp7Parser::topLevelSequences(const pugi::xml_node& root) const {
    // Sequences directly under <xmeml>, or inside <project>/<bin>/<children> containers (not inside clips).
    std::vector<pugi::xml_node> out;
    std::vector<pugi::xml_node> stack{root};
    while (!stack.empty()) {
        const pugi::xml_node node = stack.back();
        stack.pop_back();
        std::vector<pugi::xml_node> children;
        for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling()) {
            if (child.type() != pugi::node_element) {
                continue;
            }
            if (nameIs(child, "sequence")) {
                out.push_back(child);
            } else if (nameIs(child, "project") || nameIs(child, "bin") || nameIs(child, "children")) {
                children.push_back(child);
            }
        }
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            stack.push_back(*it);
        }
    }
    return out;
}

std::optional<Rate> Fcp7Parser::parseRate(const pugi::xml_node& rateNode) const {
    if (!rateNode) {
        return std::nullopt;
    }
    const auto timebase = childInt(rateNode, "timebase");
    if (!timebase) {
        return std::nullopt;
    }
    return rateFromTimebase(*timebase, childBool(rateNode, "ntsc").value_or(false));
}

Remap Fcp7Parser::speedOf(const pugi::xml_node& clip) const {
    Remap speed;
    for (const pugi::xml_node filter : clip.children("filter")) {
        const pugi::xml_node effect = filter.child("effect");
        if (!iequals(childText(effect, "effectid"), "timeremap") &&
            !iequals(childText(effect, "name"), "Time Remap")) {
            continue;
        }
        for (const pugi::xml_node p : effect.children("parameter")) {
            const std::string_view id = childText(p, "parameterid");
            if (iequals(id, "speed")) {
                speed.percent = childDouble(p, "value").value_or(100.0);
            } else if (iequals(id, "reverse")) {
                speed.reverse = childBool(p, "value").value_or(false);
            } else if (iequals(id, "variablespeed")) {
                speed.variable = childDouble(p, "value").value_or(0.0) != 0.0;
            } else if (iequals(id, "graphdict")) {
                // "For the in point in the media, use the setting of value in the speedkfin keyframe."
                for (const pugi::xml_node key : p.children("keyframe")) {
                    if (childBool(key, "speedkfin").value_or(false)) {
                        if (const auto v = childDouble(key, "value")) {
                            speed.mediaInFrame = std::llround(*v);
                        }
                    }
                    const auto when = childDouble(key, "when");
                    const auto value = childDouble(key, "value");
                    if (when && value) {
                        const std::string_view interpolation = childText(key.child("interpolation"), "name");
                        const bool smooth = !interpolation.empty() && !iequals(interpolation, "FCPLinear") &&
                                            !iequals(interpolation, "linear");
                        speed.graph.push_back(GraphKey{*when, *value, smooth || key.child("inscale") ||
                                                                          key.child("outscale")});
                    }
                }
            }
        }
    }
    if (speed.percent < 0.0) {
        speed.reverse = true;
        speed.percent = -speed.percent;
    }
    return speed;
}

/// FCP7's Text and Outline Text generators (Apple's FCP XML reference, "Generator Effects"): parameter ids str,
/// fontname, fontsize, fontstyle (1 plain, 2 bold, 3 italic, 4 bold italic), fontalign (1 left, 2 centre,
/// 3 right), fontcolor / textcolor ({alpha, red, green, blue} 0..255), and for Outline Text linewidth and
/// linecolor. Parameters are matched by id, else by display name.
std::optional<IrTitle> Fcp7Parser::titleOf(const pugi::xml_node& generator, const std::string& name) const {
    const pugi::xml_node effect = generator.child("effect");
    const std::string id = toLowerAscii(childText(effect, "effectid"));
    const std::string effectName = toLowerAscii(childText(effect, "name"));
    const bool outline = id == "outline text" || id == "outlinetext" || effectName == "outline text";
    if (!outline && id != "text" && effectName != "text") {
        return std::nullopt;
    }
    IrTitle title;
    std::vector<std::string> notCarried;
    const auto color = [](const pugi::xml_node& value, const char* prefix, std::vector<EffectParam>& style) {
        for (const char* channel : {"red", "green", "blue", "alpha"}) {
            if (const auto v = childDouble(value, channel)) {
                style.push_back(EffectParam{std::string(prefix) + "." + channel,
                                            ParamValue(std::clamp(*v / 255.0, 0.0, 1.0))});
            }
        }
    };
    for (const pugi::xml_node p : effect.children("parameter")) {
        const std::string pid = toLowerAscii(childText(p, "parameterid"));
        const std::string pname = toLowerAscii(childText(p, "name"));
        const auto is = [&](std::string_view a, std::string_view b) { return pid == a || pname == b; };
        const pugi::xml_node value = p.child("value");
        const auto number = childDouble(p, "value");
        if (is("str", "text")) {
            std::string text(value.text().get());
            // FCP7 separates lines with carriage returns.
            std::string normalized;
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '\r') {
                    normalized += '\n';
                    if (i + 1 < text.size() && text[i + 1] == '\n') {
                        ++i;
                    }
                } else {
                    normalized += text[i];
                }
            }
            title.text = std::move(normalized);
        } else if (is("fontname", "font")) {
            title.fontFamily = std::string(childText(p, "value"));
        } else if (is("fontsize", "size")) {
            if (number && *number >= 1.0 && *number <= 2000.0) {
                title.style.push_back(EffectParam{"size", ParamValue(*number)});
            }
        } else if (is("fontstyle", "style")) {
            const auto v = number ? std::llround(*number) : 1;
            title.fontStyle = v == 2 ? "Bold" : v == 3 ? "Italic" : v == 4 ? "Bold Italic" : "Regular";
        } else if (is("fontalign", "alignment")) {
            if (number && *number >= 1.0 && *number <= 3.0) {
                title.style.push_back(EffectParam{"alignment", ParamValue(static_cast<double>(std::llround(*number) - 1))});
            }
        } else if (is("fontcolor", "font color") || is("textcolor", "text color")) {
            color(value, "fill", title.style);
        } else if (outline && is("linewidth", "line width")) {
            if (number && *number >= 0.0 && *number <= 500.0) {
                title.style.push_back(EffectParam{"stroke.width", ParamValue(*number)});
            }
        } else if (outline && is("linecolor", "line color")) {
            color(value, "stroke", title.style);
        } else if (is("autokern", "auto kerning") || is("subpixel", "use subpixel")) {
            // Rendering hints, not part of the look.
        } else if ((is("origin", "origin") && childDouble(value, "horiz").value_or(0.0) == 0.0 &&
                    childDouble(value, "vert").value_or(0.0) == 0.0) ||
                   (is("leading", "leading") && number.value_or(0.0) == 0.0) ||
                   (is("fonttrack", "tracking") && (number.value_or(1.0) == 1.0 || number.value_or(1.0) == 0.0)) ||
                   (is("aspect", "aspect") && number.value_or(1.0) == 1.0)) {
            // At their defaults: nothing to carry.
        } else {
            const std::string label(childText(p, "name"));
            notCarried.push_back(label.empty() ? pid : label);
        }
    }
    if (title.text.empty()) {
        title.text = name;
    }
    if (!notCarried.empty()) {
        title.approximation = "not carried: ";
        for (std::size_t i = 0; i < notCarried.size(); ++i) {
            title.approximation += (i ? ", " : "") + notCarried[i];
        }
    }
    // Keys sorted and unique for the style effect.
    std::sort(title.style.begin(), title.style.end(), [](const EffectParam& a, const EffectParam& b) { return a.key < b.key; });
    title.style.erase(std::unique(title.style.begin(), title.style.end(),
                                  [](const EffectParam& a, const EffectParam& b) { return a.key == b.key; }),
                      title.style.end());
    return title;
}

std::optional<ClipProperties> Fcp7Parser::propertiesOf(SequenceState& state, const pugi::xml_node& clip,
                                                       const std::string& name, std::vector<EffectInstance>& effects) {
    ClipProperties props;
    bool any = false;
    for (const pugi::xml_node filter : clip.children("filter")) {
        const pugi::xml_node effect = filter.child("effect");
        const std::string id = toLowerAscii(childText(effect, "effectid"));
        const std::string effectName(childText(effect, "name"));
        if (id == "timeremap" || iequals(effectName, "Time Remap")) {
            continue; // handled as speed
        }
        const bool disabled = childBool(filter, "enabled") == false || childBool(effect, "enabled") == false;
        const std::string originalId(childText(effect, "effectid"));
        const NativeEffect* native = nativeEffectFromFcp(id, effectName);
        const EffectDescriptor* own =
            originalId.starts_with("wabcut.") ? EffectRegistry::builtin().find(originalId) : nullptr;
        if (disabled && !native && !own) {
            report(ReportOutcome::Dropped, ReportCategory::Effect,
                   "disabled effect '" + effectName + "' on '" + name + "' was not imported", effect,
                   state.name);
            continue;
        }
        // A parameter's value: a constant, or keyframes (X.1) as a curve whose times are the <when> frames (the
        // clip's media frames, the space of <in>/<out>). `convert` maps FCP's units to the model's.
        const Rate clipRate = parseRate(clip.child("rate")).value_or(state.rate);
        bool keyed = false;
        auto valueIn = [&](std::string_view parameterId, double minValue, double maxValue,
                           const auto& convert) -> std::optional<ParamValue> {
            for (const pugi::xml_node p : effect.children("parameter")) {
                if (!iequals(childText(p, "parameterid"), parameterId)) {
                    continue;
                }
                struct {
                    double minValue;
                    double maxValue;
                } info{minValue, maxValue};
                if (p.child("keyframe")) {
                    Curve curve;
                    bool eased = false;
                    for (const pugi::xml_node key : p.children("keyframe")) {
                        const auto when = childDouble(key, "when");
                        const auto v = childDouble(key, "value");
                        if (!when || !v || !std::isfinite(*when) || std::abs(*when) > 1e12 || curve.keys.size() >= 10'000) {
                            continue;
                        }
                        const auto time = frames(std::llround(*when), clipRate);
                        if (!time) {
                            continue;
                        }
                        Keyframe k{*time, convert(*v)};
                        const std::string interp = toLowerAscii(childText(key.child("interpolation"), "name"));
                        if (interp.find("hold") != std::string::npos) {
                            k.interpolation = Interpolation::Hold;
                        } else if (!interp.empty() && interp != "linear" && interp != "fcplinear") {
                            k.interpolation = Interpolation::EaseInOut;
                            eased = true;
                        }
                        if (const auto at = curve.keyAt(k.time)) {
                            curve.keys[*at] = k;
                        } else {
                            curve.keys.insert(curve.keys.begin() + static_cast<std::ptrdiff_t>(curve.insertionIndex(k.time)), k);
                        }
                    }
                    ParamValue result(curve);
                    if (curve.keys.empty() || !result.isValid(info.minValue, info.maxValue)) {
                        report(ReportOutcome::Dropped, ReportCategory::Effect,
                               "'" + effectName + "' " + std::string(parameterId) + " keyframes on '" + name +
                                   "' are out of range or malformed; they were not imported",
                               p, state.name);
                        return std::nullopt;
                    }
                    if (eased) {
                        report(ReportOutcome::Approximated, ReportCategory::Effect,
                               "'" + effectName + "' " + std::string(parameterId) + " on '" + name +
                                   "' has curved keyframes; they were imported as ease in and out",
                               p, state.name);
                    }
                    keyed = true;
                    return result;
                }
                const auto v = childDouble(p, "value");
                if (!v) {
                    return std::nullopt;
                }
                const double converted = convert(*v);
                if (!ParamValue(converted).isValid(info.minValue, info.maxValue)) {
                    return std::nullopt;
                }
                return ParamValue(converted);
            }
            return std::nullopt;
        };
        const auto value = [&](const char* parameterId, ParamId target, const auto& convert) {
            return valueIn(parameterId, parameterInfo(target).minValue, parameterInfo(target).maxValue, convert);
        };
        const auto same = [](double v) { return v; };
        const auto percent = [](double v) { return v / 100.0; };
        // FCP's audio level is a linear gain (1 = 0 dB).
        const auto decibels = [](double v) { return v > 0.0 ? 20.0 * std::log10(v) : -96.0; };
        bool understood = true;
        const auto set = [&](ParamId target, std::optional<ParamValue> v) {
            if (v) {
                props[target] = std::move(*v);
                any = true;
            }
        };
        if (id == "opacity") {
            set(ParamId::Opacity, value("opacity", ParamId::Opacity, percent));
        } else if (id == "basic") {
            set(ParamId::Scale, value("scale", ParamId::Scale, percent));
            set(ParamId::Rotation, value("rotation", ParamId::Rotation, same));
            // Center and anchor: points as fractions of the frame from its middle (X.1).
            for (const pugi::xml_node p : effect.children("parameter")) {
                const std::string_view pid = childText(p, "parameterid");
                const bool center = iequals(pid, "center");
                if (!center && !iequals(pid, "centerOffset")) {
                    continue;
                }
                const pugi::xml_node v = p.child("value");
                const double h = childDouble(v, "horiz").value_or(0.0);
                const double vert = childDouble(v, "vert").value_or(0.0);
                if (p.child("keyframe")) {
                    report(ReportOutcome::Approximated, ReportCategory::Effect,
                           std::string(center ? "position" : "anchor point") + " keyframes of '" + name +
                               "' were not imported; the static value is used",
                           p, state.name);
                }
                const double x = h * state.seq.width;
                const double y = vert * state.seq.height;
                const ParamId px = center ? ParamId::PositionX : ParamId::AnchorX;
                const ParamId py = center ? ParamId::PositionY : ParamId::AnchorY;
                if (std::isfinite(x) && std::isfinite(y) && ParamValue(x).isValid(parameterInfo(px).minValue, parameterInfo(px).maxValue) &&
                    ParamValue(y).isValid(parameterInfo(py).minValue, parameterInfo(py).maxValue)) {
                    if (x != 0.0 || y != 0.0) {
                        props.set(px, x);
                        props.set(py, y);
                        any = true;
                    }
                }
            }
        } else if (id == "audiolevels") {
            set(ParamId::VolumeDb, value("level", ParamId::VolumeDb, decibels));
        } else if (id == "audiopan") {
            set(ParamId::Pan, value("pan", ParamId::Pan, same));
        } else if (native || own) {
            // X.1: an effect of the clip's stack. A native FCP7 filter in its units; Wabcut's own by parameter key.
            EffectInstance instance;
            instance.effect = native ? std::string(native->wabcut) : originalId;
            instance.enabled = !disabled;
            const EffectDescriptor* descriptor = EffectRegistry::builtin().find(instance.effect);
            const auto infoOf = [&](std::string_view key) -> const EffectParamInfo* {
                if (!descriptor) {
                    return nullptr;
                }
                for (const EffectParamInfo& p : descriptor->params) {
                    if (p.key == key) {
                        return &p;
                    }
                }
                return nullptr;
            };
            if (native) {
                for (const NativeParam& np : native->params) {
                    const EffectParamInfo* info = infoOf(np.key);
                    if (!info) {
                        continue;
                    }
                    const double scale = np.scale;
                    auto v = valueIn(np.fcpId, info->minValue, info->maxValue, [scale](double x) { return x / scale; });
                    // A value at the effect's default is left out, as Wabcut stores it.
                    if (v && !(v->isConstant() && v->constant() == info->defaultValue)) {
                        instance.params.push_back(EffectParam{std::string(np.key), std::move(*v)});
                    }
                }
                if (native->shadowColor) {
                    for (const pugi::xml_node p : effect.children("parameter")) {
                        if (!iequals(childText(p, "parameterid"), "color")) {
                            continue;
                        }
                        const pugi::xml_node v = p.child("value");
                        for (const char* channel : {"red", "green", "blue"}) {
                            const double c = std::clamp(childDouble(v, channel).value_or(0.0) / 255.0, 0.0, 1.0);
                            if (c != 0.0) {
                                instance.params.push_back(EffectParam{channel, ParamValue(c)});
                            }
                        }
                    }
                }
            } else {
                for (const pugi::xml_node p : effect.children("parameter")) {
                    const std::string key(childText(p, "parameterid"));
                    const EffectParamInfo* info = infoOf(key);
                    if (!info) {
                        report(ReportOutcome::Dropped, ReportCategory::Effect,
                               "'" + effectName + "' parameter '" + key + "' on '" + name + "' is unknown; not imported", p,
                               state.name);
                        continue;
                    }
                    if (auto v = valueIn(key, info->minValue, info->maxValue, same)) {
                        instance.params.push_back(EffectParam{key, std::move(*v)});
                    }
                }
            }
            std::sort(instance.params.begin(), instance.params.end(),
                      [](const EffectParam& a, const EffectParam& b) { return a.key < b.key; });
            instance.params.erase(std::unique(instance.params.begin(), instance.params.end(),
                                              [](const EffectParam& a, const EffectParam& b) { return a.key == b.key; }),
                                  instance.params.end());
            effects.push_back(std::move(instance));
        } else {
            understood = false;
        }
        if (!understood) {
            report(ReportOutcome::Dropped, ReportCategory::Effect,
                   "effect '" + (effectName.empty() ? id : effectName) + "' on '" + name +
                       "' is not supported yet",
                   effect, state.name);
        } else {
            report(ReportOutcome::Carried, ReportCategory::Effect,
                   "'" + effectName + "' on '" + name + "'" + (keyed ? " (keyframed)" : " (static values)"), effect,
                   state.name);
        }
    }
    if (!any) {
        return std::nullopt;
    }
    return props;
}

std::size_t Fcp7Parser::mediaForFile(const pugi::xml_node& fileRef, const pugi::xml_node& clipNode) {
    const std::string id = fileRef.attribute("id").value();
    pugi::xml_node file = fileRef;
    if (!id.empty()) {
        if (const auto it = m_files.find(id); it != m_files.end()) {
            file = it->second;
        }
    }
    const std::string key = !id.empty() ? "file:" + id : "url:" + std::string(childText(file, "pathurl"));
    if (const std::size_t existing = m_media.find(key); existing != kNoMedia) {
        return existing;
    }
    IrMedia media;
    media.key = key;
    media.name = std::string(childText(file, "name"));
    media.location = m_loc.locate(file);
    if (const auto it = m_fileBins.find(id); !id.empty() && it != m_fileBins.end()) {
        media.bin = it->second; // X.1: the bin its master clip is in
    }
    const std::string_view url = childText(file, "pathurl");
    bool decoded = false;
    media.reference = referenceFrom(url, media.name, decoded);
    if (!url.empty() && !decoded) {
        report(ReportOutcome::Approximated, ReportCategory::Media,
               "the path of '" + media.name + "' cannot be decoded (" + std::string(url) +
                   "); the media is offline",
               file, {});
    }
    if (media.name.empty()) {
        media.name =
            media.reference.name.empty() ? std::string(childText(clipNode, "name")) : media.reference.name;
    }
    const auto fileRate = parseRate(file.child("rate"));
    if (fileRate) {
        media.frameRate = fileRate;
    }
    if (const auto duration = childInt(file, "duration"); duration && *duration > 0 && fileRate) {
        media.duration = frames(*duration, *fileRate);
    }
    const pugi::xml_node fileMedia = file.child("media");
    if (const pugi::xml_node video = fileMedia.child("video")) {
        media.hasVideo = true;
        const pugi::xml_node sc = video.child("samplecharacteristics");
        media.width = static_cast<std::int32_t>(childInt(sc, "width").value_or(0));
        media.height = static_cast<std::int32_t>(childInt(sc, "height").value_or(0));
        if (!media.frameRate) {
            media.frameRate = parseRate(sc.child("rate"));
        }
    }
    for (const pugi::xml_node audio : fileMedia.children("audio")) {
        const auto channels = childInt(audio, "channelcount");
        media.audioChannels.push_back(
            static_cast<std::int32_t>(std::clamp<std::int64_t>(channels.value_or(1), 1, 64)));
        if (const auto rate = childInt(audio.child("samplecharacteristics"), "samplerate");
            rate && *rate > 0 && *rate < 1'000'000) {
            media.sampleRate = Rate{static_cast<std::int32_t>(*rate), 1};
        }
    }
    media.stillImage = isStillImageName(media.reference.path.empty() ? media.name : media.reference.path);
    return m_media.add(std::move(media));
}

StreamRef Fcp7Parser::audioStreamFor(std::size_t mediaIndex, std::int64_t trackIndex, int& channel,
                                     int& channelsInStream) {
    // <sourcetrack><trackindex> counts the file's audio channels across its audio streams (1-based).
    IrMedia& media = m_doc.media[mediaIndex];
    if (media.audioChannels.empty()) {
        media.audioChannels.push_back(1);
    }
    std::int64_t remaining = std::max<std::int64_t>(trackIndex, 1) - 1;
    for (std::size_t s = 0; s < media.audioChannels.size(); ++s) {
        if (remaining < media.audioChannels[s]) {
            channel = static_cast<int>(remaining) + 1;
            channelsInStream = media.audioChannels[s];
            return {StreamKind::Audio, static_cast<std::int32_t>(s)};
        }
        remaining -= media.audioChannels[s];
    }
    // More channels referenced than the file declares: treat each extra one as a mono stream.
    while (remaining >= 0 && media.audioChannels.size() < 64) {
        media.audioChannels.push_back(1);
        if (remaining == 0) {
            break;
        }
        --remaining;
    }
    channel = 1;
    channelsInStream = 1;
    return {StreamKind::Audio, static_cast<std::int32_t>(media.audioChannels.size() - 1)};
}

void Fcp7Parser::parseMarkers(SequenceState& state, const pugi::xml_node& owner,
                              std::optional<Ticks> clipStart, std::optional<Ticks> clipIn,
                              std::optional<Ticks> clipDuration, Rate rate, const std::string& clipName,
                              std::size_t media) {
    for (const pugi::xml_node m : owner.children("marker")) {
        const auto in = childInt(m, "in");
        const auto out = childInt(m, "out");
        std::string name(childText(m, "name"));
        if (!in || *in < 0) {
            report(ReportOutcome::Dropped, ReportCategory::Marker, "marker '" + name + "' has no position", m,
                   state.name);
            continue;
        }
        const auto start = frames(*in, rate);
        const auto end = out && *out > *in ? frames(*out, rate) : start;
        if (!start || !end) {
            report(ReportOutcome::Dropped, ReportCategory::Marker, "marker '" + name + "' is out of range", m,
                   state.name);
            continue;
        }
        IrMarker marker;
        marker.marker.name = name;
        marker.marker.comment = std::string(childText(m, "comment"));
        marker.marker.duration = *end - *start;
        marker.location = m_loc.locate(m);
        if (const pugi::xml_node color = m.child("color")) {
            marker.marker.colorIndex =
                markerColorFromRgb(static_cast<int>(childInt(color, "red").value_or(0)),
                                   static_cast<int>(childInt(color, "green").value_or(0)),
                                   static_cast<int>(childInt(color, "blue").value_or(0)));
        } else if (const auto packed = childInt(m, "pproColor")) {
            // Premiere packs the colour as 0xAABBGGRR.
            const auto v = static_cast<std::uint32_t>(*packed);
            marker.marker.colorIndex =
                markerColorFromRgb(static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF),
                                   static_cast<int>((v >> 16) & 0xFF));
        }
        if (clipStart && media != kNoMedia) {
            // X.1: a clip marker is a marker of the clip's source (Premiere's clip markers live on the project item),
            // in its media frames: it moves with the clip. Every clip of the source lists them; they are kept once.
            marker.marker.start = *start;
            auto& list = m_doc.media[media].markers;
            const bool known = std::any_of(list.begin(), list.end(), [&](const IrMarker& k) {
                return k.marker.start == marker.marker.start && k.marker.duration == marker.marker.duration &&
                       k.marker.name == marker.marker.name && k.marker.comment == marker.marker.comment;
            });
            if (!known && list.size() < 10'000) {
                list.push_back(std::move(marker));
            }
            continue;
        }
        if (clipStart) {
            // Clip markers are in source frames; place them where that frame plays in the sequence.
            const Ticks offset = *start - clipIn.value_or(Ticks{});
            if (offset.value < 0 || (clipDuration && offset >= *clipDuration)) {
                report(ReportOutcome::Dropped, ReportCategory::Marker,
                       "clip marker '" + name + "' on '" + clipName +
                           "' lies outside the part of the clip in the sequence",
                       m, state.name);
                continue;
            }
            marker.marker.start = *clipStart + offset;
            marker.approximation = "clip marker on '" + clipName +
                                   "' became a sequence marker (it no longer moves with the clip)";
        } else {
            marker.marker.start = *start;
        }
        state.seq.markers.push_back(std::move(marker));
    }
}

void Fcp7Parser::resolveEdges(std::vector<TrackItem>& items, SequenceState& state) {
    // Transitions first: their extent and the cut they sit on. Premiere writes the cut's offset from the
    // transition start as <cutPointTicks>; otherwise the alignment says: centre = the middle (whole frames,
    // rounded down like OTIO), start/start-black = the transition's start, end/end-black = its end.
    for (TrackItem& t : items) {
        if (t.kind != TrackItem::Kind::Transition || t.start < 0 || t.end <= t.start) {
            continue;
        }
        t.startTicks = frames(t.start, state.rate);
        t.endTicks = frames(t.end, state.rate);
        if (!t.startTicks || !t.endTicks) {
            continue;
        }
        const std::string_view alignment = childText(t.node, "alignment");
        std::int64_t cutFrame = t.start + (t.end - t.start) / 2;
        if (iequals(alignment, "start") || iequals(alignment, "start-black")) {
            cutFrame = t.start;
        } else if (iequals(alignment, "end") || iequals(alignment, "end-black")) {
            cutFrame = t.end;
        }
        if (const auto cutPoint = childInt(t.node, "cutPointTicks"); cutPoint && *cutPoint >= 0) {
            const std::int64_t offset = floorDiv(*cutPoint, state.rate.ticksPerUnit());
            if (offset <= t.end - t.start) {
                cutFrame = t.start + offset;
            }
        }
        t.cutTicks = frames(cutFrame, state.rate);
    }
    // Clips: a -1 start/end is the cut of the adjacent transition. The clip's <in>/<out> include the
    // transition's handles (Apple's FCP XML reference, listing 3-17), so they are not used for the edit
    // points.
    for (std::size_t i = 0; i < items.size(); ++i) {
        TrackItem& item = items[i];
        if (item.kind == TrackItem::Kind::Transition) {
            continue;
        }
        if (item.start >= 0) {
            item.startTicks = frames(item.start, state.rate);
        } else if (i > 0 && items[i - 1].kind == TrackItem::Kind::Transition && items[i - 1].cutTicks) {
            item.startTicks = items[i - 1].cutTicks;
            item.headHandle = *items[i - 1].cutTicks - *items[i - 1].startTicks;
        }
        if (item.end >= 0) {
            item.endTicks = frames(item.end, state.rate);
        } else if (i + 1 < items.size() && items[i + 1].kind == TrackItem::Kind::Transition &&
                   items[i + 1].cutTicks) {
            item.endTicks = items[i + 1].cutTicks;
        }
        // Malformed: a -1 without a transition next to it. Fall back to the clip's own in/out.
        if (item.startTicks.has_value() != item.endTicks.has_value()) {
            const auto in = childInt(item.node, "in");
            const auto out = childInt(item.node, "out");
            const Rate clipRate = parseRate(item.node.child("rate")).value_or(state.rate);
            if (in && out && *out > *in) {
                if (const auto length = frames(*out - *in, clipRate)) {
                    if (item.startTicks) {
                        item.endTicks = *item.startTicks + *length;
                    } else {
                        item.startTicks = *item.endTicks - *length;
                    }
                }
            }
        }
    }
}

void Fcp7Parser::parseTrack(SequenceState& state, const pugi::xml_node& trackNode, TrackKind kind,
                            std::size_t index, int depth, IrTrack* mergeInto, std::int64_t explodedChannels) {
    const std::string label = (kind == TrackKind::Video ? "V" : "A") + std::to_string(index + 1);
    IrTrack local;
    IrTrack& track = mergeInto ? *mergeInto : local;
    if (!mergeInto) {
        track.kind = kind;
        track.enabled = childBool(trackNode, "enabled").value_or(true);
        track.locked = childBool(trackNode, "locked").value_or(false);
        track.location = m_loc.locate(trackNode);
        if (const auto targeted = attrInt(trackNode, "MZ.TrackTargeted")) {
            track.targeted = *targeted != 0;
        }
        track.name = std::string(attrText(trackNode, "MZ.TrackName"));
    }

    std::vector<TrackItem> items;
    for (pugi::xml_node child = trackNode.first_child(); child; child = child.next_sibling()) {
        TrackItem item;
        item.node = child;
        if (nameIs(child, "clipitem")) {
            item.kind = TrackItem::Kind::Clip;
        } else if (nameIs(child, "transitionitem")) {
            item.kind = TrackItem::Kind::Transition;
        } else if (nameIs(child, "generatoritem")) {
            item.kind = TrackItem::Kind::Generator;
        } else {
            continue;
        }
        item.start = childInt(child, "start").value_or(-1);
        item.end = childInt(child, "end").value_or(-1);
        items.push_back(item);
    }
    resolveEdges(items, state);

    for (const TrackItem& item : items) {
        const pugi::xml_node node = item.node;
        const std::string name(childText(node, "name"));
        if (item.kind == TrackItem::Kind::Transition) {
            if (mergeInto) {
                continue; // the primary track of an exploded stereo pair already has it
            }
            const pugi::xml_node effect = node.child("effect");
            IrTransition t;
            t.name = std::string(childText(effect, "name"));
            const std::string id = toLowerAscii(childText(effect, "effectid"));
            const std::string lowerName = toLowerAscii(t.name);
            if (kind == TrackKind::Audio) {
                t.kind = TransitionKind::AudioCrossfade;
            } else if (lowerName.find("dip to black") != std::string::npos ||
                       id.find("diptocolor") != std::string::npos) {
                t.kind = lowerName.find("white") != std::string::npos ? TransitionKind::DipToWhite
                                                                      : TransitionKind::DipToBlack;
            } else if (lowerName.find("dissolve") != std::string::npos ||
                       id.find("dissolve") != std::string::npos) {
                t.kind = TransitionKind::Dissolve;
            } else if (lowerName.find("wipe") != std::string::npos) {
                t.kind = TransitionKind::Wipe;
            } else {
                t.kind = TransitionKind::Other;
            }
            if (!item.startTicks || !item.endTicks || !item.cutTicks) {
                report(ReportOutcome::Dropped, ReportCategory::Transition,
                       "transition '" + t.name + "' has no valid extent", node, state.name);
                continue;
            }
            t.cut = *item.cutTicks;
            t.before = *item.cutTicks - *item.startTicks;
            t.after = *item.endTicks - *item.cutTicks;
            t.location = m_loc.locate(node);
            track.transitions.push_back(std::move(t));
            continue;
        }
        if (item.kind == TrackItem::Kind::Generator) {
            if (kind == TrackKind::Video && !mergeInto) {
                if (auto title = titleOf(node, name)) {
                    if (!item.startTicks || !item.endTicks || *item.endTicks <= *item.startTicks) {
                        report(ReportOutcome::Dropped, ReportCategory::Clip,
                               "title '" + name + "' on " + label + " has no valid position (start/end)", node,
                               state.name);
                        continue;
                    }
                    IrClip clip;
                    // Default generator names ("Text") give way to the title's own name (its first line).
                    const std::string effectName(childText(node.child("effect"), "name"));
                    clip.name = iequals(name, effectName) || iequals(name, "Text") || iequals(name, "Outline Text")
                                    ? std::string()
                                    : name;
                    clip.stream = {StreamKind::Video, 0};
                    clip.start = *item.startTicks;
                    clip.duration = *item.endTicks - *item.startTicks;
                    clip.enabled = childBool(node, "enabled").value_or(true);
                    clip.location = m_loc.locate(node);
                    clip.title = std::move(title);
                    track.clips.push_back(std::move(clip));
                    continue;
                }
            }
            report(ReportOutcome::Dropped, ReportCategory::Clip,
                   "generated clip '" + name + "' on " + label +
                       " (no media file) is not supported yet; its time is left as a gap",
                   node, state.name);
            continue;
        }
        // A clipitem: media, nested sequence or multiclip (C.8: nested and multicam clips).
        if (const pugi::xml_node nested = node.child("sequence")) {
            const auto slot = depth + 1 < kMaxNesting ? parseSequence(nested, depth + 1) : std::nullopt;
            if (!slot) {
                report(ReportOutcome::Dropped, ReportCategory::Clip,
                       "nested sequence '" + name + "' on " + label +
                           (depth + 1 >= kMaxNesting ? " is nested too deep" : " refers to a sequence the file does not have") +
                           "; its time is left as a gap",
                       node, state.name);
                continue;
            }
            if (!mergeInto) {
                addNestedClip(state, item, track, kind, index, *slot, {}, name, label);
            }
            continue;
        }
        if (const pugi::xml_node multiclip = node.child("multiclip")) {
            const auto slot = depth + 1 < kMaxNesting
                                  ? parseMulticlip(multiclip, parseRate(node.child("rate")).value_or(state.rate), depth + 1)
                                  : std::nullopt;
            if (!slot) {
                report(ReportOutcome::Dropped, ReportCategory::Clip,
                       "multicam clip '" + name + "' on " + label +
                           " has no angles with media tracks; its time is left as a gap",
                       node, state.name);
                continue;
            }
            // The active angle (the first when none is marked).
            std::size_t active = 0;
            std::size_t angleIndex = 0;
            for (const pugi::xml_node angle : multiclip.children("angle")) {
                if (childBool(angle, kind == TrackKind::Video ? "activevideoangle" : "activeaudioangle").value_or(false)) {
                    active = angleIndex;
                    break;
                }
                ++angleIndex;
            }
            if (!mergeInto) {
                addNestedClip(state, item, track, kind, index, *slot, {active}, name, label);
            }
            continue;
        }
        const pugi::xml_node fileRef = node.child("file");
        if (!fileRef) {
            report(ReportOutcome::Dropped, ReportCategory::Clip,
                   "clip '" + name + "' on " + label + " has no media file", node, state.name);
            continue;
        }
        const auto clipRate = parseRate(node.child("rate")).value_or(state.rate);
        const auto start = item.startTicks;
        const auto end = item.endTicks;
        if (!start || !end || *end <= *start || start->value < 0) {
            report(ReportOutcome::Dropped, ReportCategory::Clip,
                   "clip '" + name + "' on " + label + " has no valid position (start/end)", node,
                   state.name);
            continue;
        }
        const Remap speed = speedOf(node);
        // The media In: Premiere's exact <pproTicksIn> (media time, speed-independent), else the speed
        // graph's speedkfin value, else <in> (which counts retimed clip frames when the speed is not 100%).
        std::optional<Ticks> sourceIn;
        const auto in = childInt(node, "in");
        if (const auto ticksIn = childInt(node, "pproTicksIn"); ticksIn && *ticksIn >= 0) {
            sourceIn = Ticks{*ticksIn};
        } else if (speed.mediaInFrame) {
            sourceIn = frames(*speed.mediaInFrame, clipRate);
        } else if (in) {
            sourceIn = frames(std::llround(static_cast<double>(*in) * speed.factor()), clipRate);
        }
        if (!sourceIn) {
            report(ReportOutcome::Dropped, ReportCategory::Clip,
                   "clip '" + name + "' on " + label + " has no source In", node, state.name);
            continue;
        }
        if (item.headHandle.value > 0) {
            // The clip's In is the frame at the start of the transition before it; the clip itself starts
            // at the cut, that much later in the media.
            *sourceIn += Ticks{std::llround(static_cast<double>(item.headHandle.value) * speed.factor())};
        }

        const std::size_t mediaIndex = mediaForFile(fileRef, node);
        IrClip clip;
        clip.name = name.empty() ? m_doc.media[mediaIndex].name : name;
        clip.media = mediaIndex;
        clip.start = *start;
        clip.duration = *end - *start;
        clip.sourceIn = *sourceIn;
        clip.enabled = childBool(node, "enabled").value_or(true);
        clip.location = m_loc.locate(node);
        const std::string_view mediaType = childText(node.child("sourcetrack"), "mediatype");
        if (kind == TrackKind::Video) {
            if (!mediaType.empty() && !iequals(mediaType, "video")) {
                report(ReportOutcome::Dropped, ReportCategory::Clip,
                       "clip '" + clip.name + "' on " + label + " plays " + std::string(mediaType) +
                           " on a video track",
                       node, state.name);
                continue;
            }
            m_doc.media[mediaIndex].hasVideo = true;
            clip.stream = {StreamKind::Video, 0};
        } else {
            int channel = 1;
            int channelsInStream = 1;
            const auto trackIndex = childInt(node.child("sourcetrack"), "trackindex").value_or(1);
            clip.stream = audioStreamFor(mediaIndex, trackIndex, channel, channelsInStream);
            if (mergeInto) {
                // Second (third, ...) channel of a Premiere stereo/multichannel track: the primary track's
                // clip already plays the whole stream.
                const auto twin = std::find_if(track.clips.begin(), track.clips.end(), [&](const IrClip& o) {
                    return o.media == clip.media && o.start == clip.start && o.duration == clip.duration;
                });
                if (twin != track.clips.end()) {
                    continue; // Premiere links every channel with the video, so the twin keeps the link
                }
                report(ReportOutcome::Approximated, ReportCategory::AudioMapping,
                       "'" + clip.name + "' on channel " + std::to_string(channel) +
                           " of an exploded multichannel track has no partner on the first channel; it was "
                           "placed on " +
                           label,
                       node, state.name);
            } else if (channelsInStream > 1 && !(explodedChannels == channelsInStream && channel == 1)) {
                // (A Premiere stereo track starting at channel 1 of a stereo stream plays exactly that
                // stream.)
                state.channelClips.push_back({index, track.clips.size(), channel, channelsInStream});
            }
        }

        if (speed.variable && speed.graph.size() >= 2 && in && *in >= 0) {
            // The time graph is a time remap: its "when" frames are the clip's own (<in> at its first frame,
            // after any transition handle), its values media frames. Corner keys at whole frames are exact.
            IrRemap remap;
            remap.location = m_loc.locate(node);
            remap.curveIn = frames(*in, clipRate).value_or(Ticks{}) + item.headHandle;
            bool whole = true;
            bool smooth = false;
            std::vector<bool> smoothKeys;
            for (const GraphKey& g : speed.graph) {
                const std::int64_t when = std::llround(g.when);
                const std::int64_t value = std::max<std::int64_t>(std::llround(g.value), 0);
                whole = whole && static_cast<double>(when) == g.when && static_cast<double>(value) == g.value;
                smooth = smooth || g.smooth;
                const auto time = frames(when, clipRate);
                const auto position = frames(value, clipRate);
                if (!time || !position) {
                    continue;
                }
                Keyframe k{*time, static_cast<double>(position->value)};
                if (const auto at = remap.curve.keyAt(k.time)) {
                    remap.curve.keys[*at] = k; // a key twice: the later one
                    smoothKeys[*at] = g.smooth;
                } else {
                    const std::size_t slot = remap.curve.insertionIndex(k.time);
                    remap.curve.keys.insert(remap.curve.keys.begin() + static_cast<std::ptrdiff_t>(slot), k);
                    smoothKeys.insert(smoothKeys.begin() + static_cast<std::ptrdiff_t>(slot), g.smooth);
                }
            }
            // X.1: smooth keys become Bezier keys (the speed ramps through them); FCP7 keeps no handles, so they
            // are monotone tangents through the neighbouring keys.
            if (smooth && !smoothRemapKeys(remap.curve, smoothKeys)) {
                remap.approximation = "its smooth speed ramps were imported as straight segments";
            } else if (smooth) {
                remap.approximation = "its smooth speed ramps were imported as Bezier keys with tangents through the "
                                      "neighbouring keys (FCP7 XML keeps no handles)";
            }
            if (!whole) {
                remap.approximation += std::string(remap.approximation.empty() ? "" : "; ") +
                                       "keys between frames were moved to whole frames";
            }
            // Untrusted keys: validated before anything evaluates them.
            if (const auto problem = remapProblem(remap.curve)) {
                report(ReportOutcome::Approximated, ReportCategory::Speed,
                       "'" + clip.name + "' on " + label + " has a time graph that is not a valid time remap (it " +
                           *problem + "); imported at 100% from the same source In",
                       node, state.name);
            } else {
                clip.sourceIn = remapSourceAt(remap.curve, remap.curveIn);
                clip.remap = std::move(remap);
            }
        } else if (speed.variable) {
            report(ReportOutcome::Approximated, ReportCategory::Speed,
                   "'" + clip.name + "' on " + label +
                       " has variable speed (time remapping) without a time graph; imported at 100% from the same "
                       "source In",
                   node, state.name);
        } else if (std::abs(speed.percent - 100.0) > 1e-6 || speed.reverse) {
            IrSpeed sp;
            sp.reversed = speed.reverse;
            sp.location = m_loc.locate(node);
            if (speed.percent == 0.0) {
                sp.ratio = wabcut::Speed{0, 1};
                sp.reversed = false;
                clip.speed = sp;
            } else if (const auto ratio = speedFromDouble(speed.percent / 100.0)) {
                sp.ratio = ratio->speed;
                if (!ratio->exact) {
                    sp.approximation = "the speed " + std::to_string(speed.percent) + "% was rounded to " +
                                       speedLabel(ratio->speed);
                }
                clip.speed = sp;
            } else {
                report(ReportOutcome::Approximated, ReportCategory::Speed,
                       "'" + clip.name + "' on " + label + " has an unusable speed; imported at 100%", node,
                       state.name);
            }
        }
        clip.properties = propertiesOf(state, node, clip.name, clip.effects);
        parseMarkers(state, node, clip.start, clip.sourceIn, clip.duration, clipRate, clip.name, mediaIndex);

        if (const std::string id = node.attribute("id").value(); !id.empty()) {
            state.links.idOf(id);
            for (const pugi::xml_node link : node.children("link")) {
                const std::string other(childText(link, "linkclipref"));
                if (!other.empty()) {
                    state.links.unite(id, other);
                }
            }
            state.clipsById[id] = {kind == TrackKind::Video, index, track.clips.size()};
        }
        track.clips.push_back(std::move(clip));
    }
    if (!mergeInto) {
        (kind == TrackKind::Video ? state.seq.videoTracks : state.seq.audioTracks)
            .push_back(std::move(local));
    }
}

void Fcp7Parser::addNestedClip(SequenceState& state, const TrackItem& item, IrTrack& track, TrackKind kind,
                               std::size_t index, std::size_t slot, std::vector<std::size_t> angles,
                               const std::string& name, const std::string& label) {
    const pugi::xml_node node = item.node;
    const auto start = item.startTicks;
    const auto end = item.endTicks;
    if (!start || !end || *end <= *start || start->value < 0) {
        report(ReportOutcome::Dropped, ReportCategory::Clip,
               "clip '" + name + "' on " + label + " has no valid position (start/end)", node, state.name);
        return;
    }
    const auto clipRate = parseRate(node.child("rate")).value_or(state.rate);
    // The exact In in ticks where Premiere (or Wabcut) wrote it, else the <in> frame.
    const auto ticksIn = childInt(node, "pproTicksIn");
    const auto in = ticksIn && *ticksIn >= 0 ? std::optional<Ticks>(Ticks{*ticksIn})
                                             : frames(std::max<std::int64_t>(childInt(node, "in").value_or(0), 0), clipRate);
    if (!in) {
        report(ReportOutcome::Dropped, ReportCategory::Clip, "clip '" + name + "' on " + label + " has no source In",
               node, state.name);
        return;
    }
    const Remap speed = speedOf(node);
    if (speed.variable || speed.reverse || std::abs(speed.percent - 100.0) > 1e-9) {
        report(ReportOutcome::Approximated, ReportCategory::Speed,
               "'" + name + "' on " + label + " plays a nested sequence at another speed; imported at 100%", node,
               state.name);
    }
    IrClip clip;
    clip.name = name.empty() ? m_doc.sequences[slot].name : name;
    clip.sequence = slot;
    clip.angles = std::move(angles);
    clip.stream = {kind == TrackKind::Video ? StreamKind::Video : StreamKind::Audio, 0};
    clip.start = *start;
    clip.duration = *end - *start;
    clip.sourceIn = *in + item.headHandle;
    clip.enabled = childBool(node, "enabled").value_or(true);
    clip.location = m_loc.locate(node);
    clip.properties = propertiesOf(state, node, clip.name, clip.effects);
    if (const std::string id = node.attribute("id").value(); !id.empty()) {
        state.links.idOf(id);
        for (const pugi::xml_node link : node.children("link")) {
            const std::string other(childText(link, "linkclipref"));
            if (!other.empty()) {
                state.links.unite(id, other);
            }
        }
        state.clipsById[id] = {kind == TrackKind::Video, index, track.clips.size()};
    }
    track.clips.push_back(std::move(clip));
}

std::optional<std::size_t> Fcp7Parser::parseMulticlip(const pugi::xml_node& multiclip, Rate rate, int depth) {
    const std::string key = "multiclip:" + std::string(multiclip.attribute("id").value());
    if (!multiclip.attribute("id").empty()) {
        if (const auto it = m_sequenceSlots.find(key); it != m_sequenceSlots.end()) {
            return it->second;
        }
    }
    if (!multiclip.child("angle")) {
        return std::nullopt; // a reference to a multiclip defined elsewhere that we have not seen
    }
    const std::size_t slot = m_doc.sequences.size();
    m_doc.sequences.emplace_back();
    if (!multiclip.attribute("id").empty()) {
        m_sequenceSlots.emplace(key, slot);
    }
    SequenceState state;
    state.name = std::string(childText(multiclip, "name"));
    if (state.name.empty()) {
        state.name = "Multiclip " + std::to_string(slot + 1);
    }
    state.rate = rate;
    IrSequence& seq = state.seq;
    seq.name = state.name;
    seq.frameRate = rate;
    seq.location = m_loc.locate(multiclip);
    // synctype: 1 In point, 2 Out point, 3 timecode, 4 / 5 aux timecodes.
    IrMulticam info;
    switch (childInt(multiclip, "synctype").value_or(0)) {
    case 1:
        info.sync = MulticamSync::InPoints;
        break;
    case 2:
        info.sync = MulticamSync::OutPoints;
        break;
    case 3:
    case 4:
    case 5:
        info.sync = MulticamSync::Timecode;
        break;
    default:
        info.sync = MulticamSync::Manual;
        break;
    }
    std::size_t videoIndex = 0;
    std::size_t audioIndex = 0;
    bool any = false;
    for (const pugi::xml_node angle : multiclip.children("angle")) {
        const pugi::xml_node clip = angle.child("clip");
        const pugi::xml_node media = clip.child("media");
        IrMulticamAngle a;
        a.name = std::string(childText(clip, "name"));
        for (const pugi::xml_node track : media.child("video").children("track")) {
            if (!a.videoTrack) {
                a.videoTrack = videoIndex;
                parseTrack(state, track, TrackKind::Video, videoIndex++, depth, nullptr);
            } else {
                report(ReportOutcome::Dropped, ReportCategory::Track,
                       "angle '" + a.name + "' of multicam '" + state.name + "' has several video tracks; only the first is carried",
                       track, state.name);
            }
        }
        for (const pugi::xml_node track : media.child("audio").children("track")) {
            a.audioTracks.push_back(audioIndex);
            parseTrack(state, track, TrackKind::Audio, audioIndex++, depth, nullptr);
        }
        Ticks first = Ticks::max();
        for (const auto* tracks : {&seq.videoTracks, &seq.audioTracks}) {
            for (std::size_t t = 0; t < tracks->size(); ++t) {
                const bool mine = (tracks == &seq.videoTracks && a.videoTrack == t) ||
                                  (tracks == &seq.audioTracks &&
                                   std::find(a.audioTracks.begin(), a.audioTracks.end(), t) != a.audioTracks.end());
                for (const IrClip& c : (*tracks)[t].clips) {
                    if (mine) {
                        first = std::min(first, c.start);
                        seq.duration = std::max(seq.duration.value_or(Ticks{}), c.start + c.duration);
                    }
                }
            }
        }
        any = any || a.videoTrack || !a.audioTracks.empty();
        a.offset = first == Ticks::max() ? Ticks{} : std::max(first, Ticks{});
        info.angles.push_back(std::move(a));
    }
    if (!any) {
        return std::nullopt; // the slot stays an empty sequence; nothing refers to it
    }
    // Links among the angles' clips (an angle's picture and sound), as in a sequence.
    std::map<std::size_t, std::int64_t> groups;
    for (const auto& [clipId, where] : state.clipsById) {
        const auto root = state.links.rootOf(clipId);
        const auto [isVideo, trackIndex, clipIndex] = where;
        auto& tracks = isVideo ? seq.videoTracks : seq.audioTracks;
        if (!root || trackIndex >= tracks.size() || clipIndex >= tracks[trackIndex].clips.size()) {
            continue;
        }
        auto [it, inserted] = groups.emplace(*root, 0);
        if (inserted) {
            it->second = m_nextLink++;
        }
        tracks[trackIndex].clips[clipIndex].linkGroup = it->second;
    }
    // A link group of one clip links nothing.
    std::map<std::int64_t, int> members;
    for (const auto* tracks : {&seq.videoTracks, &seq.audioTracks}) {
        for (const IrTrack& t : *tracks) {
            for (const IrClip& c : t.clips) {
                if (c.linkGroup >= 0) {
                    ++members[c.linkGroup];
                }
            }
        }
    }
    for (auto* tracks : {&seq.videoTracks, &seq.audioTracks}) {
        for (IrTrack& t : *tracks) {
            for (IrClip& c : t.clips) {
                if (c.linkGroup >= 0 && members[c.linkGroup] < 2) {
                    c.linkGroup = -1;
                }
            }
        }
    }
    seq.multicam = std::move(info);
    m_doc.sequences[slot] = std::move(seq);
    return slot;
}

void Fcp7Parser::mergeStereoPairs(SequenceState& state) {
    // FCP7 stereo pairs: linked clips on different audio tracks playing channels 1, 2, ... of one stream over
    // the same range. A Wabcut clip plays a whole stream, so they become one clip on the first track.
    auto& tracks = state.seq.audioTracks;
    using Key = std::tuple<std::int64_t, std::size_t, std::int32_t, std::int64_t, std::int64_t, std::int64_t>;
    std::map<Key, std::vector<std::size_t>> groups;
    for (std::size_t k = 0; k < state.channelClips.size(); ++k) {
        const auto& cc = state.channelClips[k];
        if (cc.track >= tracks.size() || cc.clip >= tracks[cc.track].clips.size()) {
            continue;
        }
        const IrClip& c = tracks[cc.track].clips[cc.clip];
        if (c.linkGroup < 0) {
            continue;
        }
        groups[{c.linkGroup, c.media, c.stream.index, c.start.value, c.duration.value, c.sourceIn.value}]
            .push_back(k);
    }
    std::vector<std::pair<std::size_t, std::size_t>> remove;
    std::vector<bool> merged(state.channelClips.size(), false);
    for (const auto& [key, members] : groups) {
        if (members.size() < 2) {
            continue;
        }
        const auto& first = state.channelClips[members.front()];
        std::string tracksLabel;
        for (const std::size_t k : members) {
            const auto& cc = state.channelClips[k];
            merged[k] = true;
            tracksLabel += (tracksLabel.empty() ? "A" : "+A") + std::to_string(cc.track + 1);
            if (k != members.front()) {
                remove.emplace_back(cc.track, cc.clip);
            }
        }
        const IrClip& c = tracks[first.track].clips[first.clip];
        m_doc.report.add(ReportOutcome::Approximated, ReportCategory::AudioMapping,
                         "the " + std::to_string(members.size()) + " channels of '" + c.name + "' on " +
                             tracksLabel + " became one clip on A" + std::to_string(first.track + 1) +
                             " playing the whole " + std::to_string(first.channels) + "-channel stream",
                         c.location, state.name);
    }
    for (std::size_t k = 0; k < state.channelClips.size(); ++k) {
        const auto& cc = state.channelClips[k];
        if (merged[k] || cc.track >= tracks.size() || cc.clip >= tracks[cc.track].clips.size()) {
            continue;
        }
        // A.9: the clip plays that one channel (a mono channel map, Premiere's mono clip of a channel).
        IrClip& c = tracks[cc.track].clips[cc.clip];
        c.channels = AudioChannelMap::mono(cc.channel - 1);
        m_doc.report.add(ReportOutcome::Carried, ReportCategory::AudioMapping,
                         "'" + c.name + "' on A" + std::to_string(cc.track + 1) + " plays channel " +
                             std::to_string(cc.channel) + " of its " + std::to_string(cc.channels) +
                             "-channel stream (a mono channel map)",
                         c.location, state.name);
    }
    // Erase from the back so earlier indices stay valid.
    std::sort(remove.begin(), remove.end(), std::greater<>());
    for (const auto& [track, clip] : remove) {
        tracks[track].clips.erase(tracks[track].clips.begin() + static_cast<std::ptrdiff_t>(clip));
    }
}

std::vector<std::string> Fcp7Parser::binPathOf(const pugi::xml_node& node) {
    std::vector<std::string> path;
    for (pugi::xml_node n = node.parent(); n && path.size() < 64; n = n.parent()) {
        if (nameIs(n, "bin")) {
            path.insert(path.begin(), std::string(childText(n, "name")));
        }
    }
    return path;
}

std::optional<std::size_t> Fcp7Parser::parseSequence(const pugi::xml_node& node, int depth,
                                                     const std::vector<std::string>& bin) {
    const std::string id = node.attribute("id").value();
    if (!id.empty()) {
        if (const auto it = m_sequenceSlots.find(id); it != m_sequenceSlots.end()) {
            return it->second;
        }
    }
    if (!node.child("media") && !id.empty() && !node.first_child()) {
        return std::nullopt; // a reference to a sequence defined elsewhere that we have not seen
    }
    // Reserve the slot first so a parent sequence is listed before the sequences nested in it.
    const std::size_t slot = m_doc.sequences.size();
    m_doc.sequences.emplace_back();
    if (!id.empty()) {
        m_sequenceSlots.emplace(id, slot);
    }

    SequenceState state;
    state.name = std::string(childText(node, "name"));
    if (state.name.empty()) {
        state.name = "Sequence " + std::to_string(slot + 1);
    }
    IrSequence& seq = state.seq;
    seq.name = state.name;
    seq.location = m_loc.locate(node);
    seq.bin = bin;
    if (const auto rate = parseRate(node.child("rate"))) {
        state.rate = *rate;
    } else {
        report(ReportOutcome::Approximated, ReportCategory::SequenceSetting,
               "the sequence has no frame rate; 30 fps assumed", node, state.name);
    }
    seq.frameRate = state.rate;
    const pugi::xml_node timecode = node.child("timecode");
    state.dropFrame = iequals(childText(timecode, "displayformat"), "DF") && supportsDropFrame(state.rate);
    seq.dropFrame = state.dropFrame;
    if (const auto startFrame = childInt(timecode, "frame"); startFrame && *startFrame > 0) {
        // X.1: the sequence keeps it (Sequence::startTimecode); times still count from its first frame.
        seq.startTimecode = frames(*startFrame, state.rate);
    }

    const pugi::xml_node media = node.child("media");
    const pugi::xml_node video = media.child("video");
    const pugi::xml_node audio = media.child("audio");
    const pugi::xml_node videoFormat = video.child("format").child("samplecharacteristics");
    seq.width = static_cast<std::int32_t>(childInt(videoFormat, "width").value_or(1920));
    seq.height = static_cast<std::int32_t>(childInt(videoFormat, "height").value_or(1080));
    if (const auto sampleRate = childInt(audio.child("format").child("samplecharacteristics"), "samplerate");
        sampleRate && *sampleRate > 0 && *sampleRate < 1'000'000) {
        seq.audioRate = Rate{static_cast<std::int32_t>(*sampleRate), 1};
    }
    if (const auto duration = childInt(node, "duration"); duration && *duration > 0) {
        seq.duration = frames(*duration, state.rate);
    }
    if (const auto in = childInt(node, "in"); in && *in >= 0) {
        seq.markIn = frames(*in, state.rate);
    }
    if (const auto out = childInt(node, "out"); out && *out > 0) {
        seq.markOut = frames(*out, state.rate);
    }

    std::size_t videoIndex = 0;
    for (const pugi::xml_node track : video.children("track")) {
        parseTrack(state, track, TrackKind::Video, videoIndex++, depth, nullptr);
    }
    std::size_t audioIndex = 0;
    for (const pugi::xml_node track : audio.children("track")) {
        // Premiere writes a stereo (5.1, ...) track as consecutive mono tracks with
        // currentExplodedTrackIndex 0, 1, ... of totalExplodedTrackCount: they are one Wabcut track.
        const auto exploded = attrInt(track, "currentExplodedTrackIndex").value_or(0);
        const auto total = attrInt(track, "totalExplodedTrackCount").value_or(1);
        if (exploded > 0 && total > 1 && !state.seq.audioTracks.empty()) {
            parseTrack(state, track, TrackKind::Audio, audioIndex - 1, depth, &state.seq.audioTracks.back());
            continue;
        }
        parseTrack(state, track, TrackKind::Audio, audioIndex++, depth, nullptr, total);
        if (total > 1) {
            report(ReportOutcome::Carried, ReportCategory::AudioMapping,
                   "A" + std::to_string(audioIndex) + " is a " + std::to_string(total) +
                       "-channel track (written as " + std::to_string(total) +
                       " mono tracks); imported as one track",
                   track, state.name);
        }
    }
    parseMarkers(state, node, std::nullopt, std::nullopt, std::nullopt, state.rate, {});

    // Links: every IR clip whose clipitem is in a <link> set with another placed clip.
    std::map<std::size_t, std::int64_t> groups;
    for (const auto& [clipId, where] : state.clipsById) {
        const auto root = state.links.rootOf(clipId);
        if (!root) {
            continue;
        }
        const auto [isVideo, trackIndex, clipIndex] = where;
        auto& tracks = isVideo ? seq.videoTracks : seq.audioTracks;
        if (trackIndex >= tracks.size() || clipIndex >= tracks[trackIndex].clips.size()) {
            continue;
        }
        auto [it, inserted] = groups.emplace(*root, 0);
        if (inserted) {
            it->second = m_nextLink++;
        }
        tracks[trackIndex].clips[clipIndex].linkGroup = it->second;
    }
    mergeStereoPairs(state);
    std::sort(seq.markers.begin(), seq.markers.end(),
              [](const IrMarker& a, const IrMarker& b) { return a.marker.start < b.marker.start; });
    m_doc.sequences[slot] = std::move(seq);
    return slot;
}

std::expected<void, Error> Fcp7Parser::parse(const pugi::xml_document& xml) {
    const pugi::xml_node root = xml.document_element();
    if (!nameIs(root, "xmeml")) {
        return std::unexpected(
            parseError("the root element is <" + std::string(root.name()) + ">, not <xmeml>"));
    }
    collectFiles(root);
    const auto sequences = topLevelSequences(root);
    if (sequences.empty()) {
        return std::unexpected(parseError("the XML contains no sequences"));
    }
    for (const pugi::xml_node& s : sequences) {
        parseSequence(s, 0, binPathOf(s)); // X.1: in the bins the project keeps it in
    }
    return {};
}

} // namespace

std::expected<IrDocument, Error> parseFcp7Xml(std::string_view data, const ParseContext& context) {
    pugi::xml_document xml;
    if (auto parsed = parseXml(data, xml, "the FCP XML file"); !parsed) {
        return std::unexpected(parsed.error());
    }
    IrDocument doc;
    Fcp7Parser parser(data, context, doc);
    if (auto r = parser.parse(xml); !r) {
        return std::unexpected(r.error());
    }
    return doc;
}

} // namespace wabcut::interchange::detail
