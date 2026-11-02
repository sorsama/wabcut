// AAF and OMF import (0.6, card #68): the golden AAF / OMF exports and Wabcut's own exports read back, hand-built files
// in the shapes Pro Tools, Media Composer and DaVinci Resolve write (48 kHz and 96 kHz, 44.1 kHz media, 5.1 as one
// multichannel file, as six mono files and as six named mono tracks, clip gain and volume keyframes, muted clips,
// fades, track automation, markers), offline media, and malformed files that must fail cleanly.

#include "AudioPostTestSupport.h"
#include "InterchangeTestSupport.h"
#include "TimelineFixture.h"

#include "AafFile.h"
#include "AudioPostImport.h"
#include "Bento.h"

#include <wabcut/interchange/AudioPostExport.h>
#include <wabcut/interchange/FileUrl.h>
#include <wabcut/interchange/Import.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::interchange;
namespace it = wabcut::interchange::test;
namespace aaf = wabcut::interchange::detail::aaf;
namespace graph = wabcut::interchange::detail::graph;
namespace cfb = wabcut::interchange::detail::cfb;
namespace bento = wabcut::interchange::detail::bento;
namespace fs = std::filesystem;
using wabcut::interchange::test::audioPostTimeline;
using wabcut::interchange::test::fixedTime;
using wabcut::interchange::test::TestRenderer;

namespace {

/// Media is looked up in the test's folder only (the golden files name paths of the machine that wrote them).
ImportOptions scratchOptions(const fs::path& dir) {
    ImportOptions o;
    o.projectFile = dir / "Project.wabcut";
    o.embeddedMediaFolder = dir / "Embedded";
    auto normal = [](const fs::path& p) {
        std::string s = pathToUtf8(p.lexically_normal());
        std::transform(s.begin(), s.end(), s.begin(), [](char c) {
            return c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        });
        return s;
    };
    const std::string root = normal(fs::absolute(dir));
    o.fileExists = [root, normal](const fs::path& p) {
        std::error_code ec;
        return normal(p).starts_with(root) && fs::is_regular_file(p, ec);
    };
    return o;
}

ImportResult importOk(const fs::path& file, const ImportOptions& options) {
    auto result = ImportRouter::importFile(file, options);
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    const auto valid = validate(result->project, Validation::Strict);
    INFO((valid ? std::string() : valid.error().message));
    REQUIRE(valid.has_value());
    REQUIRE(io::toJson(result->project).has_value());
    return std::move(*result);
}

double volumeDb(const Clip& c) { return c.properties()[ParamId::VolumeDb].constant(); }

const Clip& clipAt(const Sequence& s, std::string_view track, std::size_t index) {
    const Track& t = it::track(s, track);
    REQUIRE(index < t.clips.size());
    return t.clips[index];
}

std::vector<std::string> trackNames(const Sequence& s) {
    std::vector<std::string> out;
    for (const TrackPtr& t : s.audioTracks) {
        out.push_back(t->name);
    }
    return out;
}

const Transition* transitionAt(const Track& t, Ticks cut) {
    for (const Transition& tr : t.transitions) {
        const Clip* out = nullptr;
        const Clip* in = nullptr;
        for (const Clip& c : t.clips) {
            if (c.id == tr.outgoing) {
                out = &c;
            }
            if (c.id == tr.incoming) {
                in = &c;
            }
        }
        if ((out && out->end() == cut) || (in && in->start() == cut)) {
            return &tr;
        }
    }
    return nullptr;
}

/// A WAV file of `frames` silent frames.
void writeWav(const fs::path& file, int channels, int rate, std::int64_t frames) {
    const std::uint64_t bytes = static_cast<std::uint64_t>(frames) * static_cast<std::uint64_t>(channels) * 2;
    it::writeFile(file, graph::wavHeader(channels, rate, 16, bytes) + std::string(static_cast<std::size_t>(bytes), '\0'));
}

// --- Hand-built AAF files, in the shapes other editors write ---------------------------------------------------------

constexpr aaf::Auid kSound{0x01030202, 0x0200, 0x0000, {0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01, 0x01}};
constexpr aaf::Auid kPicture{0x01030202, 0x0100, 0x0000, {0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01, 0x01}};
constexpr aaf::Auid kTimecode{0x01030201, 0x0100, 0x0000, {0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01, 0x01}};
constexpr aaf::Auid kDescriptive{0x01030201, 0x1000, 0x0000, {0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01, 0x01}};
constexpr aaf::Auid kGain{0x9d2ea894, 0x0968, 0x11d3, {0x8a, 0x38, 0x00, 0x50, 0x04, 0x0e, 0xf7, 0xd2}};
constexpr aaf::Auid kPan{0x9d2ea893, 0x0968, 0x11d3, {0x8a, 0x38, 0x00, 0x50, 0x04, 0x0e, 0xf7, 0xd2}};
constexpr aaf::Auid kDissolve{0x0c3bea41, 0xfc05, 0x11d2, {0x8a, 0x29, 0x00, 0x50, 0x04, 0x0e, 0xf7, 0xd2}};
/// Avid's operation definitions as Media Composer names them (identifiers made up: the importer knows them by name).
constexpr aaf::Auid kCombiner{0x11111111, 0x0001, 0x0001, {0x80, 0, 0, 0, 0, 0, 0, 1}};
constexpr aaf::Auid kAudioEq{0x11111111, 0x0001, 0x0002, {0x80, 0, 0, 0, 0, 0, 0, 2}};
constexpr aaf::Auid kAvidGain{0x11111111, 0x0001, 0x0003, {0x80, 0, 0, 0, 0, 0, 0, 3}};
constexpr aaf::Auid kAmplitude{0xe4962321, 0x2267, 0x11d3, {0x8a, 0x4c, 0x00, 0x50, 0x04, 0x0e, 0xf7, 0xd2}};
constexpr aaf::Auid kPanParam{0xe4962322, 0x2267, 0x11d3, {0x8a, 0x4c, 0x00, 0x50, 0x04, 0x0e, 0xf7, 0xd2}};
constexpr aaf::Auid kLevel{0xe4962320, 0x2267, 0x11d3, {0x8a, 0x4c, 0x00, 0x50, 0x04, 0x0e, 0xf7, 0xd2}};
constexpr aaf::Auid kAvidVolume{0x47c00f7d, 0xfb55, 0x11d3, {0x9f, 0xf7, 0x00, 0x04, 0xac, 0x96, 0x9f, 0x50}};
constexpr aaf::Auid kAvidFadeType{0x8134fc48, 0x3bd6, 0x4904, {0x95, 0x20, 0x7a, 0x63, 0x60, 0xcc, 0x71, 0x27}};
constexpr aaf::Auid kLinear{0x5b6c85a4, 0x0ede, 0x11d3, {0x80, 0xa9, 0x00, 0x60, 0x08, 0x14, 0x3e, 0x6f}};
constexpr aaf::Auid kExternal{0x4313b572, 0xd8ba, 0x11d2, {0x80, 0x9b, 0x00, 0x60, 0x08, 0x14, 0x3e, 0x6f}};
constexpr aaf::Auid kRationalType{0x03010100, 0x0000, 0x0000, {0x06, 0x0e, 0x2b, 0x34, 0x01, 0x04, 0x01, 0x01}};
constexpr aaf::Auid kSubClip{0x0d010102, 0x0101, 0x0500, {0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01, 0x01}};

std::string rationalBytes(std::int32_t num, std::int32_t den) {
    std::string out;
    for (const std::uint32_t v : {static_cast<std::uint32_t>(num), static_cast<std::uint32_t>(den)}) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
        }
    }
    return out;
}

