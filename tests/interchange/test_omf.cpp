// OMF 2.0 export for audio post (P.2): the Bento container layer (Bento.h) and the exporter (AudioPostExport.h), read
// back with the module's own Bento reader. Golden file byte for byte, the composition's structure (tracks, clips,
// handles, gain, fades, pan), encapsulated and separate audio, and malformed containers.

#include "AudioPostTestSupport.h"
#include "InterchangeTestSupport.h"

#include "Bento.h"

#include <wabcut/interchange/AudioPostExport.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <set>

using namespace wabcut;
using namespace wabcut::interchange;
namespace it = wabcut::interchange::test;
namespace bento = wabcut::interchange::detail::bento;
using it::audioPostTimeline;
using it::fixedTime;
using it::signal;
using it::TestRenderer;

namespace {

std::uint32_t be32(std::string_view b, std::size_t at = 0) {
    REQUIRE(b.size() >= at + 4);
    return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at])) << 24) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 2])) << 8) | static_cast<std::uint8_t>(b[at + 3]);
}
std::uint16_t be16(std::string_view b, std::size_t at = 0) {
    REQUIRE(b.size() >= at + 2);
    return static_cast<std::uint16_t>((static_cast<std::uint8_t>(b[at]) << 8) | static_cast<std::uint8_t>(b[at + 1]));
}

OmfExportOptions fixedOmfOptions() {
    OmfExportOptions o;
    o.timestamp = fixedTime();
    o.productVersion = "0.2.0";
    o.audio.handleFrames = 2;
    return o;
}

/// The OMF objects of a container, by class and property name.
struct Omf {
    bento::Container c;

    std::string cls(std::uint32_t id) const {
        const bento::ReadValue* v = c.value(id, "OMFI:OOBJ:ObjClass");
        return v ? v->data : std::string();
    }
    const bento::ReadValue& value(std::uint32_t id, std::string_view property) const {
        const bento::ReadValue* v = c.value(id, property);
        INFO(property);
        REQUIRE(v != nullptr);
        return *v;
    }
    bool has(std::uint32_t id, std::string_view property) const { return c.value(id, property) != nullptr; }
    std::string str(std::uint32_t id, std::string_view property) const {
        std::string s = value(id, property).data;
        if (!s.empty() && s.back() == '\0') {
            s.pop_back();
        }
        return s;
    }
    std::int64_t i32(std::uint32_t id, std::string_view property) const {
        return static_cast<std::int32_t>(be32(value(id, property).data));
    }
    std::uint32_t ref(std::uint32_t id, std::string_view property) const {
        const bento::ReadValue& v = value(id, property);
        const auto target = c.resolve(v, be32(v.data));
        REQUIRE(target.has_value());
        return *target;
    }
    std::vector<std::uint32_t> refs(std::uint32_t id, std::string_view property) const {
        const bento::ReadValue& v = value(id, property);
        const std::size_t n = be16(v.data);
        REQUIRE(v.data.size() == 2 + 4 * n);
        std::vector<std::uint32_t> out;
        for (std::size_t k = 0; k < n; ++k) {
            const auto target = c.resolve(v, be32(v.data, 2 + 4 * k));
            REQUIRE(target.has_value());
            out.push_back(*target);
        }
        return out;
    }
    std::uint32_t mobByUid(const std::string& uid) const {
        for (const std::uint32_t m : refs(bento::kTocObject, "OMFI:HEAD:Mobs")) {
            if (value(m, "OMFI:MOBJ:MobID").data == uid) {
                return m;
            }
        }
        return 0;
    }
    std::string dataKind(std::uint32_t id, std::string_view property = "OMFI:CPNT:DataKind") const {
        return str(ref(id, property), "OMFI:DDEF:DataKindID");
    }
    std::string effectOf(std::uint32_t effe) const { return str(ref(effe, "OMFI:EFFE:EffectKind"), "OMFI:EDEF:EffectID"); }
    /// The segment of an EFFE's slot `argId`.
    std::uint32_t slotValue(std::uint32_t effe, std::int32_t argId) const {
        for (const std::uint32_t s : refs(effe, "OMFI:EFFE:EffectSlots")) {
            if (i32(s, "OMFI:ESLT:ArgID") == argId) {
                return ref(s, "OMFI:ESLT:ArgValue");
            }
        }
        FAIL("no effect slot " << argId);
        return 0;
    }
    /// "FILL 7680 | SCLP 11520 @3840 'C A2 Left' in 5760/1 | TRAN 7680 cut 3840 | gain(SCLP ...)".
    std::string describe(std::uint32_t sequ) const {
        std::string out;
        for (const std::uint32_t component : refs(sequ, "OMFI:SEQU:Components")) {
            if (!out.empty()) {
                out += " | ";
            }
            std::uint32_t clip = component;
            std::string gain;
            if (cls(component) == "EFFE") {
                CHECK(effectOf(component) == "omfi:effect:MonoAudioGain");
                const std::uint32_t control = slotValue(component, 1);
                gain = cls(control) == "CVAL" ? " gain" : " gain*" + std::to_string(refs(control, "OMFI:VVAL:PointList").size());
                clip = slotValue(component, -1);
            }
            out += cls(clip) + " " + std::to_string(i32(clip, "OMFI:CPNT:Length"));
            if (cls(clip) == "SCLP") {
                const std::uint32_t master = mobByUid(value(clip, "OMFI:SCLP:SourceID").data);
                out += " @" + std::to_string(i32(clip, "OMFI:SCLP:StartTime")) + " '" +
                       (master ? str(master, "OMFI:MOBJ:Name") : std::string("?")) + "'";
                if (has(clip, "OMFI:SCLP:FadeInLength")) {
                    out += " in " + std::to_string(i32(clip, "OMFI:SCLP:FadeInLength")) + "/" +
                           std::to_string(be16(value(clip, "OMFI:SCLP:FadeInType").data));
                }
                if (has(clip, "OMFI:SCLP:FadeOutLength")) {
                    out += " out " + std::to_string(i32(clip, "OMFI:SCLP:FadeOutLength")) + "/" +
                           std::to_string(be16(value(clip, "OMFI:SCLP:FadeOutType").data));
                }
            }
            if (cls(clip) == "TRAN") {
                out += " cut " + std::to_string(i32(clip, "OMFI:TRAN:CutPoint"));
                CHECK(effectOf(ref(clip, "OMFI:TRAN:Effect")) == "omfi:effect:SimpleMonoAudioDissolve");
            }
            out += gain;
        }
        return out;
    }
};

