// AAF export for audio post (P.2): the compound file layer (Cfb.h), the AAF object layer (AafFile.h) and the exporter
// (AudioPostExport.h), read back with the module's own reader (AafReader.h). The exporter is checked against a golden
// file byte for byte and structurally: tracks, clips, handles, gain, fades, markers and the embedded or separate audio.

#include "AudioPostTestSupport.h"
#include "InterchangeTestSupport.h"
#include "TimelineFixture.h"

#include "AafFile.h"
#include "AafReader.h"
#include "AudioPost.h"
#include "Cfb.h"

#include <wabcut/interchange/AudioPostExport.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>

using namespace wabcut;
using namespace wabcut::interchange;
using wabcut::test::f;
using wabcut::test::ok;
namespace it = wabcut::interchange::test;
namespace aaf = wabcut::interchange::detail::aaf;
namespace cfb = wabcut::interchange::detail::cfb;
namespace audiopost = wabcut::interchange::detail::audiopost;
using wabcut::interchange::test::audioPostTimeline;
using wabcut::interchange::test::fixedTime;
using wabcut::interchange::test::signal;
using wabcut::interchange::test::TestRenderer;

namespace {

std::string bytesOf(std::size_t size, std::uint32_t seed) {
    std::string out(size, '\0');
    std::uint32_t x = seed * 2654435761u + 1;
    for (char& c : out) {
        x = x * 1664525u + 1013904223u;
        c = static_cast<char>(x >> 24);
    }
    return out;
}

AafExportOptions fixedOptions() {
    AafExportOptions o;
    o.timestamp = fixedTime();
    o.productVersion = "0.2.0";
    o.audio.handleFrames = 2;
    return o;
}

const aaf::ReadObject& headerOf(const aaf::ReadFile& file) {
    const aaf::ReadObject* h = file.root.child("Header");
    REQUIRE(h != nullptr);
    return *h;
}

std::vector<const aaf::ReadObject*> mobs(const aaf::ReadFile& file, std::string_view cls) {
    std::vector<const aaf::ReadObject*> out;
    const aaf::ReadObject* content = headerOf(file).child("Content");
    REQUIRE(content != nullptr);
    for (const aaf::ReadObject* m : content->children("Mobs")) {
        if (m->className() == cls) {
            out.push_back(m);
        }
    }
    return out;
}

const aaf::ReadObject* mobById(const aaf::ReadFile& file, const std::array<std::uint8_t, 32>& id) {
    const aaf::ReadObject* content = headerOf(file).child("Content");
    for (const aaf::ReadObject* m : content->children("Mobs")) {
        if (m->mobId("MobID") == id) {
            return m;
        }
    }
    return nullptr;
}

/// "Filler 10 | SourceClip 20 @5 'name' gain | Transition 4 cut 2" for a track's sequence.
std::string describeSequence(const aaf::ReadFile& file, const aaf::ReadObject& seq) {
    std::string out;
    for (const aaf::ReadObject* c : seq.children("Components")) {
        if (!out.empty()) {
            out += " | ";
        }
        const aaf::ReadObject* clip = c;
        std::string gain;
        if (c->className() == "OperationGroup") {
            const auto params = c->children("Parameters");
            REQUIRE(params.size() == 1);
            gain = params.front()->className() == "ConstantValue" ? " gain" : " gain*" + std::to_string(params.front()->children("PointList").size());
            clip = c->children("InputSegments").front();
        }
        out += std::string(clip->className()) + " " + std::to_string(*clip->integer("Length"));
        if (clip->className() == "SourceClip") {
            const aaf::ReadObject* master = mobById(file, *clip->mobId("SourceID"));
            out += " @" + std::to_string(*clip->integer("StartTime")) + " '" + (master ? *master->string("Name") : "?") + "'";
        }
        if (clip->className() == "Transition") {
            out += " cut " + std::to_string(*clip->integer("CutPoint"));
        }
        out += gain;
    }
    return out;
}

const aaf::ReadObject& slotNamed(const aaf::ReadObject& mob, std::string_view name) {
    for (const aaf::ReadObject* s : mob.children("Slots")) {
        if (s->string("SlotName") == std::string(name)) {
            return *s;
        }
    }
    FAIL("no slot " << name);
    throw std::logic_error("unreachable");
}

/// The Sequence under a slot's operation groups.
const aaf::ReadObject& sequenceOf(const aaf::ReadObject& slot) {
    const aaf::ReadObject* s = slot.child("Segment");
    while (s && s->className() == "OperationGroup") {
        s = s->children("InputSegments").front();
    }
    REQUIRE(s != nullptr);
    REQUIRE(s->className() == "Sequence");
    return *s;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Compound files

TEST_CASE("AAF: a compound file reads back with every storage and stream (P.2)", "[interchange][aaf]") {
    for (const std::uint32_t sectorSize : {4096u, 512u}) {
        INFO(sectorSize);
        cfb::Builder b(sectorSize);
        b.setHeaderClsid(aaf::kFileKindAaf4K.bytes());
        const std::vector<std::size_t> sizes{0, 1, 63, 64, 4095, 4096, 4097, 70'000};
        const std::size_t storage = b.addStorage(cfb::Builder::kRoot, "Child-1");
        b.setClsid(storage, aaf::Auid{1, 2, 3, {4, 5, 6, 7, 8, 9, 10, 11}}.bytes());
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            b.addStream(i % 2 == 0 ? cfb::Builder::kRoot : storage, "s" + std::to_string(i),
                        cfb::StreamSource::of(bytesOf(sizes[i], static_cast<std::uint32_t>(i))));
        }
        // Enough entries for several directory sectors and a deep tree.
        const std::size_t many = b.addStorage(storage, "many");
        for (int i = 0; i < 300; ++i) {
            b.addStream(many, "item{" + std::to_string(i) + "}", cfb::StreamSource::of(std::to_string(i)));
        }
        REQUIRE_FALSE(b.problem().has_value());
        const auto bytes = b.toBytes();
        REQUIRE(bytes.has_value());
        CHECK(bytes->size() % sectorSize == 0);
        auto r = cfb::Reader::open(*bytes);
        REQUIRE(r.has_value());
        CHECK(r->headerClsid() == aaf::kFileKindAaf4K.bytes());
        CHECK(r->majorVersion() == (sectorSize == 512 ? 3 : 4));
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            const auto at = r->find((i % 2 == 0 ? "/s" : "/Child-1/s") + std::to_string(i));
            REQUIRE(at.has_value());
            const auto data = r->read(*at);
            REQUIRE(data.has_value());
            CHECK(*data == bytesOf(sizes[i], static_cast<std::uint32_t>(i)));
        }
        const auto child = r->find("/child-1"); // names compare without case
        REQUIRE(child.has_value());
        CHECK(r->entries()[*child].clsid == aaf::Auid{1, 2, 3, {4, 5, 6, 7, 8, 9, 10, 11}}.bytes());
        const auto list = r->find("/Child-1/many");
        REQUIRE(list.has_value());
        CHECK(r->entries()[*list].children.size() == 300);
        CHECK(r->read(*r->find("/Child-1/many/item{299}")).value() == "299");
        // Siblings come out in the format's order: shorter names first.
        const auto& kids = r->entries()[*list].children;
        CHECK(r->entries()[kids.front()].name == "item{0}");
        CHECK(r->entries()[kids.back()].name == "item{299}");
    }
}

TEST_CASE("AAF: a compound file with more FAT sectors than the header lists uses the DIFAT (P.2)", "[interchange][aaf]") {
    // 512-byte sectors: 109 FAT sectors cover 13,952 sectors (6.8 MB); a 9 MB stream needs DIFAT sectors.
    cfb::Builder b(512);
    const std::string big = bytesOf(9 * 1024 * 1024, 7);
    b.addStream(cfb::Builder::kRoot, "big", cfb::StreamSource::of(big));
    const auto bytes = b.toBytes();
    REQUIRE(bytes.has_value());
    std::uint32_t difatCount = 0;
    std::memcpy(&difatCount, bytes->data() + 72, 4);
    CHECK(difatCount > 0);
    auto r = cfb::Reader::open(*bytes);
    REQUIRE(r.has_value());
    CHECK(r->read(*r->find("/big")).value() == big);
}

TEST_CASE("AAF: file-backed streams are copied into the compound file (P.2)", "[interchange][aaf]") {
    const auto dir = it::scratchDir("aaf-file-streams");
    const std::string payload = bytesOf(200'000, 3);
    const std::string small = bytesOf(1000, 4);
    it::writeFile(dir / "big.bin", "HEADER" + payload);
    it::writeFile(dir / "small.bin", small);
    cfb::Builder b;
    b.addStream(cfb::Builder::kRoot, "big", cfb::StreamSource::ofFile(dir / "big.bin", 6, payload.size()));
    b.addStream(cfb::Builder::kRoot, "small", cfb::StreamSource::ofFile(dir / "small.bin", 0, small.size()));
    REQUIRE(b.write(dir / "out.cfb").has_value());
    auto r = cfb::Reader::open(it::readFile(dir / "out.cfb"));
    REQUIRE(r.has_value());
    CHECK(r->read(*r->find("/big")).value() == payload);
    CHECK(r->read(*r->find("/small")).value() == small);
    // A file shorter than the size given fails and leaves nothing behind.
    cfb::Builder bad;
    bad.addStream(cfb::Builder::kRoot, "big", cfb::StreamSource::ofFile(dir / "small.bin", 0, 50'000));
    CHECK_FALSE(bad.write(dir / "bad.cfb").has_value());
    CHECK_FALSE(std::filesystem::exists(dir / "bad.cfb"));
    CHECK_FALSE(std::filesystem::exists(dir / "bad.cfb.wabcut-partial"));
}

TEST_CASE("AAF: bad compound file names are refused (P.2)", "[interchange][aaf]") {
    cfb::Builder b;
    b.addStream(cfb::Builder::kRoot, "a", cfb::StreamSource::of("x"));
    b.addStream(cfb::Builder::kRoot, "A", cfb::StreamSource::of("y")); // same name, other case
    CHECK(b.problem().has_value());
    CHECK_FALSE(b.toBytes().has_value());
    cfb::Builder c;
    c.addStorage(cfb::Builder::kRoot, std::string(32, 'n'));
    CHECK(c.problem().has_value());
    cfb::Builder d;
    d.addStorage(cfb::Builder::kRoot, "a/b");
    CHECK(d.problem().has_value());
}

TEST_CASE("AAF: malformed compound files are refused without crashing (P.2)", "[interchange][aaf]") {
    cfb::Builder b(512);
    const std::size_t s = b.addStorage(cfb::Builder::kRoot, "s");
    for (int i = 0; i < 40; ++i) {
        b.addStream(i % 3 == 0 ? cfb::Builder::kRoot : s, "x" + std::to_string(i),
                    cfb::StreamSource::of(bytesOf(static_cast<std::size_t>(i * 300), static_cast<std::uint32_t>(i))));
    }
    const std::string good = b.toBytes().value();
    REQUIRE(cfb::Reader::open(good).has_value());

    CHECK_FALSE(cfb::Reader::open("").has_value());
    CHECK_FALSE(cfb::Reader::open(good.substr(0, 100)).has_value());
    std::string badMagic = good;
    badMagic[0] = 'X';
    CHECK_FALSE(cfb::Reader::open(badMagic).has_value());
    // The first FAT sector pointing at itself: chains loop.
    std::string loop = good;
    std::uint32_t fatSector = 0;
    std::memcpy(&fatSector, loop.data() + 76, 4);
    std::uint32_t dirStart = 0;
    std::memcpy(&dirStart, loop.data() + 48, 4);
    const std::size_t entry = (fatSector + 1) * 512 + 4 * dirStart;
    std::memcpy(loop.data() + entry, &dirStart, 4);
    CHECK_FALSE(cfb::Reader::open(loop).has_value());
    // A directory entry whose left sibling is the root's child itself: a cycle.
    std::string cycle = good;
    const std::size_t dirAt = (dirStart + 1) * 512;
    std::uint32_t child = 0;
    std::memcpy(&child, cycle.data() + dirAt + 76, 4);
    std::memcpy(cycle.data() + dirAt + child * 128 + 68, &child, 4);
    CHECK_FALSE(cfb::Reader::open(cycle).has_value());

    // Random corruption: the reader may accept or refuse, and every stream read is bounded.
    std::mt19937 rng(12345);
    for (int round = 0; round < 1500; ++round) {
        std::string mutated = good;
        const int flips = 1 + static_cast<int>(rng() % 8);
        for (int k = 0; k < flips; ++k) {
            mutated[rng() % mutated.size()] = static_cast<char>(rng());
        }
        auto r = cfb::Reader::open(mutated);
        if (!r) {
            continue;
        }
        for (std::size_t i = 0; i < r->entries().size(); ++i) {
            if (r->entries()[i].type == cfb::EntryType::Stream) {
                (void)r->read(i);
            }
        }
        (void)aaf::readAaf(mutated);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// AAF objects

TEST_CASE("AAF: a file describes the whole object model in its MetaDictionary (P.2)", "[interchange][aaf]") {
    aaf::File file;
    file.header()
        .setInt("ByteOrder", 0x4949)
        .setTimeStamp("LastModified", aaf::TimeStamp{})
        .setRaw("Version", std::string("\x01\x01", 2));
    file.header()
        .append("IdentificationList", "Identification")
        .setString("CompanyName", "Wabcut")
        .setString("ProductName", "Wabcut")
        .setString("ProductVersionString", "test")
        .setAuid("ProductID", aaf::Auid{})
        .setTimeStamp("Date", aaf::TimeStamp{})
        .setAuid("GenerationAUID", aaf::Auid{});
    file.contentStorage().touch("Mobs");
    REQUIRE(file.problems().empty());
    REQUIRE(file.missingRequired().empty());
    const auto bytes = file.toBytes();
    REQUIRE(bytes.has_value());
    const auto read = aaf::readAaf(*bytes);
    REQUIRE(read.has_value());
    CHECK(read->headerClsid == aaf::kFileKindAaf4K.bytes());
    CHECK(read->root.className() == "Root");
    const aaf::ReadObject* meta = read->root.child("MetaDictionary");
    REQUIRE(meta != nullptr);
    const auto classDefs = meta->children("ClassDefinitions");
    const auto typeDefs = meta->children("TypeDefinitions");
    CHECK(classDefs.size() == aaf::classes().size() - 1); // all but Root
    CHECK(typeDefs.size() == aaf::types().size());
    std::size_t properties = 0;
    for (const aaf::ReadObject* c : classDefs) {
        const int index = aaf::findClass(*c->auid("Identification"));
        REQUIRE(index > 0);
        CHECK(c->string("Name") == std::string(aaf::classes()[static_cast<std::size_t>(index)].name));
        properties += c->children("Properties").size();
    }
    CHECK(properties == aaf::properties().size() - 2); // the Root's two are not listed
    // Weak references resolve through "referenced properties": the class definitions' set, the type definitions'.
    CHECK(std::find(read->weakPaths.begin(), read->weakPaths.end(), std::vector<std::uint16_t>{0x0001, 0x0003}) !=
          read->weakPaths.end());
    CHECK(std::find(read->weakPaths.begin(), read->weakPaths.end(), std::vector<std::uint16_t>{0x0001, 0x0004}) !=
          read->weakPaths.end());
}

TEST_CASE("AAF: setters refuse properties a class does not have and values of the wrong type (P.2)", "[interchange][aaf]") {
    aaf::File file;
    file.header().setString("NoSuchProperty", "x");
    file.header().setString("ByteOrder", "x"); // an Int16
    file.header().setInt("ObjectModelVersion", -1); // unsigned
    file.contentStorage().append("Mobs", "Filler"); // not a Mob
    file.contentStorage().append("Mobs", "Mob");    // abstract
    CHECK(file.problems().size() == 5);
    CHECK_FALSE(file.toBytes().has_value());
    aaf::File missing;
    CHECK(std::find(missing.missingRequired().begin(), missing.missingRequired().end(), "Header.ByteOrder") !=
          missing.missingRequired().end());
    CHECK_FALSE(missing.toBytes().has_value());
}

TEST_CASE("AAF: names of child storages follow the AAF mangling (P.2)", "[interchange][aaf]") {
    CHECK(aaf::mangledName("Header", 0x0002, 32) == "Header-2");
    CHECK(aaf::mangledName("Mobs", 0x1901, 22) == "Mobs-1901");
    CHECK(aaf::mangledName("ClassDefinitions", 0x0003, 22) == "ClassDefinitions-3");
    CHECK(aaf::mangledName("InterpolationDefinitions", 0x2609, 22) == "Interpol-nitions-2609");
    CHECK(aaf::mangledName("InterpolationDefinitions", 0x2609, 22).size() <= 21);
    CHECK(aaf::utf16le("A\xC3\xA9") == std::string("A\0\xE9\0\0\0", 6));
    CHECK(aaf::utf16le("\xF0\x9F\x8E\xAC") == std::string("\x3C\xD8\xAC\xDF\0\0", 6)); // a surrogate pair
}

// ---------------------------------------------------------------------------------------------------------------------
// The exporter

TEST_CASE("AAF: export golden file with embedded audio (P.2)", "[interchange][aaf]") {
    const auto tl = audioPostTimeline();
    const auto dir = it::scratchDir("aaf-golden");
    TestRenderer renderer;
    AudioPostHost host;
    host.audio = &renderer;
    const auto result = exportAafToFile(tl.state, tl.seq, dir / "audiopost.aaf", fixedOptions(), host);
    REQUIRE(result.has_value());
    const std::string bytes = it::readFile(dir / "audiopost.aaf");
    const std::filesystem::path golden = it::fixturePath("export/audiopost.aaf");
    if (std::getenv("WABCUT_UPDATE_GOLDEN") != nullptr) {
        it::writeFile(golden, bytes);
    }
    const std::string want = std::filesystem::exists(golden) ? it::readFile(golden) : std::string();
    if (bytes != want) {
        it::writeFile(std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "export" / "audiopost.aaf", bytes);
    }
    CHECK(bytes == want);
    // Deterministic: a second export gives the same bytes.
    const auto again = exportAafToFile(tl.state, tl.seq, dir / "again.aaf", fixedOptions(), host);
    REQUIRE(again.has_value());
    CHECK(it::readFile(dir / "again.aaf") == bytes);
    // No temporary audio is left behind.
    CHECK_FALSE(std::filesystem::exists(dir / "audiopost.aaf.wabcut-partial-audio"));
}

TEST_CASE("AAF: the composition reads back with tracks clips handles gain fades and markers (P.2)", "[interchange][aaf]") {
    const auto tl = audioPostTimeline();
    const auto dir = it::scratchDir("aaf-structure");
    TestRenderer renderer;
    AudioPostHost host;
    host.audio = &renderer;
    const auto result = exportAafToFile(tl.state, tl.seq, dir / "mix.aaf", fixedOptions(), host);
    REQUIRE(result.has_value());
    CHECK(result->tracks == 4); // A1 and A2 are stereo: a Left and a Right track each
    const auto read = aaf::readAaf(it::readFile(dir / "mix.aaf"));
    REQUIRE(read.has_value());
    const aaf::ReadObject& header = headerOf(*read);
    CHECK(header.auid("OperationalPattern") == aaf::kOpEditProtocol);
    const auto compositions = mobs(*read, "CompositionMob");
    REQUIRE(compositions.size() == 1);
    const aaf::ReadObject& comp = *compositions.front();
    CHECK(comp.string("Name") == std::string("Fixture"));
    CHECK(comp.auid("UsageCode") == aaf::kUsageTopLevel);

    // Timecode 01:00:00:00 at 25 fps.
    const aaf::ReadObject& tc = slotNamed(comp, "TC1");
    CHECK(tc.rational("EditRate") == std::pair{25, 1});
    const aaf::ReadObject* timecode = tc.child("Segment");
    REQUIRE(timecode != nullptr);
    CHECK(timecode->integer("Start") == 3600 * 25);
    CHECK(timecode->integer("FPS") == 25);

    // A1 is stereo: two mono tracks panned hard left and right. 25 fps = 1920 samples a frame at 48 kHz.
    const aaf::ReadObject& left = slotNamed(comp, "Audio 1 Left");
    CHECK(left.rational("EditRate") == std::pair{48000, 1});
    CHECK(left.integer("PhysicalTrackNumber") == 1);
    const aaf::ReadObject* pan = left.child("Segment");
    REQUIRE(pan != nullptr);
    CHECK(pan->className() == "OperationGroup");
    const auto panValue = pan->children("Parameters").front()->indirect("Value");
    REQUIRE(panValue.has_value());
    CHECK(panValue->second.substr(0, 4) == std::string(4, '\0')); // 0 = hard left
    // A plays frames 0..10 and runs 2 frames past its end into the crossfade (2 + 2 frames) with B, whose segment starts
    // 2 frames early; their files hold just that (A has no media before its In). A's gain has keyframes, B is -6 dB.
    const auto points = sequenceOf(left).children("Components").front()->children("Parameters").front()->children("PointList").size();
    CHECK(points >= 2);
    CHECK(describeSequence(*read, sequenceOf(left)) == "SourceClip 23040 @0 'A A1 Left' gain*" + std::to_string(points) +
                                                           " | Transition 7680 cut 3840 | SourceClip 23040 @0 'B A1 Left' gain");
    const aaf::ReadObject& a2 = slotNamed(comp, "Audio 2 Left");
    // C starts at frame 4 with a 3-frame fade in: filler to 4 + 3 frames, the fade, then C.
    CHECK(describeSequence(*read, sequenceOf(a2)).starts_with("Filler 13440 | Transition 5760 cut 0 | SourceClip 11520 @3840 'C A2 Left'"));
    // Track volume -3 dB on A2 as a gain over the whole track.
    const aaf::ReadObject* a2pan = a2.child("Segment");
    REQUIRE(a2pan != nullptr);
    const aaf::ReadObject* a2gain = a2pan->children("InputSegments").front();
    CHECK(a2gain->className() == "OperationGroup");
    const auto gainValue = a2gain->children("Parameters").front()->indirect("Value");
    REQUIRE(gainValue.has_value());
    std::int32_t num = 0;
    std::int32_t den = 0;
    std::memcpy(&num, gainValue->second.data(), 4);
    std::memcpy(&den, gainValue->second.data() + 4, 4);
    CHECK(std::abs(static_cast<double>(num) / den - 0.70795) < 0.001);

    // Markers.
    const aaf::ReadObject* events = nullptr;
    for (const aaf::ReadObject* s : comp.children("Slots")) {
        if (s->className() == "EventMobSlot") {
            events = s;
        }
    }
    REQUIRE(events != nullptr);
    const auto markers = events->child("Segment")->children("Components");
    REQUIRE(markers.size() == 1);
    CHECK(markers.front()->className() == "DescriptiveMarker");
    CHECK(markers.front()->integer("Position") == 5);
    CHECK(markers.front()->string("Comment") == std::string("Mix note: Music up"));

    // Each rendered file: a master mob -> a file source mob with a WAVE descriptor and its embedded essence.
    const auto masters = mobs(*read, "MasterMob");
    const auto sources = mobs(*read, "SourceMob");
    const aaf::ReadObject* content = headerOf(*read).child("Content");
    const auto essence = content->children("EssenceData");
    std::size_t wave = 0;
    for (const aaf::ReadObject* src : sources) {
        const aaf::ReadObject* d = src->child("EssenceDescription");
        REQUIRE(d != nullptr);
        if (d->className() != "WAVEDescriptor") {
            continue; // the video's CDCI descriptor
        }
        ++wave;
        const auto summary = d->find("Summary");
        REQUIRE(summary != nullptr);
        CHECK(summary->data.starts_with("RIFF"));
        const auto found = std::find_if(essence.begin(), essence.end(),
                                        [&](const aaf::ReadObject* e) { return e->mobId("MobID") == src->mobId("MobID"); });
        REQUIRE(found != essence.end());
        const auto data = (*found)->find("Data");
        REQUIRE(data != nullptr);
        CHECK(data->stream.starts_with(summary->data)); // the stream is the whole WAV file
        CHECK(static_cast<std::int64_t>(data->stream.size() - summary->data.size()) / 3 >= *d->integer("Length"));
    }
    CHECK(wave == renderer.requests.size() * 2); // two channels a request
    CHECK(masters.size() == wave + 1);           // and the video's
    CHECK(result->audioFiles.empty());
}

TEST_CASE("AAF: separate audio files are Broadcast WAV or AIFF next to the AAF (P.2)", "[interchange][aaf]") {
    const auto tl = audioPostTimeline();
    for (const AudioPostFormat format : {AudioPostFormat::BroadcastWave, AudioPostFormat::Aiff}) {
        const auto dir = it::scratchDir(format == AudioPostFormat::BroadcastWave ? "aaf-separate-wav" : "aaf-separate-aiff");
        TestRenderer renderer;
        AudioPostHost host;
        host.audio = &renderer;
        AafExportOptions options = fixedOptions();
        options.audio.files = AudioPostFiles::Separate;
        options.audio.format = format;
        options.audio.bitsPerSample = 16;
        const auto result = exportAafToFile(tl.state, tl.seq, dir / "mix.aaf", options, host);
        REQUIRE(result.has_value());
        REQUIRE_FALSE(result->audioFiles.empty());
        for (const auto& file : result->audioFiles) {
            CHECK(file.parent_path() == dir / "mix Audio");
            CHECK(file.extension() == (format == AudioPostFormat::BroadcastWave ? ".wav" : ".aif"));
            const std::string bytes = it::readFile(file);
            if (format == AudioPostFormat::BroadcastWave) {
                CHECK(bytes.substr(0, 4) == "RIFF");
                CHECK(bytes.substr(8, 8) == "WAVEbext");
                const std::size_t fmt = bytes.find("fmt ");
                REQUIRE(fmt != std::string::npos);
                std::uint16_t bits = 0;
                std::memcpy(&bits, bytes.data() + fmt + 22, 2);
                CHECK(bits == 16);
            } else {
                CHECK(bytes.substr(0, 4) == "FORM");
                CHECK(bytes.substr(8, 4) == "AIFF");
                CHECK(bytes.find("SSND") != std::string::npos);
            }
        }
        // The AAF points at them.
        const auto read = aaf::readAaf(it::readFile(dir / "mix.aaf"));
        REQUIRE(read.has_value());
        std::size_t located = 0;
        for (const aaf::ReadObject* src : mobs(*read, "SourceMob")) {
            const aaf::ReadObject* d = src->child("EssenceDescription");
            if (d->className() == (format == AudioPostFormat::BroadcastWave ? "WAVEDescriptor" : "AIFCDescriptor")) {
                const auto locators = d->children("Locator");
                REQUIRE(locators.size() == 1);
                CHECK(locators.front()->string("URLString")->starts_with("file:///"));
                ++located;
            }
        }
        CHECK(located == result->audioFiles.size());
        CHECK(headerOf(*read).child("Content")->children("EssenceData").empty());
    }
}

TEST_CASE("AAF: the rendered audio holds each clip and its handles sample for sample (P.2)", "[interchange][aaf]") {
    const auto tl = audioPostTimeline();
    const auto dir = it::scratchDir("aaf-samples");
    TestRenderer renderer;
    AudioPostHost host;
    host.audio = &renderer;
    AafExportOptions options = fixedOptions();
    options.audio.files = AudioPostFiles::Separate;
    const auto result = exportAafToFile(tl.state, tl.seq, dir / "mix.aaf", options, host);
    REQUIRE(result.has_value());
    // Clip C on A2 (frames 4..10, 2 handle frames, its media starts at 0): rendered from frame 2 to frame 12.
    const auto c = std::find_if(renderer.requests.begin(), renderer.requests.end(), [&](const AudioPostRenderRequest& r) {
        return r.clip == wabcut::test::clipAt(tl.state, tl.seq, "A2@4");
    });
    REQUIRE(c != renderer.requests.end());
    CHECK(c->range.start == f(2));
    CHECK(c->range.duration == f(10));
    CHECK(c->channels == 2);
    CHECK(c->sampleRate == 48000);
    CHECK_FALSE(c->clipEffects);
    // Its right channel file: samples of signal(timeline sample, 1), 24-bit little-endian.
    const auto file = std::find_if(result->audioFiles.begin(), result->audioFiles.end(),
                                   [](const std::filesystem::path& p) { return p.filename() == "C A2 Right.wav"; });
    REQUIRE(file != result->audioFiles.end());
    const std::string bytes = it::readFile(*file);
    const std::size_t data = bytes.find("data");
    REQUIRE(data != std::string::npos);
    std::uint32_t size = 0;
    std::memcpy(&size, bytes.data() + data + 4, 4);
    CHECK(size == 10 * 1920 * 3);
    const std::int64_t first = 2 * 1920;
    for (const std::int64_t k : {std::int64_t{0}, std::int64_t{1}, std::int64_t{999}, std::int64_t{19199}}) {
        std::int32_t v = 0;
        std::memcpy(&v, bytes.data() + data + 8 + 3 * k, 3);
        v = (v << 8) >> 8;
        CHECK(v == static_cast<std::int32_t>(std::floor(static_cast<double>(signal(first + k, 1)) * 8388607.0 + 0.5)));
    }
    // bext: the time reference is the sample since midnight at timecode 01:00:00:02.
    CHECK(bytes.substr(12, 4) == "bext");
    std::uint32_t low = 0;
    std::memcpy(&low, bytes.data() + 20 + 256 + 32 + 32 + 10 + 8, 4);
    CHECK(low == static_cast<std::uint32_t>(3600 * 48000 + first));
}

TEST_CASE("AAF: without the audio engine clips link to their original files (P.2)", "[interchange][aaf]") {
    const auto tl = audioPostTimeline();
    const auto dir = it::scratchDir("aaf-linked");
    const auto result = exportAafToFile(tl.state, tl.seq, dir / "linked.aaf", fixedOptions());
    REQUIRE(result.has_value());
    CHECK(it::hasEntry(result->report, ReportOutcome::Approximated, ReportCategory::Media, "Breakout to Mono needs"));
    CHECK(result->tracks == 2); // one AAF track per Wabcut track
    const auto read = aaf::readAaf(it::readFile(dir / "linked.aaf"));
    REQUIRE(read.has_value());
    std::set<std::string> urls;
    for (const aaf::ReadObject* src : mobs(*read, "SourceMob")) {
        const aaf::ReadObject* d = src->child("EssenceDescription");
        if (d->className() == "PCMDescriptor") {
            CHECK(d->integer("Channels") == 2);
            urls.insert(*d->children("Locator").front()->string("URLString"));
        }
    }
    CHECK(urls == std::set<std::string>{"file:///D:/footage/A.mov", "file:///D:/footage/B.mov", "file:///D:/footage/C.mov"});
    const auto comp = mobs(*read, "CompositionMob");
    REQUIRE(comp.size() == 1);
    // B plays from its source In (frame 0 of B) at 48 kHz units; its crossfade with A is kept.
    CHECK(describeSequence(*read, sequenceOf(slotNamed(*comp.front(), "Audio 1"))).find("Transition 7680 cut 3840") !=
          std::string::npos);
}

TEST_CASE("AAF: options out of range are refused (P.2)", "[interchange][aaf]") {
    const auto tl = audioPostTimeline();
    const auto dir = it::scratchDir("aaf-options");
    AafExportOptions o = fixedOptions();
    o.audio.sampleRate = 44100;
    CHECK(exportAafToFile(tl.state, tl.seq, dir / "x.aaf", o).error().code == ErrorCode::InvalidArgument);
    o = fixedOptions();
    o.audio.bitsPerSample = 32;
    CHECK(exportAafToFile(tl.state, tl.seq, dir / "x.aaf", o).error().code == ErrorCode::InvalidArgument);
    o = fixedOptions();
    o.audio.handleFrames = -1;
    CHECK(exportAafToFile(tl.state, tl.seq, dir / "x.aaf", o).error().code == ErrorCode::InvalidArgument);
    CHECK(exportAafToFile(tl.state, SequenceId{999'999}, dir / "x.aaf", fixedOptions()).error().code == ErrorCode::NotFound);
    CHECK_FALSE(std::filesystem::exists(dir / "x.aaf"));
}

TEST_CASE("AAF: an offline clip is left out and cancelling leaves nothing behind (P.2)", "[interchange][aaf]") {
    const auto tl = audioPostTimeline();
    const auto dir = it::scratchDir("aaf-offline");
    TestRenderer renderer;
    renderer.offline.insert(wabcut::test::clipAt(tl.state, tl.seq, "A2@4").value);
    AudioPostHost host;
    host.audio = &renderer;
    AafExportOptions options = fixedOptions();
    options.audio.files = AudioPostFiles::Separate;
    const auto result = exportAafToFile(tl.state, tl.seq, dir / "mix.aaf", options, host);
    REQUIRE(result.has_value());
    CHECK(it::hasEntry(result->report, ReportOutcome::Dropped, ReportCategory::Media, "could not be rendered"));
    const auto read = aaf::readAaf(it::readFile(dir / "mix.aaf"));
    REQUIRE(read.has_value());
    const auto comp = mobs(*read, "CompositionMob").front();
    CHECK(describeSequence(*read, sequenceOf(slotNamed(*comp, "Audio 2 Left"))) == "Filler 38400");

    // Cancelled half way: an error, no AAF, no audio files.
    const auto cancelDir = it::scratchDir("aaf-cancel");
    TestRenderer second;
    int calls = 0;
    AudioPostHost cancelling;
    cancelling.audio = &second;
    cancelling.progress = [&](double) { return ++calls < 3; };
    const auto cancelled = exportAafToFile(tl.state, tl.seq, cancelDir / "mix.aaf", options, cancelling);
    REQUIRE_FALSE(cancelled.has_value());
    CHECK(cancelled.error().code == ErrorCode::Cancelled);
    CHECK_FALSE(std::filesystem::exists(cancelDir / "mix.aaf"));
    std::size_t files = 0;
    if (std::filesystem::exists(cancelDir / "mix Audio")) {
        for ([[maybe_unused]] const auto& e : std::filesystem::directory_iterator(cancelDir / "mix Audio")) {
            ++files;
        }
    }
    CHECK(files == 0);
}

TEST_CASE("AAF: clip effects are rendered only when asked and clean copies go on No FX tracks (P.2)", "[interchange][aaf]") {
    auto tl = audioPostTimeline();
    const ClipId c = wabcut::test::clipAt(tl.state, tl.seq, "A2@4");
    tl.state = ok(edit::addEffect(tl.state, tl.seq, std::span<const ClipId>(&c, 1), "wabcut.audio.reverb")).state;
    const auto dir = it::scratchDir("aaf-effects");
    {
        TestRenderer renderer;
        AudioPostHost host;
        host.audio = &renderer;
        const auto result = exportAafToFile(tl.state, tl.seq, dir / "plain.aaf", fixedOptions(), host);
        REQUIRE(result.has_value());
        CHECK(it::hasEntry(result->report, ReportOutcome::Dropped, ReportCategory::Effect, "Reverb"));
        CHECK(std::none_of(renderer.requests.begin(), renderer.requests.end(),
                           [](const AudioPostRenderRequest& r) { return r.clipEffects; }));
    }
    TestRenderer renderer;
    AudioPostHost host;
    host.audio = &renderer;
    AafExportOptions options = fixedOptions();
    options.renderAudioClipEffects = true;
    options.includeClipCopiesWithoutEffects = true;
    const auto result = exportAafToFile(tl.state, tl.seq, dir / "fx.aaf", options, host);
    REQUIRE(result.has_value());
    CHECK(result->tracks == 6); // A1 Left/Right, A2 Left/Right FX, A2 Left/Right No FX
    const auto read = aaf::readAaf(it::readFile(dir / "fx.aaf"));
    REQUIRE(read.has_value());
    const auto comp = mobs(*read, "CompositionMob").front();
    CHECK(describeSequence(*read, sequenceOf(slotNamed(*comp, "Audio 2 Left FX"))).find("'C A2 Left FX'") != std::string::npos);
    CHECK(describeSequence(*read, sequenceOf(slotNamed(*comp, "Audio 2 Left No FX"))).find("'C A2 Left No FX'") != std::string::npos);
    const auto withFx = std::count_if(renderer.requests.begin(), renderer.requests.end(),
                                      [](const AudioPostRenderRequest& r) { return r.clipEffects; });
    CHECK(withFx == 1);
}

TEST_CASE("AAF: audio file headers are laid out as WAV and AIFF readers expect (P.2)", "[interchange][aaf]") {
    const std::string wav = audiopost::fileHeader(audiopost::FileKind::Wave, 1, 48000, 24, 3, nullptr);
    CHECK(wav.size() == 44);
    CHECK(wav.substr(0, 4) == "RIFF");
    std::uint32_t riff = 0;
    std::memcpy(&riff, wav.data() + 4, 4);
    CHECK(riff == 36 + 9 + 1); // 9 data bytes and a pad byte
    CHECK(audiopost::fileTrailer(9) == std::string(1, '\0'));
    const std::string aiff = audiopost::fileHeader(audiopost::FileKind::Aiff, 1, 48000, 16, 10, nullptr);
    CHECK(aiff.substr(0, 4) == "FORM");
    CHECK(aiff.substr(8, 4) == "AIFF");
    CHECK(aiff.size() == 12 + 26 + 16);
    const auto rate = audiopost::extended80(48000.0);
    CHECK(rate == std::array<std::uint8_t, 10>{0x40, 0x0E, 0xBB, 0x80, 0, 0, 0, 0, 0, 0});
    const std::string aifc = audiopost::fileHeader(audiopost::FileKind::Aifc, 1, 96000, 24, 1, nullptr);
    CHECK(aifc.substr(8, 4) == "AIFC");
    CHECK(aifc.find("NONE") != std::string::npos);
    std::string samples;
    audiopost::encodeSamples(std::vector<float>{1.0f, -1.0f, 0.0f, 2.0f}, 16, true, samples);
    CHECK(samples == std::string("\x7F\xFF\x80\x01\x00\x00\x7F\xFF", 8));
}