/// Builds AAF files the way other applications lay them out.
struct AafWriter {
    aaf::File file;
    aaf::TimeStamp now{2026, 11, 1, 12, 0, 0, 0};
    int nextId = 1;

    AafWriter() {
        aaf::Object& h = file.header();
        h.setInt("ByteOrder", 0x4949)
            .setTimeStamp("LastModified", now)
            .setRaw("Version", std::string("\x01\x01", 2))
            .setInt("ObjectModelVersion", 1)
            .setAuid("OperationalPattern", aaf::kOpEditProtocol);
        h.append("IdentificationList", "Identification")
            .setString("CompanyName", "Test")
            .setString("ProductName", "Hand-built")
            .setRaw("ProductVersion", std::string("\x01\x00\x00\x00\x00\x00\x00\x00\x01", 9))
            .setString("ProductVersionString", "1.0")
            .setAuid("ProductID", aaf::Auid{0x12345678, 1, 2, {1, 2, 3, 4, 5, 6, 7, 8}})
            .setTimeStamp("Date", now)
            .setAuid("GenerationAUID", aaf::Auid{0x12345678, 1, 3, {1, 2, 3, 4, 5, 6, 7, 9}})
            .setString("Platform", "Test");
        aaf::Object& dict = file.dictionary();
        auto def = [&](std::string_view property, std::string_view cls, const aaf::Auid& id, std::string_view name) -> aaf::Object& {
            return dict.append(property, cls).setAuid("Identification", id).setString("Name", name).setString("Description", name);
        };
        def("DataDefinitions", "DataDefinition", kSound, "Sound");
        def("DataDefinitions", "DataDefinition", kPicture, "Picture");
        def("DataDefinitions", "DataDefinition", kTimecode, "Timecode");
        def("DataDefinitions", "DataDefinition", kDescriptive, "DescriptiveMetadata");
        for (const auto& [id, name] : {std::pair{kAmplitude, "Amplitude"}, std::pair{kPanParam, "Pan"}, std::pair{kLevel, "Level"},
                                       std::pair{kAvidVolume, "Volume"}, std::pair{kAvidFadeType, "Fade Type"}}) {
            def("ParameterDefinitions", "ParameterDefinition", id, name).setWeakRef("Type", kRationalType);
        }
        auto op = [&](const aaf::Auid& id, std::string_view name, int inputs) {
            def("OperationDefinitions", "OperationDefinition", id, name)
                .setWeakRef("DataDefinition", kSound)
                .setBool("IsTimeWarp", false)
                .setInt("NumberInputs", inputs);
        };
        op(kGain, "Mono Audio Gain", 1);
        op(kPan, "Mono Audio Pan", 1);
        op(kDissolve, "Mono Audio Dissolve", 2);
        op(kCombiner, "Audio Channel Combiner", -1);
        op(kAudioEq, "Audio EQ 3 Band", 1);
        op(kAvidGain, "Audio Gain", 1);
        def("InterpolationDefinitions", "InterpolationDefinition", kLinear, "LinearInterp");
        def("ContainerDefinitions", "ContainerDefinition", kExternal, "External");
    }

    aaf::MobId id() {
        aaf::Auid a{static_cast<std::uint32_t>(0x0A000000 + nextId++), 0x1111, 0x2222, {9, 8, 7, 6, 5, 4, 3, 2}};
        return aaf::MobId::fromMaterial(a);
    }
    aaf::Object& mob(std::string_view cls, const aaf::MobId& mobId, std::string_view name) {
        aaf::Object& m = file.contentStorage().append("Mobs", cls);
        m.setMobId("MobID", mobId).setString("Name", name).setTimeStamp("CreationTime", now).setTimeStamp("LastModified", now);
        return m;
    }
    static aaf::Object& slot(aaf::Object& mob, std::uint32_t slotId, std::int32_t rateNum, std::int32_t rateDen,
                             std::string_view name = {}, std::uint32_t physical = 0) {
        aaf::Object& s = mob.append("Slots", "TimelineMobSlot");
        s.setInt("SlotID", slotId).setRational("EditRate", rateNum, rateDen).setInt("Origin", 0);
        if (!name.empty()) {
            s.setString("SlotName", name);
        }
        if (physical > 0) {
            s.setInt("PhysicalTrackNumber", physical);
        }
        return s;
    }
    static aaf::Object& component(aaf::Object& parent, std::string_view property, bool append, std::string_view cls,
                                  const aaf::Auid& data, std::int64_t length) {
        aaf::Object& c = append ? parent.append(property, cls) : parent.child(property, cls);
        c.setWeakRef("DataDefinition", data).setInt("Length", length);
        return c;
    }
    static aaf::Object& clip(aaf::Object& parent, std::string_view property, bool append, const aaf::MobId& source,
                             std::int64_t start, std::int64_t length, std::uint32_t sourceSlot = 1) {
        return component(parent, property, append, "SourceClip", kSound, length)
            .setInt("StartTime", start)
            .setMobId("SourceID", source)
            .setInt("SourceMobSlotID", sourceSlot);
    }
    /// A file source mob (PCM at `rate`, `channels`, linked to `url`) and its master mob; returns the master's id.
    aaf::MobId media(std::string_view name, int rate, int channels, std::int64_t length, const std::string& url,
                     std::uint32_t fileSlotPhysical = 0, aaf::MobId* fileId = nullptr) {
        const aaf::MobId fileMob = fileId && !(*fileId == aaf::MobId{}) ? *fileId : id();
        if (!fileId || *fileId == aaf::MobId{}) {
            aaf::Object& src = mob("SourceMob", fileMob, name);
            aaf::Object& s = slot(src, 1, rate, 1, {}, fileSlotPhysical);
            clip(s, "Segment", false, aaf::MobId{}, 0, length, 0);
            aaf::Object& d = src.child("EssenceDescription", "PCMDescriptor");
            d.setRational("SampleRate", rate, 1)
                .setInt("Length", length)
                .setRational("AudioSamplingRate", rate, 1)
                .setInt("Channels", channels)
                .setInt("QuantizationBits", 16)
                .setInt("BlockAlign", channels * 2)
                .setInt("AverageBPS", channels * 2 * rate)
                .setWeakRef("ContainerFormat", kExternal);
            d.append("Locator", "NetworkLocator").setString("URLString", url);
            if (fileId) {
                *fileId = fileMob;
            }
        }
        const aaf::MobId master = id();
        aaf::Object& mm = mob("MasterMob", master, name);
        aaf::Object& ms = slot(mm, 1, rate, 1, {}, fileSlotPhysical);
        clip(ms, "Segment", false, fileMob, 0, length, 1);
        return master;
    }
    aaf::Object& composition(std::string_view name, const aaf::Auid& usage = aaf::kUsageTopLevel, aaf::MobId* mobId = nullptr) {
        const aaf::MobId made = id();
        if (mobId) {
            *mobId = made;
        }
        return mob("CompositionMob", made, name).setAuid("UsageCode", usage);
    }
    static void timecode(aaf::Object& comp, std::uint32_t slotId, std::int32_t rateNum, std::int32_t rateDen, int fps,
                         std::int64_t start, std::int64_t length) {
        aaf::Object& s = slot(comp, slotId, rateNum, rateDen, "TC1", 1);
        component(s, "Segment", false, "Timecode", kTimecode, length).setInt("Start", start).setInt("FPS", fps).setInt("Drop", 0);
    }
    static aaf::Object& constant(aaf::Object& group, const aaf::Auid& definition, std::int32_t num, std::int32_t den) {
        return group.append("Parameters", "ConstantValue")
            .setAuid("Definition", definition)
            .setIndirect("Value", "Rational", rationalBytes(num, den));
    }
    static aaf::Object& varying(aaf::Object& group, const aaf::Auid& definition,
                                const std::vector<std::pair<std::pair<int, int>, std::pair<int, int>>>& points) {
        aaf::Object& v = group.append("Parameters", "VaryingValue");
        v.setAuid("Definition", definition).setWeakRef("Interpolation", kLinear);
        for (const auto& [time, value] : points) {
            v.append("PointList", "ControlPoint")
                .setRational("Time", time.first, time.second)
                .setIndirect("Value", "Rational", rationalBytes(value.first, value.second));
        }
        return v;
    }
    fs::path write(const fs::path& path) {
        INFO(it::Strings(file.problems()).size());
        for (const std::string& p : file.problems()) {
            UNSCOPED_INFO(p);
        }
        for (const std::string& p : file.missingRequired()) {
            UNSCOPED_INFO("missing " << p);
        }
        const auto written = file.write(path);
        INFO((written ? std::string() : written.error().message));
        REQUIRE(written.has_value());
        return path;
    }
};

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Wabcut's own files

