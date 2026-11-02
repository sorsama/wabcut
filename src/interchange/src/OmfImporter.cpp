// OMF import (0.6, card #68): an OMF Interchange 2.0 file's objects (a Bento container, Bento.h) as the mob graph
// AudioPostImport.h builds the sequence from.
//
// OMF 2.0 has the same model as AAF under other names: CMOB / MMOB / SMOB mobs, MSLT slots with a TRKD track
// description, SEQU sequences of SCLP clips (with their own fade in and out), FILL, TRAN transitions and EFFE effects
// (MonoAudioGain, MonoAudioPan, dissolves) whose ESLT slots hold the input (ArgID -1) and CVAL / VVAL controls. Values
// follow the file's byte order (HEAD:ByteOrder 'MM' or 'II'); the Bento table of contents is always big-endian.
// Encapsulated audio is in HEAD:MediaData (WAVE / AIFC objects keyed by the file source mob's MobID). OMF carries no
// markers; its video tracks are reported and left out (card #68), as are effects other than gain and pan.

#include "AudioPostImport.h"
#include "Bento.h"
#include "TextUtil.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>

namespace wabcut::interchange::detail {

namespace {

using bento::ReadValue;
using graph::Component;
using graph::DataKind;
using graph::FadeCurve;

Error bad(std::string message) { return Error{ErrorCode::ParseError, "OMF: " + std::move(message)}; }

std::string hexKey(std::string_view bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (const char ch : bytes) {
        out.push_back(kHex[(static_cast<std::uint8_t>(ch) >> 4) & 0xF]);
        out.push_back(kHex[static_cast<std::uint8_t>(ch) & 0xF]);
    }
    return out;
}

/// Reads the bytes of a value too large to load (encapsulated media) from where it lies.
class SegmentSource {
public:
    explicit SegmentSource(const BinaryInput& input) : m_bytes(input.bytes), m_file(input.file) {}

    std::expected<void, Error> copy(const std::vector<std::pair<std::uint64_t, std::uint64_t>>& segments,
                                    const std::function<bool(std::string_view)>& sink) const {
        std::ifstream in;
        if (!m_file.empty()) {
            in.open(m_file, std::ios::binary);
            if (!in) {
                return std::unexpected(Error{ErrorCode::IoError, "OMF: cannot read " + utf8FromPath(m_file)});
            }
        }
        std::string block;
        for (const auto& [offset, size] : segments) {
            for (std::uint64_t done = 0; done < size;) {
                const std::uint64_t n = std::min<std::uint64_t>(size - done, 1u << 20);
                if (m_file.empty()) {
                    if (offset + done + n > m_bytes.size()) {
                        return std::unexpected(Error{ErrorCode::ParseError, "OMF: media data lies outside the file"});
                    }
                    block.assign(m_bytes.substr(static_cast<std::size_t>(offset + done), static_cast<std::size_t>(n)));
                } else {
                    block.resize(static_cast<std::size_t>(n));
                    in.seekg(static_cast<std::streamoff>(offset + done));
                    in.read(block.data(), static_cast<std::streamsize>(n));
                    if (in.gcount() != static_cast<std::streamsize>(n)) {
                        return std::unexpected(Error{ErrorCode::IoError, "OMF: the media data cannot be read"});
                    }
                }
                if (!sink(block)) {
                    return std::unexpected(Error{ErrorCode::Cancelled, "OMF: copy stopped"});
                }
                done += n;
            }
        }
        return {};
    }

private:
    std::string m_bytes; // the in-memory file (importData), kept for copies after parsing
    std::filesystem::path m_file;
};

class Reader {
public:
    Reader(const bento::Container& container, const BinaryInput& input, const ParseContext& context)
        : m_c(container), m_source(std::make_shared<SegmentSource>(input)), m_ctx(context) {}

