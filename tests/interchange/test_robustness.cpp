// Malformed input never crashes: truncated and mutated fixtures, garbage, hostile nesting and huge values.
// Every import either fails with an Error or yields a project that passes validate(Strict).

#include "InterchangeTestSupport.h"

#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <random>

using namespace wabcut;
using namespace wabcut::interchange;
using namespace wabcut::interchange::test;

namespace {

struct Sample {
    const char* fixture;
    Format format;
};

constexpr Sample kSamples[] = {
    {"premiere_2997df.xml", Format::Fcp7Xml},
    {"fcp7_23976_mac.xml", Format::Fcp7Xml},
    {"fcpx_1_10_23976.fcpxml", Format::FcpXml},
    {"Wedding Teaser.fcpxmld/Info.fcpxml", Format::FcpXml},
    {"resolve_timeline.otio", Format::Otio},
    {"promo_2997df.edl", Format::Edl},
    {"avid_25fps.edl", Format::Edl},
    {"prproj/07_transitions.prproj", Format::PremiereProject},
};

ImportOptions experimental() {
    ImportOptions options;
    options.experimentalPremiereProject = true;
    return options;
}

/// Imports and checks the contract: an error, or a valid project.
void survives(std::string_view data, Format format) {
    const auto result = ImportRouter::importData(data, format, "fuzz", experimental());
    if (result) {
        const auto valid = validate(result->project, Validation::Strict);
        if (!valid) {
            FAIL("invalid project from mutated input: " << valid.error().message);
        }
    } else {
        CHECK(result.error().code != ErrorCode::Unknown); // Unknown would mean an internal inconsistency
    }
}

} // namespace

TEST_CASE("interchange: robustness: truncated fixtures", "[interchange][robustness]") {
    for (const Sample& sample : kSamples) {
        CAPTURE(sample.fixture);
        const std::string data = readFixture(sample.fixture);
        const std::size_t step = std::max<std::size_t>(1, data.size() / 97);
        for (std::size_t cut = 0; cut < data.size(); cut += step) {
            survives(std::string_view(data).substr(0, cut), sample.format);
        }
    }
}

TEST_CASE("interchange: robustness: mutated fixtures", "[interchange][robustness]") {
    std::mt19937 rng(20261011); // fixed seed: failures reproduce
    for (const Sample& sample : kSamples) {
        CAPTURE(sample.fixture);
        const std::string original = readFixture(sample.fixture);
        for (int round = 0; round < 60; ++round) {
            std::string data = original;
            std::uniform_int_distribution<std::size_t> position(0, data.size() - 1);
            const int edits = 1 + static_cast<int>(rng() % 8);
            for (int e = 0; e < edits; ++e) {
                const std::size_t at = position(rng);
                switch (rng() % 5) {
                case 0: // random byte
                    data[at] = static_cast<char>(rng() & 0xFF);
                    break;
                case 1: // digit swap, to bend numbers rather than syntax
                    if (data[at] >= '0' && data[at] <= '9') {
                        data[at] = static_cast<char>('0' + rng() % 10);
                    }
                    break;
                case 2: // a minus sign
                    data[at] = '-';
                    break;
                case 3: // delete a chunk
                    data.erase(at, std::min<std::size_t>(data.size() - at, rng() % 64));
                    break;
                default: // huge number
                    data.insert(at, "99999999999999999999");
                    break;
                }
                if (data.empty()) {
                    break;
                }
                position = std::uniform_int_distribution<std::size_t>(0, data.size() - 1);
            }
            survives(data, sample.format);
        }
    }
}

TEST_CASE("interchange: robustness: garbage of every shape", "[interchange][robustness]") {
    std::mt19937 rng(7);
    const Format formats[] = {Format::Fcp7Xml, Format::FcpXml, Format::Otio, Format::Edl,
                              Format::PremiereProject};
    for (const Format format : formats) {
        CAPTURE(toString(format));
        survives("", format);
        survives(std::string(1, '\0'), format);
        survives("\xEF\xBB\xBF", format);
        for (int i = 0; i < 20; ++i) {
            std::string noise(static_cast<std::size_t>(rng() % 4096), '\0');
            for (char& c : noise) {
                c = static_cast<char>(rng() & 0xFF);
            }
            survives(noise, format);
        }
        // Gzip magic followed by junk.
        survives(std::string("\x1F\x8B\x08\x00garbage", 11), format);
        // Detection must not crash on any of it either.
        (void)ImportRouter::detectData(std::string(1000, '\xFF'), "x.xml");
    }
}