TEST_CASE("AAF import: the golden export reads back with tracks clips fades levels and markers (0.6)", "[interchange][aaf-import]") {
    const fs::path dir = it::scratchDir("aaf-import-golden");
    ImportOptions options = scratchOptions(dir);
    const ImportResult r = importOk(it::fixturePath("export/audiopost.aaf"), options);
    CHECK(r.format == Format::Aaf);
    REQUIRE(r.sequences.size() == 1);
    const Sequence& s = it::sequenceAt(r, 0);
    const Rate rate{25, 1};
    CHECK(s.name == "Fixture");
    CHECK(s.frameRate == rate);
    CHECK(s.startTimecode == Ticks::fromSeconds(3600));
    CHECK(s.audioRate == kRate48kHz);
    // Stereo tracks were broken out to mono: four mono tracks panned hard left and right.
    CHECK(trackNames(s) == std::vector<std::string>{"Audio 1 Left", "Audio 1 Right", "Audio 2 Left", "Audio 2 Right"});
    for (const TrackPtr& t : s.audioTracks) {
        CHECK(t->format == ChannelFormat::Mono);
    }
    CHECK(it::track(s, "A1").pan.constant() == -1.0);
    CHECK(it::track(s, "A2").pan.constant() == 1.0);
    // A plays to the crossfade's cut, B from it (its file starts 2 frames early: its handle); C at frame 4.
    CHECK(it::clips(s, "A1") == it::Strings{"0+10 A A1 Left @0", "10+10 B A1 Left @2"});
    CHECK(it::clips(s, "A3") == it::Strings{"4+6 C A2 Left @2"});
    const Transition* cross = transitionAt(it::track(s, "A1"), it::frames(10, rate));
    REQUIRE(cross != nullptr);
    CHECK(cross->effect == "constantPower");
    CHECK(cross->before == it::frames(2, rate));
    CHECK(cross->after == it::frames(2, rate));
    const Transition* fadeIn = transitionAt(it::track(s, "A3"), it::frames(4, rate));
    REQUIRE(fadeIn != nullptr);
    CHECK(fadeIn->effect == "constantGain"); // the fade's dissolve has a linear curve
    CHECK(fadeIn->after == it::frames(3, rate));
    // Clip gain: B -6 dB, A keyframed from 0 to -12 dB; track volume -3 dB on A2's tracks.
    CHECK(std::abs(volumeDb(clipAt(s, "A1", 1)) + 6.0) < 0.01);
    const Curve* gain = clipAt(s, "A1", 0).properties()[ParamId::VolumeDb].curve();
    REQUIRE(gain != nullptr);
    CHECK(std::abs(gain->valueAt(Ticks{}) - 0.0) < 0.01);
    CHECK(std::abs(gain->valueAt(it::frames(9, rate)) + 12.0) < 0.01);
    CHECK(std::abs(it::track(s, "A3").volume.constant() + 3.0) < 0.01);
    CHECK(std::abs(it::track(s, "A1").volume.constant()) < 1e-9);
    // The marker.
    REQUIRE(s.markers.size() == 1);
    CHECK(s.markers.front().start == it::frames(5, rate));
    CHECK(s.markers.front().name == "Mix note");
    CHECK(s.markers.front().comment == "Music up");
    // The video clip links to its original file, which is not here: offline, reported, still in the timeline.
    CHECK(it::clips(s, "V1") == it::Strings{"0+10 A @0"});
    REQUIRE(r.unresolvedMedia.size() == 1);
    CHECK(r.unresolvedMedia.front().path.ends_with("footage/A.mov"));
    // The embedded audio was written out and is online.
    std::size_t wavs = 0;
    for (const auto& e : fs::directory_iterator(dir / "Embedded")) {
        wavs += e.path().extension() == ".wav" ? 1 : 0;
    }
    CHECK(wavs == 6);
    for (const MediaId m : r.media) {
        const MediaItem* item = r.project.findMedia(m);
        REQUIRE(item != nullptr);
        if (item->info.video.empty()) {
            CHECK_FALSE(item->locator.offline);
            CHECK(fs::exists(pathFromUtf8(item->locator.path)));
        }
    }
    CHECK(it::hasEntry(r.report, ReportOutcome::Carried, ReportCategory::Media, "embedded audio was written to"));
    CHECK(it::hasEntry(r.report, ReportOutcome::Carried, ReportCategory::Track, "track volume on 'Audio 2 Left'"));
    CHECK(it::hasEntry(r.report, ReportOutcome::Carried, ReportCategory::Effect, "clip gain -6.0 dB on 'B A1 Left' at 01:00:00:10"));
    // Importing again reuses the files written the first time.
    const ImportResult again = importOk(it::fixturePath("export/audiopost.aaf"), options);
    std::size_t after = 0;
    for (const auto& e : fs::directory_iterator(dir / "Embedded")) {
        after += e.path().extension() == ".wav" ? 1 : 0;
    }
    CHECK(after == 6);
}