    std::expected<graph::Graph, Error> read();

private:
    const ReadValue* value(std::uint32_t id, std::string_view property) const { return m_c.value(id, property); }
    std::string cls(std::uint32_t id) const {
        const ReadValue* v = value(id, "OMFI:OOBJ:ObjClass");
        return v ? v->data.substr(0, 4) : std::string();
    }
    std::uint64_t unsignedOf(std::string_view b) const {
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < b.size() && i < 8; ++i) {
            const std::size_t k = m_little ? b.size() - 1 - i : i;
            v = (v << 8) | static_cast<std::uint8_t>(b[k]);
        }
        return v;
    }
    /// Int16 / Int32 / Int64 (and unsigned) values by their stored size.
    std::optional<std::int64_t> integer(std::uint32_t id, std::string_view property) const {
        const ReadValue* v = value(id, property);
        if (!v || (v->data.size() != 1 && v->data.size() != 2 && v->data.size() != 4 && v->data.size() != 8)) {
            return std::nullopt;
        }
        const std::uint64_t u = unsignedOf(v->data);
        const std::size_t bits = v->data.size() * 8;
        if (bits < 64 && (u >> (bits - 1)) != 0) {
            return static_cast<std::int64_t>(u | (~std::uint64_t{0} << bits));
        }
        return static_cast<std::int64_t>(u);
    }
    std::optional<std::pair<std::int32_t, std::int32_t>> rational(std::string_view bytes) const {
        if (bytes.size() != 8) {
            return std::nullopt;
        }
        return std::pair{static_cast<std::int32_t>(unsignedOf(bytes.substr(0, 4))),
                         static_cast<std::int32_t>(unsignedOf(bytes.substr(4, 4)))};
    }
    std::string text(std::uint32_t id, std::string_view property) const {
        const ReadValue* v = value(id, property);
        if (!v) {
            return {};
        }
        std::string_view s = v->data;
        if (const auto nul = s.find('\0'); nul != std::string_view::npos) {
            s = s.substr(0, nul);
        }
        return graph::textToUtf8(s);
    }
    std::optional<std::uint32_t> resolveKey(const ReadValue& v, std::string_view key) const {
        if (key.size() != 4) {
            return std::nullopt;
        }
        const auto be = static_cast<std::uint32_t>((static_cast<std::uint8_t>(key[0]) << 24) |
                                                   (static_cast<std::uint8_t>(key[1]) << 16) |
                                                   (static_cast<std::uint8_t>(key[2]) << 8) | static_cast<std::uint8_t>(key[3]));
        const auto le = static_cast<std::uint32_t>((static_cast<std::uint8_t>(key[3]) << 24) |
                                                   (static_cast<std::uint8_t>(key[2]) << 16) |
                                                   (static_cast<std::uint8_t>(key[1]) << 8) | static_cast<std::uint8_t>(key[0]));
        for (const std::uint32_t k : {be, le}) {
            if (const auto target = m_c.resolve(v, k)) {
                return target;
            }
        }
        for (const std::uint32_t k : {be, le}) {
            if (m_c.objects.contains(k)) {
                return k;
            }
        }
        return std::nullopt;
    }
    std::optional<std::uint32_t> ref(std::uint32_t id, std::string_view property) const {
        const ReadValue* v = value(id, property);
        return v ? resolveKey(*v, v->data) : std::nullopt;
    }
    /// omfi:ObjRefArray: a 16-bit count and its references, or (other writers) the references alone.
    std::vector<std::uint32_t> refs(std::uint32_t id, std::string_view property) const {
        std::vector<std::uint32_t> out;
        const ReadValue* v = value(id, property);
        if (!v) {
            return out;
        }
        std::string_view b = v->data;
        if (b.size() % 4 == 2) {
            const std::uint64_t n = unsignedOf(b.substr(0, 2));
            const auto be = static_cast<std::uint64_t>((static_cast<std::uint8_t>(b[0]) << 8) | static_cast<std::uint8_t>(b[1]));
            if (n * 4 + 2 != b.size() && be * 4 + 2 != b.size()) {
                return out;
            }
            b.remove_prefix(2);
        } else if (b.size() % 4 != 0) {
            return out;
        }
        for (std::size_t at = 0; at + 4 <= b.size() && out.size() < 1'000'000; at += 4) {
            if (const auto target = resolveKey(*v, b.substr(at, 4))) {
                out.push_back(*target);
            }
        }
        return out;
    }
    std::string uid(std::uint32_t id, std::string_view property) const {
        const ReadValue* v = value(id, property);
        if (!v || v->data.size() != 12 || std::all_of(v->data.begin(), v->data.end(), [](char ch) { return ch == '\0'; })) {
            return {};
        }
        return hexKey(v->data);
    }
    DataKind dataKind(std::uint32_t id) const;
    graph::Control control(std::uint32_t id) const;
    Component component(std::uint32_t id, const std::string& where, int depth);
    graph::Slot slot(std::uint32_t id, const std::string& where);
    graph::Mob mob(std::uint32_t id, std::size_t index, const std::map<std::string, std::uint32_t>& media);
    void describe(graph::Mob& m, std::uint32_t descriptor);