TEST_CASE("interchange: robustness: hostile nesting depth", "[interchange][robustness]") {
    constexpr int kDepth = 100'000;
    std::string xml;
    xml.reserve(kDepth * 9);
    for (int i = 0; i < kDepth; ++i) {
        xml += "<a>";
    }
    survives("<xmeml version=\"5\">" + xml, Format::Fcp7Xml);
    survives("<fcpxml version=\"1.10\"><library><event><project name=\"p\"><sequence><spine>" + xml,
             Format::FcpXml);
    std::string json(kDepth, '[');
    survives(json, Format::Otio);
    std::string objects = "{\"OTIO_SCHEMA\":\"Timeline.1\",\"tracks\":";
    for (int i = 0; i < 2'000; ++i) {
        objects += "{\"OTIO_SCHEMA\":\"Stack.1\",\"children\":[";
    }
    survives(objects, Format::Otio);

    // Deep but well-formed FCPXML storylines are cut off at the nesting limit.
    std::string spines;
    for (int i = 0; i < 200; ++i) {
        spines += "<spine lane=\"1\" offset=\"0s\">";
    }
    for (int i = 0; i < 200; ++i) {
        spines += "</spine>";
    }
    const std::string doc =
        R"(<fcpxml version="1.10"><resources><format id="r1" frameDuration="1/25s"/></resources>
        <project name="p"><sequence format="r1"><spine><gap offset="0s" duration="1s">)" +
        spines + "</gap></spine></sequence></project></fcpxml>";
    survives(doc, Format::FcpXml);
}

TEST_CASE("interchange: robustness: extreme values are reported, not wrapped", "[interchange][robustness]") {
    const std::string fcp7 =
        R"(<xmeml version="4"><sequence><name>x</name><rate><timebase>25</timebase></rate><media><video><track>
        <clipitem id="a"><name>a</name><start>0</start><end>9223372036854775807</end><in>0</in><out>10</out><file id="f"><pathurl>file:///a.mov</pathurl></file></clipitem>
        <clipitem id="b"><name>b</name><start>-50</start><end>10</end><in>0</in><out>60</out><file id="g"><pathurl>file:///b.mov</pathurl></file></clipitem>
        <clipitem id="c"><name>c</name><start>20</start><end>30</end><in>-9223372036854775808</in><out>10</out><file id="h"><pathurl>file:///c.mov</pathurl></file></clipitem>
        </track></video></media></sequence></xmeml>)";
    const auto r = importText(fcp7, Format::Fcp7Xml);
    CHECK(r.report.count(ReportOutcome::Dropped, ReportCategory::Clip) >= 1);

    const std::string rates =
        R"(<xmeml version="4"><sequence><name>x</name><rate><timebase>0</timebase></rate><media><video><format>
        <samplecharacteristics><width>-5</width><height>99999999</height></samplecharacteristics></format><track/></video></media></sequence></xmeml>)";
    const auto r2 = importText(rates, Format::Fcp7Xml);
    const Sequence& seq = sequenceAt(r2, 0);
    CHECK(seq.frameRate == kRate30);
    CHECK(seq.width == 1920);
    CHECK(hasEntry(r2.report, ReportOutcome::Approximated, ReportCategory::SequenceSetting, "is not usable"));

    survives(
        R"({"OTIO_SCHEMA":"Timeline.1","name":"x","tracks":{"OTIO_SCHEMA":"Stack.1","children":[{"OTIO_SCHEMA":"Track.1","kind":"Video","children":[
        {"OTIO_SCHEMA":"Clip.1","name":"c","source_range":{"OTIO_SCHEMA":"TimeRange.1","start_time":{"value":1e308,"rate":24},"duration":{"value":5,"rate":1e-300}},"media_reference":null},
        {"OTIO_SCHEMA":"Gap.1","source_range":{"OTIO_SCHEMA":"TimeRange.1","start_time":{"value":0,"rate":24},"duration":{"value":9e15,"rate":24}}},
        {"OTIO_SCHEMA":"Gap.1","source_range":{"OTIO_SCHEMA":"TimeRange.1","start_time":{"value":0,"rate":24},"duration":{"value":9e15,"rate":24}}}]}]}})",
        Format::Otio);
    survives("001  X  V  C  99:59:59:29 99:59:59:29 99:59:59:28 99:59:59:29\n", Format::Edl);
}

