// AAF import (0.6, card #68): an AAF file's object tree (AafReader.h) as the mob graph AudioPostImport.h builds the
// sequence from.
//
// Read from the file only: the Header's ContentStorage (mobs, embedded essence) and Dictionary (the names of data,
// operation, parameter and interpolation definitions, so that definitions the importer does not know by identifier
// are still recognised by name). What the writers put where (checked against files from Pro Tools, Media Composer,
// Premiere Pro and DaVinci Resolve):
//  * clip gain: an OperationGroup "Audio Gain" / "Mono Audio Gain" around the SourceClip, with an Amplitude constant
//    and (Media Composer) a second, varying amplitude for its volume keyframes; track volume and pan: the same
//    groups around the track's whole Sequence;
//  * fades and crossfades: Transitions whose dissolve names its curve by an Avid fade type parameter, a power or log
//    interpolation (equal power) or a linear one;
//  * multichannel tracks: an "Audio Channel Combiner" with one SourceClip per channel; a multichannel file's channel
//    is its file source mob slot's physical track number;
//  * a muted clip (Media Composer): a Selector choosing Filler over it;
//  * subclips and adjusted clips: lower-level compositions the top-level one plays.

#include "AafReader.h"
#include "AudioPostImport.h"
#include "TextUtil.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace wabcut::interchange::detail {

namespace {

using aaf::Auid;
using aaf::ReadObject;
using graph::Component;
using graph::DataKind;
using graph::FadeCurve;

constexpr Auid kDataPicture{0x01030202, 0x0100, 0x0000, {0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01, 0x01}};
constexpr Auid kDataSound{0x01030202, 0x0200, 0x0000, {0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01, 0x01}};
constexpr Auid kDataTimecode{0x01030201, 0x0100, 0x0000, {0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01, 0x01}};
constexpr Auid kDataDescriptive{0x01030201, 0x1000, 0x0000, {0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01, 0x01}};
constexpr Auid kDataLegacyPicture{0x6f3c8ce1, 0x6cef, 0x11d2, {0x80, 0x7d, 0x00, 0x60, 0x08, 0x14, 0x3e, 0x6f}};
constexpr Auid kDataLegacySound{0x78e1ebe1, 0x6cef, 0x11d2, {0x80, 0x7d, 0x00, 0x60, 0x08, 0x14, 0x3e, 0x6f}};
constexpr Auid kDataLegacyTimecode{0x7f275e81, 0x77e5, 0x11d2, {0x80, 0x7f, 0x00, 0x60, 0x08, 0x14, 0x3e, 0x6f}};
constexpr Auid kOpMonoAudioGain{0x9d2ea894, 0x0968, 0x11d3, {0x8a, 0x38, 0x00, 0x50, 0x04, 0x0e, 0xf7, 0xd2}};
constexpr Auid kOpMonoAudioPan{0x9d2ea893, 0x0968, 0x11d3, {0x8a, 0x38, 0x00, 0x50, 0x04, 0x0e, 0xf7, 0xd2}};
constexpr Auid kParamLevel{0xe4962320, 0x2267, 0x11d3, {0x8a, 0x4c, 0x00, 0x50, 0x04, 0x0e, 0xf7, 0xd2}};
constexpr Auid kParamAmplitude{0xe4962321, 0x2267, 0x11d3, {0x8a, 0x4c, 0x00, 0x50, 0x04, 0x0e, 0xf7, 0xd2}};
constexpr Auid kParamPan{0xe4962322, 0x2267, 0x11d3, {0x8a, 0x4c, 0x00, 0x50, 0x04, 0x0e, 0xf7, 0xd2}};
/// Media Composer: a clip's volume keyframes (amplitudes), beside its constant gain.
constexpr Auid kParamAvidVolume{0x47c00f7d, 0xfb55, 0x11d3, {0x9f, 0xf7, 0x00, 0x04, 0xac, 0x96, 0x9f, 0x50}};
/// Media Composer: an audio dissolve's curve (0 linear, 1 equal power).
constexpr Auid kParamAvidFadeType{0x8134fc48, 0x3bd6, 0x4904, {0x95, 0x20, 0x7a, 0x63, 0x60, 0xcc, 0x71, 0x27}};
constexpr Auid kInterpolationLinear{0x5b6c85a4, 0x0ede, 0x11d3, {0x80, 0xa9, 0x00, 0x60, 0x08, 0x14, 0x3e, 0x6f}};
constexpr Auid kInterpolationPower{0xc09153f7, 0xbd18, 0x4e5a, {0xad, 0x09, 0xcb, 0xdd, 0x65, 0x4f, 0xa0, 0x01}};

Error bad(std::string message) { return Error{ErrorCode::ParseError, "AAF: " + std::move(message)}; }

std::string hexKey(std::string_view bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (const char ch : bytes) {
        out.push_back(kHex[(static_cast<std::uint8_t>(ch) >> 4) & 0xF]);
        out.push_back(kHex[static_cast<std::uint8_t>(ch) & 0xF]);
    }
    return out;
}

std::uint32_t le32(std::string_view b, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 3])) << 24);
}