Omf readOmf(const std::filesystem::path& file) {
    auto c = bento::read(it::readFile(file));
    REQUIRE(c.has_value());
    return Omf{std::move(*c)};
}

/// The composition's slot named `name` (its track description's name).
std::uint32_t slotNamed(const Omf& omf, std::uint32_t cmob, std::string_view name) {
    for (const std::uint32_t s : omf.refs(cmob, "OMFI:MOBJ:Slots")) {
        const std::uint32_t td = omf.ref(s, "OMFI:MSLT:TrackDesc");
        if (omf.has(td, "OMFI:TRKD:TrackName") && omf.str(td, "OMFI:TRKD:TrackName") == name) {
            return s;
        }
    }
    FAIL("no slot " << name);
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Bento

TEST_CASE("OMF: Bento containers write and read back (P.2)", "[interchange][omf]") {
    bento::Builder b;
    const std::uint32_t typeInt = b.typeId("omfi:Int32");
    const std::uint32_t typeString = b.typeId("omfi:String");
    const std::uint32_t typeRef = b.typeId("omfi:ObjRef");
    const std::uint32_t propA = b.propertyId("TEST:A");
    const std::uint32_t propB = b.propertyId("TEST:B");
    CHECK(b.typeId("omfi:Int32") == typeInt); // declared once
    const std::uint32_t one = b.newObject();
    const std::uint32_t two = b.newObject();
    b.set(one, propA, typeInt, std::string("\x00\x00\x01\x02", 4));  // immediate
    b.set(one, propB, typeString, "a longer string\0" + std::string(1, '\0'));
    b.setReference(two, propA, typeRef, std::string("\x00\x01\x00\x00", 4) /* key = object 0x10000 */, {0x10000});
    b.set(bento::kTocObject, propB, typeString, std::string("hd", 2));
    const auto bytes = b.toBytes();
    REQUIRE(bytes.has_value());
    // The label: magic, flags 0, 1 KiB TOC blocks, version 2.0, TOC offset and size, at the very end.
    const std::string_view label = std::string_view(*bytes).substr(bytes->size() - 24);
    CHECK(label.substr(0, 8) == std::string_view("\xA4\x43\x4D\xA5\x48\x64\x72\xD7", 8));
    CHECK(be16(label, 10) == 1);
    CHECK(be16(label, 12) == 2);
    CHECK(be16(label, 14) == 0);
    const auto read = bento::read(*bytes);
    REQUIRE(read.has_value());
    CHECK(read->names.at(typeInt) == "omfi:Int32");
    CHECK(read->names.at(propB) == "TEST:B");
    const bento::ReadValue* a = read->value(one, "TEST:A");
    REQUIRE(a != nullptr);
    CHECK(a->immediate);
    CHECK(a->data == std::string("\x00\x00\x01\x02", 4));
    CHECK(read->value(one, propB)->data == std::string("a longer string") + std::string(1, '\0'));
    const bento::ReadValue* r = read->value(two, propA);
    REQUIRE(r != nullptr);
    CHECK(read->resolve(*r, 0x10000) == std::optional<std::uint32_t>(0x10000));
    CHECK_FALSE(read->resolve(*r, 0x10001).has_value());
    // The TOC object's own values: the seed, the TOC and the whole container.
    CHECK(be32(read->value(bento::kTocObject, bento::kPropTocSeed)->data) == b.nextId());
    CHECK(be32(read->value(bento::kTocObject, bento::kPropTocMinSeed)->data) == bento::kFirstUserId);
    CHECK(read->value(bento::kTocObject, bento::kPropTocContainer)->size == bytes->size());
    CHECK(read->value(bento::kTocObject, propB)->data == "hd");
}

TEST_CASE("OMF: a TOC over many blocks keeps every object whole (P.2)", "[interchange][omf]") {
    bento::Builder b;
    const std::uint32_t type = b.typeId("omfi:Int32");
    std::vector<std::uint32_t> props;
    for (int i = 0; i < 12; ++i) {
        props.push_back(b.propertyId("TEST:P" + std::to_string(i)));
    }
    std::vector<std::uint32_t> objects;
    for (int i = 0; i < 400; ++i) {
        const std::uint32_t o = b.newObject();
        objects.push_back(o);
        for (std::size_t k = 0; k <= static_cast<std::size_t>(i % 12); ++k) {
            std::string v(4, '\0');
            v[3] = static_cast<char>(k);
            v[2] = static_cast<char>(i & 0xFF);
            b.set(o, props[k], type, v);
        }
    }
    // One object longer than a TOC block (it is split, each part opening with NewObject).
    const std::uint32_t big = b.newObject();
    for (int k = 0; k < 120; ++k) {
        b.set(big, b.propertyId("TEST:BIG" + std::to_string(k)), type, std::string(8, static_cast<char>(k)));
    }
    const auto bytes = b.toBytes();
    REQUIRE(bytes.has_value());
    const std::string_view label = std::string_view(*bytes).substr(bytes->size() - 24);
    CHECK(be32(label, 20) > 4 * bento::kTocBlockSize);
    const auto read = bento::read(*bytes);
    REQUIRE(read.has_value());
    for (int i = 0; i < 400; ++i) {
        const auto& o = read->objects.at(objects[static_cast<std::size_t>(i)]);
        REQUIRE(o.size() == static_cast<std::size_t>(i % 12) + 1);
        CHECK(static_cast<std::uint8_t>(o.at(props[0]).front().data[2]) == (i & 0xFF));
    }
    CHECK(read->objects.at(big).size() == 120);
    // No object's entries straddle a block: each block starts with NewObject.
    const std::uint32_t tocOffset = be32(label, 16);
    const std::uint32_t tocSize = be32(label, 20);
    for (std::uint32_t at = 0; at < tocSize; at += bento::kTocBlockSize) {
        CHECK(static_cast<std::uint8_t>((*bytes)[tocOffset + at]) == 1);
    }
}

TEST_CASE("OMF: malformed Bento containers are refused without crashing (P.2)", "[interchange][omf]") {
    bento::Builder b;
    const std::uint32_t type = b.typeId("omfi:String");
    std::uint32_t first = 0;
    for (int i = 0; i < 60; ++i) {
        const std::uint32_t object = b.newObject();
        const std::uint32_t property = b.propertyId("TEST:N" + std::to_string(i % 7));
        b.set(object, property, type, std::string(static_cast<std::size_t>(i % 9), 'x'));
        first = i == 0 ? object : first;
    }
    const std::string good = *b.toBytes();
    const auto clean = bento::read(good);
    REQUIRE(clean.has_value());
    REQUIRE(clean->value(first, "TEST:N0") != nullptr);
    CHECK(clean->value(first, "TEST:N0")->data.empty()); // a zero-length value (Immediate0) reads back
    CHECK_FALSE(bento::read("").has_value());
    CHECK_FALSE(bento::read(std::string(24, '\0')).has_value());
    std::string badToc = good;
    badToc[badToc.size() - 4] = '\x7F'; // TOC size far past the end
    CHECK_FALSE(bento::read(badToc).has_value());
    std::mt19937 rng(77);
    int parsed = 0;
    for (int round = 0; round < 4000; ++round) {
        std::string data = good;
        const int edits = 1 + static_cast<int>(rng() % 5);
        for (int e = 0; e < edits; ++e) {
            const std::size_t at = rng() % data.size();
            data[at] = static_cast<char>(rng());
        }
        if (rng() % 8 == 0) {
            data.erase(rng() % data.size(), rng() % 64);
        }
        const auto read = bento::read(data);
        parsed += read.has_value() ? 1 : 0;
    }
    CHECK(parsed > 0); // edits to value data leave a readable container
}

// ---------------------------------------------------------------------------------------------------------------------
// The exporter

TEST_CASE("OMF: export golden file with encapsulated audio (P.2)", "[interchange][omf]") {
    const auto tl = audioPostTimeline();
    const auto dir = it::scratchDir("omf-golden");
    TestRenderer renderer;
    AudioPostHost host;
    host.audio = &renderer;
    const auto result = exportOmfToFile(tl.state, tl.seq, dir / "audiopost.omf", fixedOmfOptions(), host);
    REQUIRE(result.has_value());
    const std::string bytes = it::readFile(dir / "audiopost.omf");
    CHECK(result->bytes == bytes.size());
    const std::filesystem::path golden = it::fixturePath("export/audiopost.omf");
    if (std::getenv("WABCUT_UPDATE_GOLDEN") != nullptr) {
        it::writeFile(golden, bytes);
    }
    const std::string want = std::filesystem::exists(golden) ? it::readFile(golden) : std::string();
    if (bytes != want) {
        it::writeFile(std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "export" / "audiopost.omf", bytes);
    }
    CHECK(bytes == want);
    const auto again = exportOmfToFile(tl.state, tl.seq, dir / "again.omf", fixedOmfOptions(), host);
    REQUIRE(again.has_value());
    CHECK(it::readFile(dir / "again.omf") == bytes);
    CHECK_FALSE(std::filesystem::exists(dir / "audiopost.omf.wabcut-partial-audio"));
}

TEST_CASE("OMF: the composition reads back with tracks clips handles gain fades and pan (P.2)", "[interchange][omf]") {
    const auto tl = audioPostTimeline();
    const auto dir = it::scratchDir("omf-structure");
    TestRenderer renderer;
    AudioPostHost host;
    host.audio = &renderer;
    OmfExportOptions options = fixedOmfOptions();
    options.title = "Reel 1 Mix";
    const auto result = exportOmfToFile(tl.state, tl.seq, dir / "mix.omf", options, host);
    REQUIRE(result.has_value());
    CHECK(result->tracks == 4);
    CHECK(result->clips == 6);
    CHECK(it::hasEntry(result->report, ReportOutcome::Dropped, ReportCategory::Marker, "no markers"));
    CHECK(it::hasEntry(result->report, ReportOutcome::Dropped, ReportCategory::Clip, "audio only"));
    CHECK(it::hasEntry(result->report, ReportOutcome::Approximated, ReportCategory::Transition, "no curve"));
    const Omf omf = readOmf(dir / "mix.omf");
    constexpr std::uint32_t head = bento::kTocObject;
    CHECK(omf.cls(head) == "HEAD");
    CHECK(omf.value(head, "OMFI:HEAD:ByteOrder").data == "MM");
    CHECK(omf.value(head, "OMFI:HEAD:Version").data == std::string("\x02\x00", 2));
    const auto mobs = omf.refs(head, "OMFI:HEAD:Mobs");
    CHECK(mobs.size() == 13); // the composition, and a master and a file source mob per mono file
    const auto primary = omf.refs(head, "OMFI:HEAD:PrimaryMobs");
    REQUIRE(primary.size() == 1);
    const std::uint32_t cmob = primary.front();
    CHECK(omf.cls(cmob) == "CMOB");
    CHECK(omf.str(cmob, "OMFI:MOBJ:Name") == "Reel 1 Mix");
    CHECK(omf.refs(head, "OMFI:HEAD:MediaData").size() == 6);
    const auto idnt = omf.refs(head, "OMFI:HEAD:IdentificationList");
    REQUIRE(idnt.size() == 1);
    CHECK(omf.str(idnt.front(), "OMFI:IDNT:ProductVersionString") == "0.2.0");
    std::set<std::string> effects;
    for (const std::uint32_t d : omf.refs(head, "OMFI:HEAD:DefinitionObjects")) {
        if (omf.cls(d) == "EDEF") {
            effects.insert(omf.str(d, "OMFI:EDEF:EffectID"));
        }
    }
    CHECK(effects == std::set<std::string>{"omfi:effect:SimpleMonoAudioDissolve", "omfi:effect:MonoAudioGain",
                                           "omfi:effect:MonoAudioPan"});

    // Timecode 01:00:00:00 at 25 fps.
    const std::uint32_t tc = omf.ref(slotNamed(omf, cmob, "TC1"), "OMFI:MSLT:Segment");
    CHECK(omf.cls(tc) == "TCCP");
    CHECK(omf.i32(tc, "OMFI:TCCP:Start") == 3600 * 25);
    CHECK(be16(omf.value(tc, "OMFI:TCCP:FPS").data) == 25);

    // A1 Left: panned hard left, A's gain keyframes, the crossfade (2 + 2 frames at 48 kHz), B at -6 dB.
    const std::uint32_t a1 = slotNamed(omf, cmob, "Audio 1 Left");
    CHECK(omf.value(a1, "OMFI:MSLT:EditRate").data == std::string("\x00\x00\xBB\x80\x00\x00\x00\x01", 8));
    const std::uint32_t pan = omf.ref(a1, "OMFI:MSLT:Segment");
    REQUIRE(omf.cls(pan) == "EFFE");
    CHECK(omf.effectOf(pan) == "omfi:effect:MonoAudioPan");
    CHECK(omf.dataKind(pan) == "omfi:data:StereoSound");
    CHECK(omf.value(omf.slotValue(pan, 1), "OMFI:CVAL:Value").data == std::string(8, '\0').replace(4, 4, "\x00\x10\x00\x00", 4));
    const std::uint32_t seqA1 = omf.slotValue(pan, -1);
    REQUIRE(omf.cls(seqA1) == "SEQU");
    CHECK(omf.describe(seqA1) == "SCLP 23040 @0 'A A1 Left' gain*4 | TRAN 7680 cut 3840 | SCLP 23040 @0 'B A1 Left' gain");

    // A2 Left: -3 dB track volume, C with its fade in (3 frames, linear amplitude) on the clip, after filler.
    const std::uint32_t a2pan = omf.ref(slotNamed(omf, cmob, "Audio 2 Left"), "OMFI:MSLT:Segment");
    const std::uint32_t a2gain = omf.slotValue(a2pan, -1);
    REQUIRE(omf.cls(a2gain) == "EFFE");
    CHECK(omf.effectOf(a2gain) == "omfi:effect:MonoAudioGain");
    const std::uint32_t seqA2 = omf.slotValue(a2gain, -1);
    CHECK(omf.describe(seqA2) == "FILL 7680 | SCLP 11520 @3840 'C A2 Left' in 5760/1 | FILL 19200");

    // The master mob leads to a file source mob with a WAVE descriptor and its encapsulated data.
    const std::uint32_t master = omf.mobByUid(omf.value(omf.refs(seqA2, "OMFI:SEQU:Components")[1], "OMFI:SCLP:SourceID").data);
    REQUIRE(master != 0);
    CHECK(omf.cls(master) == "MMOB");
    const std::uint32_t masterClip = omf.ref(omf.refs(master, "OMFI:MOBJ:Slots").front(), "OMFI:MSLT:Segment");
    const std::string fileUid = omf.value(masterClip, "OMFI:SCLP:SourceID").data;
    const std::uint32_t smob = omf.mobByUid(fileUid);
    REQUIRE(omf.cls(smob) == "SMOB");
    const std::uint32_t wavd = omf.ref(smob, "OMFI:SMOB:MediaDescription");
    CHECK(omf.cls(wavd) == "WAVD");
    CHECK(omf.value(wavd, "OMFI:MDFL:IsOMFI").data == std::string(1, '\x01'));
    CHECK(omf.i32(wavd, "OMFI:MDFL:Length") == 19200);
    CHECK(omf.value(wavd, "OMFI:WAVD:Summary").data.substr(0, 4) == "RIFF");
    bool found = false;
    for (const std::uint32_t md : omf.refs(head, "OMFI:HEAD:MediaData")) {
        if (omf.value(md, "OMFI:MDAT:MobID").data == fileUid) {
            found = true;
            CHECK(omf.cls(md) == "WAVE");
            const std::string& wave = omf.value(md, "OMFI:WAVE:AudioData").data;
            CHECK(wave.substr(0, 4) == "RIFF");
            CHECK(wave.find("bext") == std::string::npos); // plain WAVE in OMF
            CHECK(wave.size() == omf.value(wavd, "OMFI:WAVD:Summary").data.size() + 19200 * 3);
        }
    }
    CHECK(found);
}

TEST_CASE("OMF: separate AIFF audio files and Include Pan off (P.2)", "[interchange][omf]") {
    const auto tl = audioPostTimeline();
    const auto dir = it::scratchDir("omf-separate");
    TestRenderer renderer;
    AudioPostHost host;
    host.audio = &renderer;
    OmfExportOptions options = fixedOmfOptions();
    options.audio.files = AudioPostFiles::Separate;
    options.audio.format = AudioPostFormat::Aiff;
    options.audio.bitsPerSample = 16;
    options.includePan = false;
    const auto result = exportOmfToFile(tl.state, tl.seq, dir / "mix.omf", options, host);
    REQUIRE(result.has_value());
    CHECK(result->audioFiles.size() == 6);
    for (const auto& f : result->audioFiles) {
        CHECK(f.parent_path().filename() == "mix Audio");
        CHECK(f.extension() == ".aif");
        CHECK(it::readFile(f).substr(8, 4) == "AIFC");
    }
    CHECK(it::hasEntry(result->report, ReportOutcome::Dropped, ReportCategory::Track, "Include Pan is off"));
    const Omf omf = readOmf(dir / "mix.omf");
    CHECK(omf.refs(bento::kTocObject, "OMFI:HEAD:MediaData").empty());
    std::size_t descriptors = 0;
    for (const std::uint32_t m : omf.refs(bento::kTocObject, "OMFI:HEAD:Mobs")) {
        if (omf.cls(m) != "SMOB") {
            continue;
        }
        const std::uint32_t d = omf.ref(m, "OMFI:SMOB:MediaDescription");
        CHECK(omf.cls(d) == "AIFD");
        CHECK(omf.value(d, "OMFI:MDFL:IsOMFI").data == std::string(1, '\0'));
        const auto locators = omf.refs(d, "OMFI:MDES:Locator");
        REQUIRE(locators.size() == 2);
        CHECK(omf.str(locators[0], "OMFI:NETL:URLString").starts_with("file:///"));
        CHECK(omf.str(locators[1], "OMFI:DOSL:PathName").ends_with(".aif"));
        ++descriptors;
    }
    CHECK(descriptors == 6);
    // No pan effect: the slots hold the gain (A2) or the sequence (A1) directly.
    const std::uint32_t cmob = omf.refs(bento::kTocObject, "OMFI:HEAD:PrimaryMobs").front();
    CHECK(omf.cls(omf.ref(slotNamed(omf, cmob, "Audio 1 Left"), "OMFI:MSLT:Segment")) == "SEQU");
}

TEST_CASE("OMF: needs the audio engine and refuses bad options (P.2)", "[interchange][omf]") {
    const auto tl = audioPostTimeline();
    const auto dir = it::scratchDir("omf-options");
    const auto none = exportOmfToFile(tl.state, tl.seq, dir / "x.omf", fixedOmfOptions());
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().code == ErrorCode::Unsupported);
    // A dry run plans without the engine and writes nothing.
    AudioPostHost dry;
    dry.dryRun = true;
    const auto planned = exportOmfToFile(tl.state, tl.seq, dir / "x.omf", fixedOmfOptions(), dry);
    REQUIRE(planned.has_value());
    CHECK(planned->tracks == 4);
    CHECK(planned->clips == 6);
    OmfExportOptions bad = fixedOmfOptions();
    bad.audio.sampleRate = 44100;
    CHECK(exportOmfToFile(tl.state, tl.seq, dir / "x.omf", bad, dry).error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(std::filesystem::exists(dir / "x.omf"));
}