TEST_CASE("OMF import: the golden export reads back with tracks clips fades gain and pan (0.6)", "[interchange][omf-import]") {
    const fs::path dir = it::scratchDir("omf-import-golden");
    const ImportResult r = importOk(it::fixturePath("export/audiopost.omf"), scratchOptions(dir));
    CHECK(r.format == Format::Omf);
    REQUIRE(r.sequences.size() == 1);
    const Sequence& s = it::sequenceAt(r, 0);
    const Rate rate{25, 1};
    CHECK(s.frameRate == rate);
    CHECK(s.startTimecode == Ticks::fromSeconds(3600));
    CHECK(trackNames(s) == std::vector<std::string>{"Audio 1 Left", "Audio 1 Right", "Audio 2 Left", "Audio 2 Right"});
    CHECK(it::clips(s, "A1") == it::Strings{"0+10 A A1 Left @0", "10+10 B A1 Left @2"});
    CHECK(it::clips(s, "A3") == it::Strings{"4+6 C A2 Left @2"});
    CHECK(it::track(s, "A1").pan.constant() == -1.0);
    CHECK(std::abs(it::track(s, "A3").volume.constant() + 3.0) < 0.01);
    CHECK(std::abs(volumeDb(clipAt(s, "A1", 1)) + 6.0) < 0.01);
    REQUIRE(clipAt(s, "A1", 0).properties()[ParamId::VolumeDb].curve() != nullptr);
    // OMF's clip fade (a Constant Gain fade in written as a linear fade) and its dissolve (equal power again).
    const Transition* fadeIn = transitionAt(it::track(s, "A3"), it::frames(4, rate));
    REQUIRE(fadeIn != nullptr);
    CHECK(fadeIn->effect == "constantGain");
    CHECK(fadeIn->after == it::frames(3, rate));
    const Transition* cross = transitionAt(it::track(s, "A1"), it::frames(10, rate));
    REQUIRE(cross != nullptr);
    CHECK(cross->effect == "constantPower");
    CHECK(s.markers.empty());
    CHECK(r.unresolvedMedia.empty());
    CHECK(s.videoTracks.empty()); // OMF brings the audio
}