/// A string property as UTF-8 without control characters; empty when absent.
std::string textOrEmpty(const std::optional<std::string>& text) { return text ? graph::textToUtf8(*text) : std::string(); }

/// Bits per sample a sound descriptor (or the first of a multiple descriptor) declares; 16 when none does.
std::int64_t quantizationBits(const ReadObject& d, int depth = 0) {
    if (const auto bits = d.integer("QuantizationBits"); bits && *bits > 0) {
        return *bits;
    }
    if (depth < 4) {
        for (const ReadObject* inner : d.children("FileDescriptors")) {
            if (const auto bits = inner->integer("QuantizationBits"); bits && *bits > 0) {
                return *bits;
            }
        }
    }
    return 16;
}

bool isA(const ReadObject& o, std::string_view cls) {
    const int target = aaf::findClass(cls);
    return o.classIndex >= 0 && target >= 0 && aaf::isA(o.classIndex, target);
}

class Reader {
public:
    Reader(const aaf::ReadFile& file, const ParseContext& context) : m_file(file), m_ctx(context) {}

    std::expected<graph::Graph, Error> read();

private:
    void dictionary(const ReadObject& dict);
    std::string nameOf(const std::map<Auid, std::string>& names, const std::optional<Auid>& id) const {
        if (!id) {
            return {};
        }
        const auto it = names.find(*id);
        return it == names.end() ? std::string() : it->second;
    }
    DataKind dataKind(const ReadObject& o) const;
    std::optional<double> numberOf(const std::pair<Auid, std::string>& value) const;
    graph::Control control(const ReadObject& parameter) const;
    graph::FadeCurve curveOf(const ReadObject& operationGroup) const;
    Component component(const ReadObject& o, const std::string& where, int depth);
    graph::Slot slot(const ReadObject& s, const std::string& where);
    graph::Mob mob(const ReadObject& m, std::size_t index);
    void describe(graph::Mob& mob, const ReadObject& descriptor);

    const aaf::ReadFile& m_file;
    const ParseContext& m_ctx;
    std::map<Auid, std::string> m_dataNames;
    std::map<Auid, std::string> m_operationNames;
    std::map<Auid, bool> m_timeWarp;
    std::map<Auid, std::string> m_interpolationNames;
    std::map<std::string, const ReadObject*> m_essence; ///< MobID key -> EssenceData
    std::map<std::string, std::uint16_t> m_extensionPids; ///< extension property name -> pid
    std::size_t m_components = 0;
    bool m_tooDeep = false;
};

void Reader::dictionary(const ReadObject& dict) {
    auto collect = [&](std::string_view property, std::map<Auid, std::string>& names) {
        for (const ReadObject* d : dict.children(property)) {
            if (const auto id = d->auid("Identification")) {
                names[*id] = textOrEmpty(d->string("Name"));
                if (property == "OperationDefinitions") {
                    m_timeWarp[*id] = d->integer("IsTimeWarp").value_or(0) != 0;
                }
            }
        }
    };
    collect("DataDefinitions", m_dataNames);
    collect("OperationDefinitions", m_operationNames);
    collect("InterpolationDefinitions", m_interpolationNames);
}