    const bento::Container& m_c;
    std::shared_ptr<SegmentSource> m_source;
    const ParseContext& m_ctx;
    bool m_little = false;
    std::size_t m_components = 0;
    bool m_tooDeep = false;
};

DataKind Reader::dataKind(std::uint32_t id) const {
    const auto ddef = ref(id, "OMFI:CPNT:DataKind");
    if (!ddef) {
        return DataKind::Other;
    }
    const std::string kind = toLowerAscii(text(*ddef, "OMFI:DDEF:DataKindID"));
    if (kind.find("sound") != std::string::npos) {
        return DataKind::Sound;
    }
    if (kind.find("picture") != std::string::npos || kind.find("matte") != std::string::npos) {
        return DataKind::Picture;
    }
    if (kind.find("timecode") != std::string::npos) {
        return DataKind::Timecode;
    }
    return DataKind::Other;
}

graph::Control Reader::control(std::uint32_t id) const {
    graph::Control c;
    auto number = [&](const ReadValue* v) -> std::optional<double> {
        if (!v) {
            return std::nullopt;
        }
        if (const auto r = rational(v->data); r && r->second != 0) {
            return static_cast<double>(r->first) / static_cast<double>(r->second);
        }
        if (v->data.size() == 4) {
            return static_cast<double>(static_cast<std::int32_t>(unsignedOf(v->data)));
        }
        return std::nullopt;
    };
    const std::string kind = cls(id);
    if (kind == "CVAL") {
        c.constant = number(value(id, "OMFI:CVAL:Value")).value_or(1.0);
        return c;
    }
    if (kind == "VVAL") {
        for (const std::uint32_t point : refs(id, "OMFI:VVAL:PointList")) {
            if (c.points.size() >= 10'000) {
                break;
            }
            const ReadValue* time = value(point, "OMFI:CTLP:Time");
            const auto t = time ? rational(time->data) : std::nullopt;
            const auto v = number(value(point, "OMFI:CTLP:Value"));
            if (t && t->second != 0 && v) {
                c.points.emplace_back(static_cast<double>(t->first) / static_cast<double>(t->second), *v);
            }
        }
        std::stable_sort(c.points.begin(), c.points.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        if (!c.points.empty()) {
            c.constant = c.points.front().second;
        }
    }
    return c;
}

Component Reader::component(std::uint32_t id, const std::string& where, int depth) {
    Component c;
    c.where = where;
    c.data = dataKind(id);
    c.length = std::max<std::int64_t>(0, integer(id, "OMFI:CPNT:Length").value_or(0));
    if (depth > 48 || ++m_components > 4'000'000) {
        m_tooDeep = true;
        c.kind = Component::Kind::Filler;
        return c;
    }
    const std::string kind = cls(id);
    if (kind == "SEQU") {
        c.kind = Component::Kind::Sequence;
        std::size_t i = 0;
        for (const std::uint32_t child : refs(id, "OMFI:SEQU:Components")) {
            c.children.push_back(component(child, where + "/component " + std::to_string(++i), depth + 1));
        }
    } else if (kind == "SCLP") {
        c.kind = Component::Kind::SourceClip;
        c.sourceId = uid(id, "OMFI:SCLP:SourceID");
        c.sourceSlot = static_cast<std::uint32_t>(std::max<std::int64_t>(0, integer(id, "OMFI:SCLP:SourceTrackID").value_or(0)));
        c.startTime = integer(id, "OMFI:SCLP:StartTime").value_or(0);
        c.fadeIn = std::max<std::int64_t>(0, integer(id, "OMFI:SCLP:FadeInLength").value_or(0));
        c.fadeOut = std::max<std::int64_t>(0, integer(id, "OMFI:SCLP:FadeOutLength").value_or(0));
        // omfi:FadeType: 1 linear amplitude, 2 linear power.
        c.fadeInCurve = integer(id, "OMFI:SCLP:FadeInType").value_or(0) == 1 ? FadeCurve::Linear : FadeCurve::Power;
        c.fadeOutCurve = integer(id, "OMFI:SCLP:FadeOutType").value_or(0) == 1 ? FadeCurve::Linear : FadeCurve::Power;
    } else if (kind == "FILL") {
        c.kind = Component::Kind::Filler;
    } else if (kind == "TRAN") {
        c.kind = Component::Kind::Transition;
        c.cutPoint = integer(id, "OMFI:TRAN:CutPoint").value_or(0);
        // OMF dissolves have no curve: Unknown (equal power, what Pro Tools and Premiere fade with).
        if (const auto effect = ref(id, "OMFI:TRAN:Effect")) {
            if (const auto edef = ref(*effect, "OMFI:EFFE:EffectKind")) {
                c.name = text(*edef, "OMFI:EDEF:EffectID");
            }
        }
    } else if (kind == "EFFE") {
        std::string effectId;
        std::string effectName;
        bool timeWarp = false;
        if (const auto edef = ref(id, "OMFI:EFFE:EffectKind")) {
            effectId = text(*edef, "OMFI:EDEF:EffectID");
            effectName = text(*edef, "OMFI:EDEF:EffectName");
            if (const ReadValue* tw = value(*edef, "OMFI:EDEF:IsTimeWarp"); tw && !tw->data.empty()) {
                timeWarp = tw->data.front() != '\0';
            }
        }
        const std::string lower = toLowerAscii(effectId);
        std::optional<std::uint32_t> input;
        std::vector<std::uint32_t> controls;
        for (const std::uint32_t slotId : refs(id, "OMFI:EFFE:EffectSlots")) {
            const auto arg = integer(slotId, "OMFI:ESLT:ArgID");
            const auto argValue = ref(slotId, "OMFI:ESLT:ArgValue");
            if (!arg || !argValue) {
                continue;
            }
            if (*arg == -1) {
                input = *argValue;
            } else if (*arg == 1) {
                controls.push_back(*argValue);
            }
        }
        c.name = effectName.empty() ? effectId : effectName;
        if (lower.find("audiogain") != std::string::npos) {
            c.kind = Component::Kind::Gain;
        } else if (lower.find("audiopan") != std::string::npos) {
            c.kind = Component::Kind::Pan;
        } else {
            c.kind = Component::Kind::Effect;
            c.timeWarp = timeWarp;
        }
        if (c.kind != Component::Kind::Effect) {
            for (const std::uint32_t control : controls) {
                c.controls.push_back(this->control(control));
            }
        }
        if (input) {
            c.children.push_back(component(*input, where + "/input", depth + 1));
        }
    } else if (kind == "SLCT") {
        c.kind = Component::Kind::Selector;
        c.name = "selector";
        if (const auto selected = ref(id, "OMFI:SLCT:Selected")) {
            c.children.push_back(component(*selected, where + "/selected", depth + 1));
        }
        std::size_t i = 0;
        for (const std::uint32_t alt : refs(id, "OMFI:SLCT:Alternates")) {
            c.children.push_back(component(alt, where + "/alternate " + std::to_string(++i), depth + 1));
        }
        c.alternates = c.children.empty() ? 0 : c.children.size() - 1;
    } else if (kind == "NEST") {
        c.kind = Component::Kind::Selector;
        const auto slots = refs(id, "OMFI:NEST:Slots");
        if (!slots.empty()) {
            c.children.push_back(component(slots.back(), where + "/scope " + std::to_string(slots.size()), depth + 1));
        }
    } else if (kind == "MGRP") {
        c.kind = Component::Kind::Selector;
        const auto choices = refs(id, "OMFI:MGRP:Choices");
        if (!choices.empty()) {
            c.children.push_back(component(choices.front(), where + "/choice 1", depth + 1));
        }
    } else if (kind == "TCCP") {
        c.kind = Component::Kind::Timecode;
        c.tcStart = integer(id, "OMFI:TCCP:Start").value_or(0);
        c.fps = static_cast<std::int32_t>(std::clamp<std::int64_t>(integer(id, "OMFI:TCCP:FPS").value_or(0), 0, 1000));
        if (const ReadValue* drop = value(id, "OMFI:TCCP:Drop"); drop && !drop->data.empty()) {
            c.drop = drop->data.front() != '\0';
        }
    } else {
        c.kind = Component::Kind::Other;
        c.name = kind.empty() ? std::string("an object") : kind;
    }
    return c;
}

graph::Slot Reader::slot(std::uint32_t id, const std::string& where) {
    graph::Slot out;
    if (const ReadValue* rate = value(id, "OMFI:MSLT:EditRate")) {
        if (const auto r = rational(rate->data); r && r->first > 0 && r->second > 0) {
            out.editRate = Rate{r->first, r->second};
        }
    }
    if (const auto track = ref(id, "OMFI:MSLT:TrackDesc")) {
        out.id = static_cast<std::uint32_t>(std::max<std::int64_t>(0, integer(*track, "OMFI:TRKD:TrackID").value_or(0)));
        out.name = text(*track, "OMFI:TRKD:TrackName");
        out.physical = static_cast<std::uint32_t>(std::max<std::int64_t>(0, integer(*track, "OMFI:TRKD:PhysicalTrack").value_or(0)));
        out.origin = integer(*track, "OMFI:TRKD:Origin").value_or(0);
    }
    if (const auto segment = ref(id, "OMFI:MSLT:Segment")) {
        out.segment = component(*segment, where, 0);
    } else {
        out.segment.kind = Component::Kind::Filler;
    }
    return out;
}

void Reader::describe(graph::Mob& m, std::uint32_t d) {
    const std::string kind = cls(d);
    const bool file = value(d, "OMFI:MDFL:SampleRate") != nullptr || value(d, "OMFI:MDFL:IsOMFI") != nullptr;
    m.kind = file ? graph::Mob::Kind::File : graph::Mob::Kind::Physical;
    if (const ReadValue* rate = value(d, "OMFI:MDFL:SampleRate")) {
        if (const auto r = rational(rate->data); r && r->first > 0 && r->second > 0) {
            m.sampleRate = Rate{r->first, r->second};
        }
    }
    if (const auto length = integer(d, "OMFI:MDFL:Length"); length && *length > 0) {
        m.length = *length;
    }
    for (const std::string_view summary : {"OMFI:WAVD:Summary", "OMFI:AIFD:Summary"}) {
        if (const ReadValue* v = value(d, summary)) {
            m.audio = true;
            if (const auto h = graph::parseAudioHeader(v->data)) {
                m.channels = h->channels;
                if (!m.sampleRate.isValid() && h->sampleRate > 0) {
                    m.sampleRate = Rate{h->sampleRate, 1};
                }
            }
        }
    }
    if (kind == "TIFD" || kind == "CDCI" || kind == "RGBA" || kind == "JPED" || kind == "DIDD") {
        m.video = true;
        m.width = static_cast<std::int32_t>(std::clamp<std::int64_t>(integer(d, "OMFI:DIDD:StoredWidth").value_or(0), 0, 32768));
        m.height = static_cast<std::int32_t>(std::clamp<std::int64_t>(integer(d, "OMFI:DIDD:StoredHeight").value_or(0), 0, 32768));
    }
    for (const std::uint32_t locator : refs(d, "OMFI:MDES:Locator")) {
        for (const std::string_view property : {"OMFI:NETL:URLString", "OMFI:DOSL:PathName", "OMFI:UNXL:PathName",
                                                "OMFI:WINL:PathName", "OMFI:TXTL:Name", "OMFI:MACL:FileName"}) {
            if (std::string path = text(locator, property); !path.empty() && m.locators.size() < 8) {
                m.locators.push_back(std::move(path));
            }
        }
    }
}

graph::Mob Reader::mob(std::uint32_t id, std::size_t index, const std::map<std::string, std::uint32_t>& media) {
    graph::Mob m;
    m.key = uid(id, "OMFI:MOBJ:MobID");
    m.name = text(id, "OMFI:MOBJ:Name");
    const std::string kind = cls(id);
    const std::string label = kind + " " + std::to_string(index + 1) + (m.name.empty() ? "" : " '" + m.name + "'");
    m.where = label;
    if (kind == "CMOB") {
        m.kind = graph::Mob::Kind::Composition;
    } else if (kind == "MMOB") {
        m.kind = graph::Mob::Kind::Master;
    } else if (kind == "SMOB") {
        m.kind = graph::Mob::Kind::Physical;
        if (const auto d = ref(id, "OMFI:SMOB:MediaDescription")) {
            describe(m, *d);
        }
    }
    std::size_t i = 0;
    for (const std::uint32_t s : refs(id, "OMFI:MOBJ:Slots")) {
        m.slots.push_back(slot(s, label + "/slot " + std::to_string(++i)));
    }
    if (m.kind == graph::Mob::Kind::File) {
        if (const auto it = media.find(m.key); it != media.end()) {
            const std::string dataClass = cls(it->second);
            const ReadValue* data = dataClass == "WAVE"   ? value(it->second, "OMFI:WAVE:AudioData")
                                    : dataClass == "AIFC" ? value(it->second, "OMFI:AIFC:AudioData")
                                                          : nullptr;
            if (!data) {
                m.essenceProblem = dataClass == "WAVE" || dataClass == "AIFC"
                                       ? "its encapsulated audio is missing"
                                       : "its encapsulated media (" + dataClass + ") cannot be played: link the media files instead";
            } else if (!data->immediate && !data->segments.empty()) {
                graph::Essence essence;
                essence.extension = dataClass == "WAVE" ? ".wav" : ".aif";
                essence.size = data->size;
                const auto source = m_source;
                const auto segments = data->segments;
                essence.copy = [source, segments](const std::function<bool(std::string_view)>& sink) {
                    return source->copy(segments, sink);
                };
                m.essence = std::move(essence);
                m.audio = true;
            }
        }
    }
    return m;
}

std::expected<graph::Graph, Error> Reader::read() {
    const std::uint32_t head = bento::kTocObject;
    if (cls(head) != "HEAD") {
        return std::unexpected(bad("the file has no OMF header (it may be a Bento file of another kind)"));
    }
    if (const ReadValue* order = value(head, "OMFI:HEAD:ByteOrder"); order && order->data.size() == 2) {
        m_little = order->data == "II";
    }
    if (const ReadValue* version = value(head, "OMFI:HEAD:Version"); version && !version->data.empty() &&
                                                                      static_cast<std::uint8_t>(version->data[0]) < 2) {
        return std::unexpected(Error{ErrorCode::Unsupported,
                                     "OMF 1.0 files are not supported: export OMF 2.0 from the audio application"});
    }
    std::map<std::string, std::uint32_t> media;
    for (const std::uint32_t md : refs(head, "OMFI:HEAD:MediaData")) {
        if (const std::string key = uid(md, "OMFI:MDAT:MobID"); !key.empty()) {
            media.emplace(key, md);
        }
    }
    graph::Graph g;
    std::set<std::uint32_t> primary;
    for (const std::uint32_t m : refs(head, "OMFI:HEAD:PrimaryMobs")) {
        primary.insert(m);
    }
    std::size_t index = 0;
    for (const std::uint32_t id : refs(head, "OMFI:HEAD:Mobs")) {
        graph::Mob m = mob(id, index++, media);
        if (m.key.empty()) {
            continue;
        }
        m.topLevel = m.kind == graph::Mob::Kind::Composition && primary.contains(id);
        g.add(std::move(m));
    }
    if (m_tooDeep) {
        g.report.add(ReportOutcome::Dropped, ReportCategory::Clip,
                     "the file nests its components too deeply or holds too many: the rest was left out",
                     SourceLocation{m_ctx.fileLabel, {}, 0});
    }
    return g;
}

} // namespace

std::expected<IrDocument, Error> parseOmf(const BinaryInput& input, const ParseContext& context) {
    auto container = input.file.empty() ? bento::read(input.bytes) : bento::readFile(input.file);
    if (!container) {
        const Error& e = container.error();
        return std::unexpected(e.code == ErrorCode::IoError ? e : Error{ErrorCode::ParseError, "OMF: " + e.message});
    }
    Reader reader(*container, input, context);
    auto g = reader.read();
    if (!g) {
        return std::unexpected(g.error());
    }
    graph::BuildOptions build;
    build.format = Format::Omf;
    build.video = false;
    return graph::buildDocument(std::move(*g), context, build);
}

} // namespace wabcut::interchange::detail