TEST_CASE("AAF and OMF import: Wabcut's exports round-trip at 48 and 96 kHz with embedded and separate audio (0.6)",
          "[interchange][aaf-import][omf-import]") {
    auto tl = audioPostTimeline();
    for (const int rateHz : {48000, 96000}) {
        for (const AudioPostFiles files : {AudioPostFiles::Embed, AudioPostFiles::Separate}) {
            for (const bool omf : {false, true}) {
                INFO((omf ? "OMF " : "AAF ") << rateHz << (files == AudioPostFiles::Embed ? " embedded" : " separate"));
                const fs::path dir = it::scratchDir(std::string(omf ? "omf" : "aaf") + "-roundtrip-" + std::to_string(rateHz) +
                                                    (files == AudioPostFiles::Embed ? "-embed" : "-separate"));
                TestRenderer renderer;
                AudioPostHost host;
                host.audio = &renderer;
                const fs::path file = dir / (omf ? "Mix.omf" : "Mix.aaf");
                if (omf) {
                    OmfExportOptions o;
                    o.timestamp = fixedTime();
                    o.audio.handleFrames = 2;
                    o.audio.sampleRate = rateHz;
                    o.audio.files = files;
                    REQUIRE(exportOmfToFile(tl.state, tl.seq, file, o, host).has_value());
                } else {
                    AafExportOptions o;
                    o.timestamp = fixedTime();
                    o.audio.handleFrames = 2;
                    o.audio.sampleRate = rateHz;
                    o.audio.files = files;
                    REQUIRE(exportAafToFile(tl.state, tl.seq, file, o, host).has_value());
                }
                const ImportResult r = importOk(file, scratchOptions(dir));
                const Sequence& s = it::sequenceAt(r, 0);
                CHECK(s.audioRate == Rate{rateHz, 1});
                CHECK(it::clips(s, "A1") == it::Strings{"0+10 A A1 Left @0", "10+10 B A1 Left @2"});
                CHECK(it::clips(s, "A3") == it::Strings{"4+6 C A2 Left @2"});
                CHECK(std::abs(volumeDb(clipAt(s, "A1", 1)) + 6.0) < 0.01);
                CHECK(std::abs(it::track(s, "A3").volume.constant() + 3.0) < 0.01);
                REQUIRE(transitionAt(it::track(s, "A1"), it::frames(10, Rate{25, 1})) != nullptr);
                REQUIRE(transitionAt(it::track(s, "A3"), it::frames(4, Rate{25, 1})) != nullptr);
                // Every audio file is found: written out of the file (embedded) or next to it (separate).
                for (const MediaId m : r.media) {
                    const MediaItem* item = r.project.findMedia(m);
                    if (item && item->info.video.empty()) {
                        CHECK_FALSE(item->locator.offline);
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Other editors' files, hand-built

TEST_CASE("AAF import: a Pro Tools style session at 96 kHz with 44.1 kHz media and a missing file (0.6)", "[interchange][aaf-import]") {
    const fs::path dir = it::scratchDir("aaf-import-96k");
    writeWav(dir / "Audio Files" / "Dialog_01.wav", 1, 96000, 96000 * 4);
    writeWav(dir / "Audio Files" / "Music_01.wav", 2, 44100, 44100 * 6);
    AafWriter w;
    const aaf::MobId dialog = w.media("Dialog_01", 96000, 1, 96000 * 4,
                                      pathToFileUrl(pathToUtf8(dir / "Audio Files" / "Dialog_01.wav"), FileUrlStyle::Triple));
    const aaf::MobId music = w.media("Music_01", 44100, 2, 44100 * 6,
                                     pathToFileUrl(pathToUtf8(dir / "Audio Files" / "Music_01.wav"), FileUrlStyle::Triple));
    const aaf::MobId missing = w.media("Room tone", 96000, 1, 96000 * 2, "file:///Volumes/Session/Audio%20Files/Room%20tone.wav");
    aaf::Object& comp = w.composition("Spot mix");
    AafWriter::timecode(comp, 1, 30, 1, 30, 30 * 3600, 30 * 20);
    // Track 1: dialog from 1 s, 2 s long, from 0.5 s into the file; then the missing room tone at 4 s.
    aaf::Object& a1 = AafWriter::slot(comp, 2, 96000, 1, "Dialog", 1);
    aaf::Object& seq1 = AafWriter::component(a1, "Segment", false, "Sequence", kSound, 96000 * 6);
    AafWriter::component(seq1, "Components", true, "Filler", kSound, 96000);
    AafWriter::clip(seq1, "Components", true, dialog, 48000, 96000 * 2);
    AafWriter::component(seq1, "Components", true, "Filler", kSound, 96000);
    AafWriter::clip(seq1, "Components", true, missing, 0, 96000 * 2);
    // Track 2: the 44.1 kHz stereo music, whole, at 0 (its source position in the composition's 96 kHz units).
    aaf::Object& a2 = AafWriter::slot(comp, 3, 96000, 1, "Music", 2);
    aaf::Object& seq2 = AafWriter::component(a2, "Segment", false, "Sequence", kSound, 96000 * 6);
    AafWriter::clip(seq2, "Components", true, music, 0, 96000 * 6);
    const fs::path file = w.write(dir / "Spot mix.aaf");

    const ImportResult r = importOk(file, scratchOptions(dir));
    const Sequence& s = it::sequenceAt(r, 0);
    CHECK(s.name == "Spot mix");
    CHECK(s.audioRate == Rate{96000, 1});
    CHECK(s.frameRate == Rate{30, 1});
    CHECK(s.startTimecode == Ticks::fromSeconds(3600));
    CHECK(it::clips(s, "A1") == it::Strings{"30+60 Dialog_01 @15", "120+60 Room tone @0"});
    CHECK(it::track(s, "A1").format == ChannelFormat::Mono);
    CHECK(it::track(s, "A2").format == ChannelFormat::Stereo);
    CHECK(it::clips(s, "A2") == it::Strings{"0+180 Music_01 @0"});
    // The missing file: offline, reported, its clip in place.
    REQUIRE(r.unresolvedMedia.size() == 1);
    CHECK(r.unresolvedMedia.front().name == "Room tone");
    CHECK(it::hasEntry(r.report, ReportOutcome::Approximated, ReportCategory::Media, "'Room tone' is offline"));
    CHECK(it::hasEntry(r.report, ReportOutcome::Carried, ReportCategory::AudioMapping, "44100 Hz media, converted to the sequence's 96000 Hz"));
    CHECK(it::mediaNamed(r, "Dialog_01").info.audio.front().sampleRate == Rate{96000, 1});
    CHECK_FALSE(it::mediaNamed(r, "Dialog_01").locator.offline);
}

TEST_CASE("AAF import: 5.1 as six mono files, as one six-channel file and as six named tracks (0.6)", "[interchange][aaf-import]") {
    const fs::path dir = it::scratchDir("aaf-import-51");
    // Pro Tools film order L C R Ls Rs LFE, one mono file each, under one channel combiner.
    static constexpr const char* kFilm[6] = {"L", "C", "R", "Ls", "Rs", "LFE"};
    AafWriter w;
    std::vector<aaf::MobId> mono;
    for (const char* ch : kFilm) {
        const std::string name = std::string("Score.") + ch;
        writeWav(dir / (name + ".wav"), 1, 48000, 48000 * 3);
        mono.push_back(w.media(name, 48000, 1, 48000 * 3, pathToFileUrl(pathToUtf8(dir / (name + ".wav")), FileUrlStyle::Triple)));
    }
    // Resolve: one six-channel file, a file mob per channel naming its channel by physical track number.
    writeWav(dir / "Stems.wav", 6, 48000, 48000 * 3);
    const std::string stems = pathToFileUrl(pathToUtf8(dir / "Stems.wav"), FileUrlStyle::Triple);
    std::vector<aaf::MobId> channels;
    for (std::uint32_t k = 1; k <= 6; ++k) {
        aaf::MobId fileMob{};
        channels.push_back(w.media("Stems", 48000, 6, 48000 * 3, stems, k, &fileMob));
    }
    // Wabcut / Premiere breakout: six mono tracks "Mix L".."Mix Rs".
    writeWav(dir / "Mix.wav", 1, 48000, 48000 * 3);
    const aaf::MobId mix = w.media("Mix", 48000, 1, 48000 * 3, pathToFileUrl(pathToUtf8(dir / "Mix.wav"), FileUrlStyle::Triple));

    aaf::Object& comp = w.composition("Surround");
    AafWriter::timecode(comp, 1, 25, 1, 25, 0, 75);
    aaf::Object& score = AafWriter::slot(comp, 2, 48000, 1, "Score", 1);
    aaf::Object& scoreSeq = AafWriter::component(score, "Segment", false, "Sequence", kSound, 48000 * 3);
    aaf::Object& combiner = AafWriter::component(scoreSeq, "Components", true, "OperationGroup", kSound, 48000 * 2);
    combiner.setWeakRef("Operation", kCombiner);
    for (const aaf::MobId& m : mono) {
        AafWriter::clip(combiner, "InputSegments", true, m, 0, 48000 * 2);
    }
    aaf::Object& stemsSlot = AafWriter::slot(comp, 3, 48000, 1, "Stems", 2);
    aaf::Object& stemsSeq = AafWriter::component(stemsSlot, "Segment", false, "Sequence", kSound, 48000 * 3);
    aaf::Object& combiner2 = AafWriter::component(stemsSeq, "Components", true, "OperationGroup", kSound, 48000 * 3);
    combiner2.setWeakRef("Operation", kCombiner);
    for (const aaf::MobId& m : channels) {
        AafWriter::clip(combiner2, "InputSegments", true, m, 0, 48000 * 3);
    }
    static constexpr const char* kSmpte[6] = {"L", "R", "C", "LFE", "Ls", "Rs"};
    for (std::uint32_t k = 0; k < 6; ++k) {
        aaf::Object& t = AafWriter::slot(comp, 4 + k, 48000, 1, std::string("Mix ") + kSmpte[k], 3 + k);
        aaf::Object& tSeq = AafWriter::component(t, "Segment", false, "Sequence", kSound, 48000 * 3);
        AafWriter::clip(tSeq, "Components", true, mix, 0, 48000);
    }
    const fs::path file = w.write(dir / "Surround.aaf");

    const ImportResult r = importOk(file, scratchOptions(dir));
    const Sequence& s = it::sequenceAt(r, 0);
    CHECK(s.masterFormat == ChannelFormat::Surround51);
    REQUIRE(s.audioTracks.size() == 6 + 1 + 6);
    // Six mono files: six linked tracks in Wabcut's order, each clip routed to its speaker.
    const std::vector<std::string> names = trackNames(s);
    CHECK(std::vector<std::string>(names.begin(), names.begin() + 6) ==
          std::vector<std::string>{"Score L", "Score C", "Score R", "Score Ls", "Score Rs", "Score LFE"});
    static constexpr int kSpeakerOfFilm[6] = {0, 2, 1, 4, 5, 3};
    for (std::size_t k = 0; k < 6; ++k) {
        const Track& t = *s.audioTracks[k];
        CHECK(t.format == ChannelFormat::Adaptive);
        REQUIRE(t.clips.size() == 1);
        const AudioChannelMap* map = t.clips[0].channelMap();
        REQUIRE(map != nullptr);
        REQUIRE(map->channels.size() == 6);
        for (std::size_t c = 0; c < 6; ++c) {
            CHECK(map->channels[c].sources == (static_cast<int>(c) == kSpeakerOfFilm[k] ? std::vector<std::int32_t>{0} : std::vector<std::int32_t>{}));
        }
    }
    // Linked: the six channels of the score clip, and the six clips of the one "Mix" master clip.
    REQUIRE(it::links(s).size() == 2);
    CHECK(it::links(s).front() == "A1@0+A2@0+A3@0+A4@0+A5@0+A6@0");
    // One six-channel file: one 5.1 clip playing it as it is.
    const Track& stemsTrack = *s.audioTracks[6];
    CHECK(stemsTrack.name == "Stems");
    CHECK(stemsTrack.format == ChannelFormat::Surround51);
    REQUIRE(stemsTrack.clips.size() == 1);
    CHECK(stemsTrack.clips[0].channelMap() == nullptr);
    CHECK(stemsTrack.clips[0].duration() == Ticks::fromSeconds(3));
    // Six named mono tracks: routed to their speakers.
    for (std::size_t k = 0; k < 6; ++k) {
        const Track& t = *s.audioTracks[7 + k];
        CHECK(t.format == ChannelFormat::Adaptive);
        REQUIRE_FALSE(t.clips.empty());
        const AudioChannelMap* map = t.clips[0].channelMap();
        REQUIRE(map != nullptr);
        CHECK(map->channels.at(k).sources == std::vector<std::int32_t>{0});
    }
    CHECK(it::hasEntry(r.report, ReportOutcome::Carried, ReportCategory::AudioMapping, "5.1 layout: tracks 'Mix L' to 'Mix Rs'"));
    CHECK(it::hasEntry(r.report, ReportOutcome::Approximated, ReportCategory::AudioMapping, "became six linked tracks"));
}

TEST_CASE("AAF import: Media Composer subclips gain keyframes muted clips fades track pan markers and effects (0.6)",
          "[interchange][aaf-import]") {
    const fs::path dir = it::scratchDir("aaf-import-mc");
    writeWav(dir / "Tone.wav", 1, 48000, 48000 * 10);
    AafWriter w;
    const aaf::MobId tone = w.media("Tone", 48000, 1, 48000 * 10, pathToFileUrl(pathToUtf8(dir / "Tone.wav"), FileUrlStyle::Triple));
    // A subclip composition (Usage_SubClip): +6 dB of gain around 1 s of the tone from 1 s.
    aaf::MobId subId{};
    aaf::Object& sub = w.composition("Tone.Sub.01", kSubClip, &subId);
    aaf::Object& subSlot = AafWriter::slot(sub, 1, 30000, 1001, {}, 1);
    aaf::Object& subGain = AafWriter::component(subSlot, "Segment", false, "OperationGroup", kSound, 30);
    subGain.setWeakRef("Operation", kAvidGain);
    AafWriter::clip(subGain, "InputSegments", true, tone, 30, 30);
    AafWriter::constant(subGain, kAmplitude, 2 * 536870912, 536870912); // x2 = +6.02 dB

    aaf::Object& comp = w.composition("MC Mix");
    AafWriter::timecode(comp, 1, 30000, 1001, 30, 108000, 300);
    aaf::Object& a1 = AafWriter::slot(comp, 3, 30000, 1001, {}, 1);
    // Track pan automation around the whole track: left at the start, right at the end.
    aaf::Object& pan = AafWriter::component(a1, "Segment", false, "OperationGroup", kSound, 150);
    pan.setWeakRef("Operation", kPan);
    aaf::Object& seq = AafWriter::component(pan, "InputSegments", true, "Sequence", kSound, 150);
    AafWriter::varying(pan, kPanParam, {{{0, 150}, {0, 1}}, {{150, 150}, {1, 1}}});
    // 1. Fade in (from filler), 30 frames of the tone with Avid volume keyframes (0 dB -> silence -> 0 dB).
    AafWriter::component(seq, "Components", true, "Filler", kSound, 5);
    aaf::Object& fade = AafWriter::component(seq, "Components", true, "Transition", kSound, 5);
    fade.setInt("CutPoint", 0);
    aaf::Object& dissolve = AafWriter::component(fade, "OperationGroup", false, "OperationGroup", kSound, 5);
    dissolve.setWeakRef("Operation", kDissolve);
    AafWriter::constant(dissolve, kAvidFadeType, 0, 1); // linear
    aaf::Object& keyed = AafWriter::component(seq, "Components", true, "OperationGroup", kSound, 30);
    keyed.setWeakRef("Operation", kAvidGain);
    AafWriter::clip(keyed, "InputSegments", true, tone, 30, 30);
    AafWriter::varying(keyed, kAvidVolume, {{{0, 30}, {1, 1}}, {{15, 30}, {0, 1}}, {{30, 30}, {1, 1}}});
    // 2. The subclip at frame 30.
    AafWriter::clip(seq, "Components", true, subId, 0, 30);
    // 3. A muted clip: Filler selected over it.
    aaf::Object& selector = AafWriter::component(seq, "Components", true, "Selector", kSound, 30);
    AafWriter::component(selector, "Selected", false, "Filler", kSound, 30);
    AafWriter::clip(selector, "Alternates", true, tone, 90, 30);
    // 4. An effect Wabcut does not carry, around a clip.
    aaf::Object& eq = AafWriter::component(seq, "Components", true, "OperationGroup", kSound, 30);
    eq.setWeakRef("Operation", kAudioEq);
    AafWriter::clip(eq, "InputSegments", true, tone, 150, 30);
    AafWriter::component(seq, "Components", true, "Filler", kSound, 30);
    // Markers.
    aaf::Object& events = comp.append("Slots", "EventMobSlot");
    events.setInt("SlotID", 9).setString("SlotName", "Markers").setRational("EditRate", 30000, 1001);
    aaf::Object& markers = events.child("Segment", "Sequence");
    markers.setWeakRef("DataDefinition", kDescriptive);
    markers.append("Components", "DescriptiveMarker")
        .setWeakRef("DataDefinition", kDescriptive)
        .setInt("Position", 45)
        .setString("Comment", "Fix the pop");
    const fs::path file = w.write(dir / "MC Mix.aaf");

    const ImportResult r = importOk(file, scratchOptions(dir));
    REQUIRE(r.sequences.size() == 1); // the subclip composition is not a sequence of its own
    const Sequence& s = it::sequenceAt(r, 0);
    const Rate ntsc{30000, 1001};
    CHECK(s.frameRate == ntsc);
    CHECK(it::clips(s, "A1") == it::Strings{"0+30 Tone @30", "30+30 Tone.Sub.01 @30", "60+30 Tone @90 off", "90+30 Tone @150"});
    // Volume keyframes in media time: 0 dB at the clip's first frame, silence in the middle.
    const Curve* volume = clipAt(s, "A1", 0).properties()[ParamId::VolumeDb].curve();
    REQUIRE(volume != nullptr);
    CHECK(std::abs(volume->valueAt(it::frames(30, ntsc))) < 0.01);
    CHECK(volume->valueAt(it::frames(45, ntsc)) <= -144.0 + 1e-9);
    CHECK(std::abs(volumeDb(clipAt(s, "A1", 1)) - 6.0206) < 0.01);
    const Transition* fadeIn = transitionAt(it::track(s, "A1"), Ticks{});
    REQUIRE(fadeIn != nullptr);
    CHECK(fadeIn->effect == "constantGain");
    CHECK(fadeIn->after == it::frames(5, ntsc));
    const Curve* trackPan = it::track(s, "A1").pan.curve();
    REQUIRE(trackPan != nullptr);
    CHECK(trackPan->valueAt(Ticks{}) == -1.0);
    CHECK(trackPan->valueAt(it::frames(150, ntsc)) == 1.0);
    REQUIRE(s.markers.size() == 1);
    CHECK(s.markers.front().start == it::frames(45, ntsc));
    CHECK(s.markers.front().name == "Fix the pop");
    CHECK(it::hasEntry(r.report, ReportOutcome::Dropped, ReportCategory::Effect, "'Audio EQ 3 Band' at 01:00:03:00"));
    CHECK(it::hasEntry(r.report, ReportOutcome::Carried, ReportCategory::Clip, "muted clip 'Tone'"));
    CHECK(it::hasEntry(r.report, ReportOutcome::Carried, ReportCategory::Track, "track pan automation"));
}

// ---------------------------------------------------------------------------------------------------------------------
// Detection and malformed input

// Fix round 1 (ARGON-08): clips are linked by the master clip (MasterMob) they play, not by their name; the two mono
// halves of a stereo clip ("Dialogue.L" / "Dialogue.R", two master clips in Pro Tools) are linked too.
TEST_CASE("AAF import: clips link by their master clip not by their name (0.6)", "[interchange][aaf-import]") {
    const fs::path dir = it::scratchDir("aaf-import-links");
    AafWriter w;
    const auto missing = [](std::string_view name) {
        return "file:///Volumes/Session/Audio%20Files/" + std::string(name) + ".wav";
    };
    const aaf::MobId toneA = w.media("Room Tone", 48000, 1, 48000 * 4, missing("Room Tone 1"));
    const aaf::MobId toneB = w.media("Room Tone", 48000, 1, 48000 * 4, missing("Room Tone 2"));
    const aaf::MobId dialog = w.media("Interview", 48000, 1, 48000 * 4, missing("Interview"));
    const aaf::MobId left = w.media("Dialogue.L", 48000, 1, 48000 * 4, missing("Dialogue.L"));
    const aaf::MobId right = w.media("Dialogue.R", 48000, 1, 48000 * 4, missing("Dialogue.R"));
    aaf::Object& comp = w.composition("Links");
    std::uint32_t slotId = 1;
    for (const aaf::MobId& m : {toneA, toneB, dialog, dialog, left, right}) {
        aaf::Object& slot = AafWriter::slot(comp, slotId, 48000, 1, "Audio " + std::to_string(slotId), slotId);
        aaf::Object& seq = AafWriter::component(slot, "Segment", false, "Sequence", kSound, 48000 * 2);
        AafWriter::clip(seq, "Components", true, m, 0, 48000 * 2);
        ++slotId;
    }
    const fs::path file = w.write(dir / "Links.aaf");

    const ImportResult r = importOk(file, scratchOptions(dir));
    const Sequence& s = it::sequenceAt(r, 0);
    REQUIRE(s.audioTracks.size() == 6);
    // The two "Room Tone" clips are different master clips: not linked. The one master on two tracks is; so are the
    // left and right halves.
    CHECK(it::links(s) == it::Strings{"A3@0+A4@0", "A5@0+A6@0"});
}

TEST_CASE("AAF and OMF import: detection by content and extension (0.6)", "[interchange][aaf-import][omf-import]") {
    const Detection aafMagic = ImportRouter::detectData(std::string("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8), "x.bin");
    CHECK(aafMagic.format == Format::Aaf);
    CHECK(aafMagic.supported);
    CHECK(ImportRouter::detectData("", "Mix.omf").format == Format::Omf);
    CHECK(ImportRouter::detectData("", "Mix.OMFI").format == Format::Omf);
    const std::string omf = it::readFixture("export/audiopost.omf");
    CHECK(ImportRouter::detectData(omf, "noext").format == Format::Omf);
    // A large OMF without its extension: known by the Bento label at its end.
    const fs::path dir = it::scratchDir("omf-detect");
    it::writeFile(dir / "mix-from-pro-tools", omf);
    const Detection d = ImportRouter::detect(dir / "mix-from-pro-tools");
    CHECK(d.format == Format::Omf);
    CHECK(d.supported);
    CHECK(toString(Format::Omf) == "omf");
    CHECK(displayName(Format::Omf) == "OMF");
}

TEST_CASE("AAF and OMF import: malformed files fail with a clear message and never crash (0.6)", "[interchange][aaf-import][omf-import]") {
    const std::string aafBytes = it::readFixture("export/audiopost.aaf");
    const std::string omfBytes = it::readFixture("export/audiopost.omf");
    ImportOptions options; // in memory: nothing is written, no path is looked up
    options.fileExists = [](const fs::path&) { return false; };
    auto expectCleanFailure = [&](const std::string& bytes, Format format) {
        const auto r = ImportRouter::importData(bytes, format, "bad", options);
        if (r) {
            CHECK(validate(r->project, Validation::Strict).has_value());
        } else {
            CHECK((r.error().code == ErrorCode::ParseError || r.error().code == ErrorCode::Unsupported));
            CHECK_FALSE(r.error().message.empty());
        }
    };
    expectCleanFailure(aafBytes.substr(0, aafBytes.size() / 2), Format::Aaf);
    for (const std::size_t keep : {std::size_t{0}, std::size_t{100}, std::size_t{512}, std::size_t{4096}}) {
        const auto r = ImportRouter::importData(aafBytes.substr(0, keep), Format::Aaf, "cut.aaf", options);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::ParseError);
    }
    for (const std::size_t keep : {std::size_t{0}, std::size_t{23}, omfBytes.size() / 2}) {
        const auto r = ImportRouter::importData(omfBytes.substr(0, keep), Format::Omf, "cut.omf", options);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::ParseError);
    }
    // A compound file that is not an AAF (no Header), and an OMF 1.0 header.
    {
        cfb::Builder b;
        b.addStream(cfb::Builder::kRoot, "WordDocument", cfb::StreamSource::of("hello"));
        const auto bytes = b.toBytes();
        REQUIRE(bytes.has_value());
        const auto r = ImportRouter::importData(*bytes, Format::Aaf, "letter.doc", options);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().message.find("AAF") != std::string::npos);
    }
    {
        bento::Builder b;
        b.set(bento::kTocObject, b.propertyId("OMFI:OOBJ:ObjClass"), b.typeId("omfi:ClassID"), "HEAD");
        b.set(bento::kTocObject, b.propertyId("OMFI:HEAD:Version"), b.typeId("omfi:VersionType"), std::string("\x01\x00", 2));
        const auto bytes = b.toBytes();
        REQUIRE(bytes.has_value());
        const auto r = ImportRouter::importData(*bytes, Format::Omf, "old.omf", options);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::Unsupported);
        CHECK(r.error().message.find("OMF 1.0") != std::string::npos);
    }
    // Mutations of both golden files: every result is a valid project or an error.
    std::uint32_t x = 12345;
    for (int round = 0; round < 60; ++round) {
        std::string a = aafBytes;
        std::string o = omfBytes;
        for (int k = 0; k < 8; ++k) {
            x = x * 1664525u + 1013904223u;
            a[x % a.size()] = static_cast<char>(x >> 24);
            x = x * 1664525u + 1013904223u;
            o[x % o.size()] = static_cast<char>(x >> 24);
        }
        expectCleanFailure(a, Format::Aaf);
        expectCleanFailure(o, Format::Omf);
    }
}

TEST_CASE("AAF and OMF import: speaker names and audio headers (0.6)", "[interchange][aaf-import]") {
    CHECK(graph::speakerOfName("Dialog.L") == 0);
    CHECK(graph::speakerOfName("Audio 1 Right") == 1);
    CHECK(graph::speakerOfName("Mix C") == 2);
    CHECK(graph::speakerOfName("FX_LFE") == 3);
    CHECK(graph::speakerOfName("Score.Ls") == 4);
    CHECK(graph::speakerOfName("Score Rs") == 5);
    CHECK(graph::speakerOfName("Ls") == -1); // a name that is only a speaker has no base
    CHECK(graph::speakerOfName("Bass") == -1);
    CHECK(graph::withoutSpeaker("Audio 1 Left") == "Audio 1");
    CHECK(graph::withoutSpeaker("Score.LFE") == "Score");
    const std::string wav = graph::wavHeader(6, 96000, 24, 6 * 3 * 100);
    const auto h = graph::parseAudioHeader(wav);
    REQUIRE(h.has_value());
    CHECK(h->channels == 6);
    CHECK(h->sampleRate == 96000);
    CHECK(h->bits == 24);
    CHECK(h->frames == 100u);
    CHECK_FALSE(graph::parseAudioHeader("RIFF\x10\0\0\0WAVEjunk").has_value());
    CHECK(graph::textToUtf8("Caf\xE9") == "Caf\xC3\xA9");
    CHECK(graph::textToUtf8("Caf\xC3\xA9") == "Caf\xC3\xA9");
}

TEST_CASE("AAF import: files from other editors import or fail cleanly (local corpus) (0.6)", "[interchange][aaf-import][.corpus]") {
    // WABCUT_AAF_CORPUS names a folder of .aaf files from Pro Tools, Media Composer, Premiere and Resolve (not part of
    // the repository: they belong to their authors). Run with the tag [.corpus].
    const char* corpus = std::getenv("WABCUT_AAF_CORPUS");
    if (!corpus) {
        return;
    }
    const fs::path dir = it::scratchDir("aaf-corpus");
    std::size_t imported = 0;
    std::size_t failed = 0;
    for (const auto& e : fs::directory_iterator(pathFromUtf8(corpus))) {
        if (e.path().extension() != ".aaf") {
            continue;
        }
        ImportOptions options = scratchOptions(dir);
        options.embeddedMediaFolder = dir / pathFromUtf8(pathToUtf8(e.path().stem()));
        const auto r = ImportRouter::importFile(e.path(), options);
        if (!r) {
            ++failed;
            UNSCOPED_INFO(pathToUtf8(e.path().filename()) << ": " << r.error().message);
            CHECK(r.error().code == ErrorCode::ParseError);
            continue;
        }
        ++imported;
        CHECK(validate(r->project, Validation::Strict).has_value());
        std::size_t clips = 0;
        // A listing beside the scratch media, to compare with what the editor that wrote the file shows.
        std::string listing;
        for (const SequenceId id : r->sequences) {
            const Sequence& s = *r->project.findSequence(id);
            s.forEachTrack([&](const Track& t) { clips += t.clips.size(); });
            listing += "sequence '" + s.name + "' " + std::to_string(s.frameRate.num) + "/" + std::to_string(s.frameRate.den) +
                       " fps, " + std::to_string(s.audioRate.num) + " Hz, start " + std::to_string(s.startTimecode.toSeconds()) +
                       " s, master " + std::string(toString(s.masterFormat)) + "\n";
            for (std::size_t k = 0; k < s.videoTracks.size() + s.audioTracks.size(); ++k) {
                const bool video = k < s.videoTracks.size();
                const Track& t = video ? *s.videoTracks[k] : *s.audioTracks[k - s.videoTracks.size()];
                const std::string label = (video ? "V" : "A") + std::to_string(video ? k + 1 : k - s.videoTracks.size() + 1);
                listing += label + " '" + t.name + "' " + std::string(toString(t.format)) + " vol " +
                           (t.volume.isCurve() ? "curve" : std::to_string(t.volume.constant())) + " pan " +
                           (t.pan.isCurve() ? "curve" : std::to_string(t.pan.constant())) + "\n";
                for (const std::string& c : it::clips(s, label)) {
                    listing += "  " + c + "\n";
                }
                for (const Clip& c : t.clips) {
                    const ParamValue& v = c.properties()[ParamId::VolumeDb];
                    if (v.isCurve() || v.constant() != 0.0) {
                        listing += "  gain @" + std::to_string(c.start().toSeconds()) + "s " +
                                   (v.isCurve() ? std::to_string(v.curve()->keys.size()) + " keys" : std::to_string(v.constant()) + " dB") + "\n";
                    }
                    if (c.channelMap()) {
                        listing += "  channels @" + std::to_string(c.start().toSeconds()) + "s " +
                                   std::to_string(c.channelMap()->channels.size()) + "\n";
                    }
                }
                for (const Transition& tr : t.transitions) {
                    listing += "  " + tr.effect + " " + std::to_string(tr.before.toSeconds()) + "+" + std::to_string(tr.after.toSeconds()) + "\n";
                }
            }
            for (const Marker& m : s.markers) {
                listing += "marker " + std::to_string(m.start.toSeconds()) + " s '" + m.name + "' colour " + std::to_string(m.colorIndex) + "\n";
            }
        }
        for (const ReportEntry& entry : r->report.entries()) {
            listing += std::string(toString(entry.outcome)) + " " + std::string(toString(entry.category)) + ": " + entry.reason + "\n";
        }
        it::writeFile(dir / pathFromUtf8(pathToUtf8(e.path().stem()) + ".txt"), listing);
        UNSCOPED_INFO(pathToUtf8(e.path().filename()) << ": " << r->sequences.size() << " sequence(s), " << clips << " clips, "
                                                      << r->report.size() << " report entries");
    }
    CHECK(imported > 0);
    CHECK(failed == 0);
}