DataKind Reader::dataKind(const ReadObject& o) const {
    const auto id = o.weakKey("DataDefinition");
    if (!id) {
        return DataKind::Other;
    }
    if (*id == kDataSound || *id == kDataLegacySound) {
        return DataKind::Sound;
    }
    if (*id == kDataPicture || *id == kDataLegacyPicture) {
        return DataKind::Picture;
    }
    if (*id == kDataTimecode || *id == kDataLegacyTimecode) {
        return DataKind::Timecode;
    }
    if (*id == kDataDescriptive) {
        return DataKind::Descriptive;
    }
    const std::string name = toLowerAscii(nameOf(m_dataNames, id));
    if (name.find("sound") != std::string::npos || name.find("audio") != std::string::npos) {
        return DataKind::Sound;
    }
    if (name.find("picture") != std::string::npos || name.find("video") != std::string::npos ||
        name.find("matte") != std::string::npos) {
        return DataKind::Picture;
    }
    if (name.find("timecode") != std::string::npos) {
        return DataKind::Timecode;
    }
    return DataKind::Other;
}

std::optional<double> Reader::numberOf(const std::pair<Auid, std::string>& value) const {
    const auto& [type, bytes] = value;
    const int index = aaf::findType(type);
    const aaf::TypeDef* t = index >= 0 ? &aaf::types()[static_cast<std::size_t>(index)] : nullptr;
    if ((t && t->name == "Rational") || (!t && bytes.size() == 8)) {
        if (bytes.size() != 8) {
            return std::nullopt;
        }
        const auto num = static_cast<std::int32_t>(le32(bytes, 0));
        const auto den = static_cast<std::int32_t>(le32(bytes, 4));
        if (den == 0) {
            return std::nullopt;
        }
        return static_cast<double>(num) / static_cast<double>(den);
    }
    if (t && t->kind == aaf::TypeKind::Integer && !bytes.empty() && bytes.size() <= 8 && bytes.size() == t->size) {
        std::uint64_t v = 0;
        for (std::size_t i = bytes.size(); i-- > 0;) {
            v = (v << 8) | static_cast<std::uint8_t>(bytes[i]);
        }
        if (t->isSigned && bytes.size() < 8 && (v >> (8 * bytes.size() - 1)) != 0) {
            v |= ~std::uint64_t{0} << (8 * bytes.size());
        }
        return t->isSigned ? static_cast<double>(static_cast<std::int64_t>(v)) : static_cast<double>(v);
    }
    if (!t && bytes.size() == 4) {
        return static_cast<double>(static_cast<std::int32_t>(le32(bytes, 0)));
    }
    return std::nullopt;
}