TEST_CASE("interchange: robustness: a large timeline imports in reasonable time",
          "[interchange][robustness]") {
    // 3,000 clips on two tracks, every video clip linked to its audio.
    std::string items;
    std::string audio;
    for (int i = 0; i < 1500; ++i) {
        const std::string n = std::to_string(i);
        const std::string start = std::to_string(i * 10);
        const std::string end = std::to_string(i * 10 + 10);
        const std::string file =
            "<file id=\"f" + std::to_string(i % 50) + "\"><name>m" + std::to_string(i % 50) +
            ".mov</name><pathurl>file:///m/m" + std::to_string(i % 50) +
            ".mov</pathurl><media><video/><audio><channelcount>2</channelcount></audio></media></file>";
        items += "<clipitem id=\"v" + n + "\"><name>c" + n + "</name><start>" + start + "</start><end>" +
                 end + "</end><in>0</in><out>10</out>" + file + "<link><linkclipref>v" + n +
                 "</linkclipref></link><link><linkclipref>a" + n + "</linkclipref></link></clipitem>";
        audio += "<clipitem id=\"a" + n + "\"><name>c" + n + "</name><start>" + start + "</start><end>" +
                 end + "</end><in>0</in><out>10</out><file id=\"f" + std::to_string(i % 50) +
                 "\"/><sourcetrack><mediatype>audio</mediatype><trackindex>1</trackindex></sourcetrack></"
                 "clipitem>";
    }
    const std::string xml =
        "<xmeml "
        "version=\"4\"><sequence><name>big</name><rate><timebase>25</timebase></rate><media><video><track>" +
        items + "</track></video><audio><track>" + audio + "</track></audio></media></sequence></xmeml>";
    const auto r = importText(xml, Format::Fcp7Xml);
    const Sequence& seq = sequenceAt(r, 0);
    CHECK(track(seq, "V1").clips.size() == 1500);
    CHECK(track(seq, "A1").clips.size() == 1500);
    CHECK(links(seq).size() == 1500);
}

TEST_CASE("interchange: robustness: the malformed fixtures fail cleanly, with where",
          "[interchange][robustness]") {
    // Detected by their start, rejected by the parser: an export cut short, a JSON syntax error, a text file.
    CHECK(ImportRouter::detect(fixturePath("malformed_truncated.xml")).format == Format::Fcp7Xml);
    const auto xml = ImportRouter::importFile(fixturePath("malformed_truncated.xml"));
    REQUIRE_FALSE(xml);
    CHECK(xml.error().code == ErrorCode::ParseError);
    CHECK(xml.error().message.find("line ") != std::string::npos);

    CHECK(ImportRouter::detect(fixturePath("malformed.otio")).format == Format::Otio);
    const auto otio = ImportRouter::importFile(fixturePath("malformed.otio"));
    REQUIRE_FALSE(otio);
    CHECK(otio.error().code == ErrorCode::ParseError);
    CHECK(otio.error().message.find("byte") != std::string::npos);

    CHECK(ImportRouter::detect(fixturePath("malformed.edl")).format == Format::Edl);
    const auto edl = ImportRouter::importFile(fixturePath("malformed.edl"));
    REQUIRE_FALSE(edl);
    CHECK(edl.error().code == ErrorCode::ParseError);
}

TEST_CASE("interchange: robustness: text that is not UTF-8 is read as Windows-1252, and the project saves (C.10)",
          "[interchange][robustness]") {
    // An Avid EDL written in Windows-1252: an e acute and curly quotes. Before C.10 the bytes went into the names
    // unchanged, and the imported project could not be saved (a fuzz finding).
    const std::string edl = "TITLE: Caf\xE9 \x93Promo\x94\n"
                            "FCM: NON-DROP FRAME\n\n"
                            "0001  R012T03  V     C        10:00:00:00 10:00:04:00 00:00:00:00 00:00:04:00\n"
                            "* FROM CLIP NAME: Ol\xE9 wide\n";
    const ImportResult r = importText(edl, Format::Edl, {}, "promo.edl");
    CHECK(sequenceAt(r, 0).name == "Caf\xC3\xA9 \xE2\x80\x9CPromo\xE2\x80\x9D");
    CHECK(io::toJson(r.project).has_value());
    // The same in XML without an encoding declaration, and a numeric reference to a lone surrogate.
    const std::string xml = "<?xml version=\"1.0\"?><xmeml version=\"5\"><sequence><name>Caf\xE9 &#xD800;</name>"
                            "<rate><timebase>25</timebase><ntsc>FALSE</ntsc></rate><media/></sequence></xmeml>";
    const auto fromXml = ImportRouter::importData(xml, Format::Fcp7Xml, "cut.xml");
    if (fromXml) {
        CHECK(io::toJson(fromXml->project).has_value());
        CHECK(sequenceAt(*fromXml, 0).name.starts_with("Caf\xC3\xA9 "));
    }
}