graph::Control Reader::control(const ReadObject& parameter) const {
    graph::Control c;
    if (const auto value = parameter.indirect("Value")) {
        c.constant = numberOf(*value).value_or(1.0);
        return c;
    }
    for (const ReadObject* point : parameter.children("PointList")) {
        if (c.points.size() >= 10'000) {
            break;
        }
        const auto time = point->rational("Time");
        const auto value = point->indirect("Value");
        if (!time || time->second == 0 || !value) {
            continue;
        }
        if (const auto v = numberOf(*value)) {
            c.points.emplace_back(static_cast<double>(time->first) / static_cast<double>(time->second), *v);
        }
    }
    std::stable_sort(c.points.begin(), c.points.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (!c.points.empty()) {
        c.constant = c.points.front().second;
    }
    return c;
}

FadeCurve Reader::curveOf(const ReadObject& og) const {
    for (const ReadObject* p : og.children("Parameters")) {
        if (p->auid("Definition") == kParamAvidFadeType) {
            if (const auto v = p->indirect("Value")) {
                return numberOf(*v).value_or(0.0) != 0.0 ? FadeCurve::Power : FadeCurve::Linear;
            }
        }
    }
    for (const ReadObject* p : og.children("Parameters")) {
        if (p->auid("Definition") != kParamLevel) {
            continue;
        }
        const auto id = p->weakKey("Interpolation");
        if (id == kInterpolationLinear) {
            return FadeCurve::Linear;
        }
        if (id == kInterpolationPower) {
            return FadeCurve::Power;
        }
        const std::string name = toLowerAscii(nameOf(m_interpolationNames, id));
        if (name.find("power") != std::string::npos || name.find("log") != std::string::npos) {
            return FadeCurve::Power;
        }
        if (name.find("linear") != std::string::npos) {
            return FadeCurve::Linear;
        }
    }
    return FadeCurve::Unknown;
}

Component Reader::component(const ReadObject& o, const std::string& where, int depth) {
    Component c;
    c.where = where;
    c.data = dataKind(o);
    c.length = o.integer("Length").value_or(0);
    if (depth > 48 || ++m_components > 4'000'000) {
        m_tooDeep = true;
        c.kind = Component::Kind::Filler;
        return c;
    }
    const std::string cls(o.className());
    auto childrenOf = [&](std::string_view property, std::string_view label) {
        std::size_t i = 0;
        for (const ReadObject* child : o.children(property)) {
            c.children.push_back(component(*child, where + "/" + std::string(label) + " " + std::to_string(++i), depth + 1));
        }
    };
    if (isA(o, "Sequence")) {
        c.kind = Component::Kind::Sequence;
        childrenOf("Components", "component");
    } else if (isA(o, "SourceClip")) {
        c.kind = Component::Kind::SourceClip;
        if (const auto id = o.mobId("SourceID")) {
            const bool null = std::all_of(id->begin(), id->end(), [](std::uint8_t b) { return b == 0; });
            if (!null) {
                c.sourceId = hexKey(std::string_view(reinterpret_cast<const char*>(id->data()), id->size()));
            }
        }
        c.sourceSlot = static_cast<std::uint32_t>(o.integer("SourceMobSlotID").value_or(0));
        c.startTime = o.integer("StartTime").value_or(0);
        c.fadeIn = std::max<std::int64_t>(0, o.integer("FadeInLength").value_or(0));
        c.fadeOut = std::max<std::int64_t>(0, o.integer("FadeOutLength").value_or(0));
        // FadeType: 1 linear amplitude, 2 linear power.
        c.fadeInCurve = o.integer("FadeInType").value_or(0) == 1 ? FadeCurve::Linear : FadeCurve::Power;
        c.fadeOutCurve = o.integer("FadeOutType").value_or(0) == 1 ? FadeCurve::Linear : FadeCurve::Power;
    } else if (isA(o, "Filler")) {
        c.kind = Component::Kind::Filler;
    } else if (isA(o, "Transition")) {
        c.kind = Component::Kind::Transition;
        c.cutPoint = o.integer("CutPoint").value_or(0);
        if (const ReadObject* og = o.child("OperationGroup")) {
            c.curve = curveOf(*og);
            c.name = nameOf(m_operationNames, og->weakKey("Operation"));
        }
    } else if (isA(o, "OperationGroup")) {
        const auto op = o.weakKey("Operation");
        c.name = nameOf(m_operationNames, op);
        const std::string name = toLowerAscii(c.name);
        if (op == kOpMonoAudioGain || name.find("gain") != std::string::npos) {
            c.kind = Component::Kind::Gain;
            for (const ReadObject* p : o.children("Parameters")) {
                const auto def = p->auid("Definition");
                if (def == kParamAmplitude || def == kParamAvidVolume) {
                    c.controls.push_back(control(*p));
                }
            }
        } else if (op == kOpMonoAudioPan || name.find("pan") != std::string::npos) {
            c.kind = Component::Kind::Pan;
            for (const ReadObject* p : o.children("Parameters")) {
                if (p->auid("Definition") == kParamPan) {
                    c.controls.push_back(control(*p));
                    break;
                }
            }
        } else if (name.find("channel combiner") != std::string::npos) {
            c.kind = Component::Kind::Combiner;
        } else {
            c.kind = Component::Kind::Effect;
            c.timeWarp = op && m_timeWarp.contains(*op) && m_timeWarp.at(*op);
            if (c.name.empty()) {
                c.name = "effect";
            }
        }
        childrenOf("InputSegments", "input");
    } else if (isA(o, "Selector")) {
        c.kind = Component::Kind::Selector;
        c.name = "selector";
        if (const ReadObject* selected = o.child("Selected")) {
            c.children.push_back(component(*selected, where + "/selected", depth + 1));
        }
        childrenOf("Alternates", "alternate");
        c.alternates = c.children.empty() ? 0 : c.children.size() - 1;
    } else if (isA(o, "EssenceGroup")) {
        c.kind = Component::Kind::Selector;
        childrenOf("Choices", "choice");
        c.children.resize(std::min<std::size_t>(c.children.size(), 1));
    } else if (isA(o, "NestedScope")) {
        // Its last slot is what it shows (the others are only referred to by ScopeReferences in it).
        c.kind = Component::Kind::Selector;
        const auto slots = o.children("Slots");
        if (!slots.empty()) {
            c.children.push_back(component(*slots.back(), where + "/scope " + std::to_string(slots.size()), depth + 1));
        }
    } else if (isA(o, "Timecode")) {
        c.kind = Component::Kind::Timecode;
        c.tcStart = o.integer("Start").value_or(0);
        c.fps = static_cast<std::int32_t>(std::clamp<std::int64_t>(o.integer("FPS").value_or(0), 0, 1000));
        c.drop = o.integer("Drop").value_or(0) != 0;
    } else if (isA(o, "Event")) {
        c.kind = Component::Kind::Marker;
        c.position = o.integer("Position").value_or(0);
        c.comment = textOrEmpty(o.string("Comment"));
        if (const auto pid = m_extensionPids.find("CommentMarkerColor"); pid != m_extensionPids.end()) {
            if (const ReadObject::Property* p = o.find(pid->second); p && p->data.size() == 6) {
                const auto channel = [&](std::size_t at) {
                    return static_cast<int>((static_cast<std::uint8_t>(p->data[at]) | (static_cast<std::uint8_t>(p->data[at + 1]) << 8)) >> 8);
                };
                c.color = std::array<int, 3>{channel(0), channel(2), channel(4)};
            }
        }
    } else {
        c.kind = Component::Kind::Other;
        c.name = cls;
    }
    return c;
}

graph::Slot Reader::slot(const ReadObject& s, const std::string& where) {
    graph::Slot out;
    out.id = static_cast<std::uint32_t>(s.integer("SlotID").value_or(0));
    out.name = textOrEmpty(s.string("SlotName"));
    out.physical = static_cast<std::uint32_t>(std::max<std::int64_t>(0, s.integer("PhysicalTrackNumber").value_or(0)));
    out.origin = s.integer("Origin").value_or(0);
    out.event = isA(s, "EventMobSlot");
    if (const auto rate = s.rational("EditRate"); rate && rate->first > 0 && rate->second > 0) {
        out.editRate = Rate{rate->first, rate->second};
    }
    if (const ReadObject* segment = s.child("Segment")) {
        out.segment = component(*segment, where, 0);
    } else {
        out.segment.kind = Component::Kind::Filler;
    }
    return out;
}

void Reader::describe(graph::Mob& m, const ReadObject& d) {
    if (isA(d, "MultipleDescriptor")) {
        for (const ReadObject* inner : d.children("FileDescriptors")) {
            describe(m, *inner);
        }
        m.kind = graph::Mob::Kind::File;
    }
    if (isA(d, "FileDescriptor")) {
        m.kind = graph::Mob::Kind::File;
        if (const auto rate = d.rational("SampleRate"); rate && rate->first > 0 && rate->second > 0 && !m.sampleRate.isValid()) {
            m.sampleRate = Rate{rate->first, rate->second};
        }
        if (const auto length = d.integer("Length"); length && *length > 0 && !m.length) {
            m.length = *length;
        }
    } else if (!isA(d, "MultipleDescriptor")) {
        m.kind = graph::Mob::Kind::Physical;
    }
    if (isA(d, "SoundDescriptor") || isA(d, "WAVEDescriptor") || isA(d, "AIFCDescriptor")) {
        m.audio = true;
        if (const auto channels = d.integer("Channels"); channels && *channels > 0) {
            m.channels = static_cast<std::int32_t>(std::min<std::int64_t>(*channels, 64));
        }
        if (const auto rate = d.rational("AudioSamplingRate"); rate && rate->first > 0 && rate->second > 0) {
            m.sampleRate = Rate{rate->first, rate->second};
        }
        if (const ReadObject::Property* summary = d.find("Summary")) {
            if (const auto h = graph::parseAudioHeader(summary->data)) {
                m.channels = h->channels;
                if (h->sampleRate > 0 && !m.sampleRate.isValid()) {
                    m.sampleRate = Rate{h->sampleRate, 1};
                }
            }
        }
    }
    if (isA(d, "DigitalImageDescriptor")) {
        m.video = true;
        m.width = static_cast<std::int32_t>(std::clamp<std::int64_t>(d.integer("StoredWidth").value_or(0), 0, 32768));
        m.height = static_cast<std::int32_t>(std::clamp<std::int64_t>(d.integer("StoredHeight").value_or(0), 0, 32768));
    }
    for (const ReadObject* locator : d.children("Locator")) {
        std::string text = textOrEmpty(locator->string("URLString"));
        if (text.empty()) {
            text = textOrEmpty(locator->string("Name")); // TextLocator
        }
        if (!text.empty() && m.locators.size() < 8) {
            m.locators.push_back(std::move(text));
        }
    }
}

graph::Mob Reader::mob(const ReadObject& o, std::size_t index) {
    graph::Mob m;
    if (const auto id = o.mobId("MobID")) {
        m.key = hexKey(std::string_view(reinterpret_cast<const char*>(id->data()), id->size()));
    }
    m.name = textOrEmpty(o.string("Name"));
    const std::string label = std::string(o.className()) + " " + std::to_string(index + 1) + (m.name.empty() ? "" : " '" + m.name + "'");
    m.where = label;
    if (isA(o, "CompositionMob")) {
        m.kind = graph::Mob::Kind::Composition;
        if (const auto usage = o.auid("UsageCode")) {
            const int type = aaf::findType("UsageType");
            std::string usageName;
            if (type >= 0) {
                for (const aaf::ExtEnumElement& e : aaf::types()[static_cast<std::size_t>(type)].extEnumElements) {
                    if (e.value == *usage) {
                        usageName = std::string(e.name);
                    }
                }
            }
            m.topLevel = usageName == "Usage_TopLevel";
            m.lowerLevel = !usageName.empty() && !m.topLevel;
        }
    } else if (isA(o, "MasterMob")) {
        m.kind = graph::Mob::Kind::Master;
    } else if (isA(o, "SourceMob")) {
        m.kind = graph::Mob::Kind::Physical;
        if (const ReadObject* d = o.child("EssenceDescription")) {
            describe(m, *d);
        }
    }
    std::size_t i = 0;
    for (const ReadObject* s : o.children("Slots")) {
        m.slots.push_back(slot(*s, label + "/slot " + std::to_string(++i)));
    }
    // Embedded essence.
    if (m.kind == graph::Mob::Kind::File) {
        if (const auto e = m_essence.find(m.key); e != m_essence.end()) {
            const ReadObject::Property* data = e->second->find("Data");
            if (!m.audio) {
                m.essenceProblem = "its embedded video cannot be played: link the media files instead";
            } else if (data && data->streamEntry != static_cast<std::size_t>(-1) && m_file.container) {
                const auto container = m_file.container;
                const std::size_t entry = data->streamEntry;
                graph::Essence essence;
                essence.size = data->streamSize;
                std::string head;
                auto sniff = container->copy(entry, [&](std::string_view block) {
                    head.assign(block.substr(0, 16));
                    return false;
                });
                static_cast<void>(sniff); // stopping after the first block is the point
                if (head.starts_with("RIFF") || head.starts_with("RF64")) {
                    essence.extension = ".wav";
                } else if (head.starts_with("FORM")) {
                    essence.extension = ".aif";
                } else if (m.channels > 0 && m.sampleRate.isValid() && m.sampleRate.den == 1) {
                    // Raw PCM (Avid's and Pro Tools' internal PCM): a WAV header in front.
                    const ReadObject* descriptor = o.child("EssenceDescription");
                    const auto bits = std::clamp<std::int64_t>(descriptor ? quantizationBits(*descriptor) : 16, 8, 32);
                    essence.extension = ".wav";
                    essence.header = graph::wavHeader(m.channels, m.sampleRate.num, static_cast<std::int32_t>(bits), essence.size);
                } else {
                    m.essenceProblem = "its embedded audio is in a form Wabcut cannot read";
                }
                if (m.essenceProblem.empty()) {
                    essence.copy = [container, entry](const std::function<bool(std::string_view)>& sink) {
                        return container->copy(entry, sink);
                    };
                    m.essence = std::move(essence);
                }
            }
        }
    }
    return m;
}

std::expected<graph::Graph, Error> Reader::read() {
    const ReadObject* header = m_file.root.child("Header");
    if (!header) {
        return std::unexpected(bad("the file has no Header"));
    }
    // Extension property names (Avid's marker colour) from the MetaDictionary.
    if (const ReadObject::Property* meta = m_file.root.find(std::uint16_t{0x0001}); meta && !meta->objects.empty()) {
        if (const ReadObject::Property* classes = meta->objects.front().find(std::uint16_t{0x0003})) {
            for (const ReadObject& cls : classes->objects) {
                if (const ReadObject::Property* props = cls.find(std::uint16_t{0x0009})) {
                    for (const ReadObject& p : props->objects) {
                        const ReadObject::Property* name = p.find(std::uint16_t{0x0006});
                        const ReadObject::Property* pid = p.find(std::uint16_t{0x000d});
                        if (name && pid && pid->data.size() == 2) {
                            const auto local = static_cast<std::uint16_t>(static_cast<std::uint8_t>(pid->data[0]) |
                                                                          (static_cast<std::uint8_t>(pid->data[1]) << 8));
                            m_extensionPids.emplace(aaf::utf8FromUtf16le(name->data), local);
                        }
                    }
                }
            }
        }
    }
    if (const ReadObject* dict = header->child("Dictionary")) {
        dictionary(*dict);
    }
    const ReadObject* content = header->child("Content");
    if (!content) {
        return std::unexpected(bad("the file has no content storage"));
    }
    for (const ReadObject* e : content->children("EssenceData")) {
        if (const auto id = e->mobId("MobID")) {
            m_essence.emplace(hexKey(std::string_view(reinterpret_cast<const char*>(id->data()), id->size())), e);
        }
    }
    graph::Graph g;
    std::size_t index = 0;
    for (const ReadObject* m : content->children("Mobs")) {
        graph::Mob built = mob(*m, index++);
        if (built.key.empty()) {
            continue;
        }
        g.add(std::move(built));
    }
    if (m_tooDeep) {
        g.report.add(ReportOutcome::Dropped, ReportCategory::Clip,
                     "the file nests its components too deeply or holds too many: the rest was left out",
                     SourceLocation{m_ctx.fileLabel, {}, 0});
    }
    return g;
}

} // namespace

std::expected<IrDocument, Error> parseAaf(const BinaryInput& input, const ParseContext& context) {
    aaf::ReadOptions options;
    options.loadStreams = false;
    auto file = input.file.empty() ? aaf::readAaf(std::string(input.bytes), options) : aaf::readAafFile(input.file, options);
    if (!file) {
        const Error& e = file.error();
        if (e.code == ErrorCode::IoError) {
            return std::unexpected(e);
        }
        return std::unexpected(Error{ErrorCode::ParseError, e.message.starts_with("AAF") ? e.message : "AAF: " + e.message});
    }
    Reader reader(*file, context);
    auto g = reader.read();
    if (!g) {
        return std::unexpected(g.error());
    }
    graph::BuildOptions build;
    build.format = Format::Aaf;
    return graph::buildDocument(std::move(*g), context, build);
}

} // namespace wabcut::interchange::detail
