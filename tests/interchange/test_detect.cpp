// ImportRouter::detect: content first, extension second, guidance for what cannot be imported.

#include "InterchangeTestSupport.h"

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::interchange;
using namespace wabcut::interchange::test;

TEST_CASE("interchange: fixtures are detected by content", "[interchange][detect]") {
    CHECK(ImportRouter::detect(fixturePath("premiere_2997df.xml")).format == Format::Fcp7Xml);
    CHECK(ImportRouter::detect(fixturePath("fcp7_23976_mac.xml")).format == Format::Fcp7Xml);
    CHECK(ImportRouter::detect(fixturePath("fcpx_1_10_23976.fcpxml")).format == Format::FcpXml);
    CHECK(ImportRouter::detect(fixturePath("resolve_timeline.otio")).format == Format::Otio);
    CHECK(ImportRouter::detect(fixturePath("promo_2997df.edl")).format == Format::Edl);
    CHECK(ImportRouter::detect(fixturePath("avid_25fps.edl")).format == Format::Edl);
    const Detection prproj = ImportRouter::detect(fixturePath("prproj/12_disabled.prproj"));
    CHECK(prproj.format == Format::PremiereProject);
    CHECK_FALSE(prproj.supported);
    CHECK(prproj.guidance.find("File > Export > Final Cut Pro XML") != std::string::npos);
    for (const auto* name :
         {"premiere_2997df.xml", "fcpx_1_10_23976.fcpxml", "resolve_timeline.otio", "promo_2997df.edl"}) {
        CHECK(ImportRouter::detect(fixturePath(name)).supported);
        CHECK(ImportRouter::detect(fixturePath(name)).guidance.empty());
    }
}

TEST_CASE("interchange: an .fcpxmld bundle is read through its Info.fcpxml", "[interchange][detect]") {
    const auto bundle = fixturePath("Wedding Teaser.fcpxmld");
    const Detection d = ImportRouter::detect(bundle);
    CHECK(d.format == Format::FcpXml);
    CHECK(d.supported);
    CHECK(d.contentPath == bundle / "Info.fcpxml");

    const auto empty = scratchDir("detect-empty-bundle") / "Empty.fcpxmld";
    std::filesystem::create_directories(empty);
    const Detection none = ImportRouter::detect(empty);
    CHECK(none.format == Format::Unknown);
    CHECK(none.guidance.find("Info.fcpxml") != std::string::npos);
}

TEST_CASE("interchange: an .otiod bundle is read through its content.otio", "[interchange][detect]") {
    const auto bundle = scratchDir("detect-otiod") / "Cut.otiod";
    writeFile(bundle / "content.otio", readFixture("resolve_timeline.otio"));
    const Detection d = ImportRouter::detect(bundle);
    CHECK(d.format == Format::Otio);
    CHECK(d.contentPath == bundle / "content.otio");
    const auto r = ImportRouter::importFile(bundle);
    REQUIRE(r);
    CHECK(r->sequences.size() == 2); // the timeline and its nested stack (C.8)
    CHECK(r->format == Format::Otio);
}

TEST_CASE("interchange: content wins over a misleading extension", "[interchange][detect]") {
    CHECK(ImportRouter::detectData(readFixture("fcpx_1_10_23976.fcpxml"), "timeline.xml").format ==
          Format::FcpXml);
    CHECK(ImportRouter::detectData(readFixture("premiere_2997df.xml"), "cut.fcpxml").format ==
          Format::Fcp7Xml);
    CHECK(ImportRouter::detectData(readFixture("resolve_timeline.otio"), "cut.json").format == Format::Otio);
    CHECK(ImportRouter::detectData(readFixture("promo_2997df.edl"), "cut.txt").format == Format::Edl);
    // With a BOM, comments and a DOCTYPE before the root.
    CHECK(ImportRouter::detectData("\xEF\xBB\xBF<?xml version=\"1.0\"?>\n<!-- x -->\n<!DOCTYPE xmeml "
                                   "[<!ENTITY a \"b\">]>\n<xmeml version=\"5\"/>",
                                   "a.xml")
              .format == Format::Fcp7Xml);
}

TEST_CASE("interchange: proprietary and unsupported formats come with guidance", "[interchange][detect]") {
    const Detection drp = ImportRouter::detectData(std::string("PK\x03\x04garbage", 11), "Project.drp");
    CHECK(drp.format == Format::ResolveProject);
    CHECK_FALSE(drp.supported);
    CHECK(drp.guidance.find("FCPXML") != std::string::npos);
    CHECK(drp.guidance.find("File > Export > Timeline") != std::string::npos);
    CHECK(ImportRouter::detectData("", "Timeline 1.drt").format == Format::ResolveProject);
    CHECK(ImportRouter::detectData("", "Archive.dra").format == Format::ResolveProject);
    CHECK(ImportRouter::detectData("", "Library.fcpbundle").format == Format::FinalCutLibrary);
    CHECK(ImportRouter::detectData("", "cut.otioz").format == Format::OtioArchive);
    CHECK(ImportRouter::detectData(std::string("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8), "x.bin").format ==
          Format::Aaf);
    CHECK(ImportRouter::detectData("", "cut.aaf").format == Format::Aaf);

    const Detection zip = ImportRouter::detectData(std::string("PK\x03\x04", 4), "archive.zip");
    CHECK(zip.format == Format::Unknown);
    CHECK_FALSE(zip.guidance.empty());
    const Detection html = ImportRouter::detectData("<html><body/></html>", "page.xml");
    CHECK(html.format == Format::Unknown);
    CHECK(html.guidance.find("<html>") != std::string::npos);
    CHECK(ImportRouter::detectData("{\"a\": 1}", "cut.otio").format == Format::Unknown);
    CHECK(ImportRouter::detectData("hello", "notes.txt").format == Format::Unknown);
    CHECK(ImportRouter::detectData("not gzip", "fake.prproj").format == Format::Unknown);
}

TEST_CASE("interchange: importFile reports why a file cannot be imported", "[interchange][detect]") {
    const auto missing = ImportRouter::importFile(fixturePath("does-not-exist.xml"));
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound);

    const auto dir = scratchDir("detect-errors");
    writeFile(dir / "Project.drp", std::string("PK\x03\x04", 4));
    const auto drp = ImportRouter::importFile(dir / "Project.drp");
    REQUIRE_FALSE(drp);
    CHECK(drp.error().code == ErrorCode::Unsupported);

    const auto prproj = ImportRouter::importFile(fixturePath("prproj/12_disabled.prproj"));
    REQUIRE_FALSE(prproj);
    CHECK(prproj.error().code == ErrorCode::Unsupported);

    writeFile(dir / "notes.txt", "just some notes");
    const auto text = ImportRouter::importFile(dir / "notes.txt");
    REQUIRE_FALSE(text);
    CHECK(text.error().code == ErrorCode::ParseError);
}

TEST_CASE("interchange: format names are stable", "[interchange][detect]") {
    CHECK(toString(Format::Fcp7Xml) == "fcp7-xml");
    CHECK(toString(Format::FcpXml) == "fcpxml");
    CHECK(toString(Format::Otio) == "otio");
    CHECK(toString(Format::Edl) == "edl");
    CHECK(toString(Format::PremiereProject) == "prproj");
    CHECK(toString(Format::ResolveProject) == "resolve-project");
    CHECK(displayName(Format::Fcp7Xml) == "Final Cut Pro 7 XML");
    CHECK(toString(ReportOutcome::Approximated) == "approximated");
    CHECK(toString(ReportCategory::SequenceSetting) == "sequence-setting");
    CHECK(toString(ReportCategory::AudioMapping) == "audio-mapping");
}

TEST_CASE("interchange: OTIO and EDL files saved with a UTF-8 byte order mark are read like the same text without one",
          "[interchange][detect]") {
    const std::string bom = "\xEF\xBB\xBF";

    // OTIO (Notepad, Python's utf-8-sig): detected and imported, same sequences as the plain file.
    const std::string otio = readFixture("resolve_timeline.otio");
    CHECK(ImportRouter::detectData(bom + otio, "cut.otio").format == Format::Otio);
    CHECK(ImportRouter::detectData(bom + otio, "cut.otio").supported);
    const ImportResult plain = importText(otio, Format::Otio);
    const ImportResult marked = importText(bom + otio, Format::Otio);
    REQUIRE(marked.sequences.size() == plain.sequences.size());
    CHECK(sequenceAt(marked, 0).name == sequenceAt(plain, 0).name);
    CHECK(clips(sequenceAt(marked, 0), "V1") == clips(sequenceAt(plain, 0), "V1"));

    // The same file on disk, through File > Import's route (detect, then importFile).
    const auto dir = scratchDir("detect-bom");
    writeFile(dir / "cut.otio", bom + otio);
    CHECK(ImportRouter::detect(dir / "cut.otio").format == Format::Otio);
    const auto fromFile = ImportRouter::importFile(dir / "cut.otio");
    REQUIRE(fromFile);
    CHECK(sequenceAt(*fromFile, 0).name == sequenceAt(plain, 0).name);

    // A real non-OTIO file after a BOM is still refused, with the same guidance.
    const Detection notOtio = ImportRouter::detectData(bom + "{\"a\": 1}", "x.otio");
    CHECK(notOtio.format == Format::Unknown);
    CHECK(notOtio.guidance.find("OTIO_SCHEMA") != std::string::npos);

    // EDL: the title on the first line survives and nothing is reported as ignored.
    const std::string edl = readFixture("promo_2997df.edl");
    CHECK(ImportRouter::detectData(bom + edl, "promo.edl").format == Format::Edl);
    const ImportResult edlMarked = importText(bom + edl, Format::Edl);
    const ImportResult edlPlain = importText(edl, Format::Edl);
    CHECK(sequenceAt(edlMarked, 0).name == "Promo_30s_v2");
    CHECK(clips(sequenceAt(edlMarked, 0), "V1") == clips(sequenceAt(edlPlain, 0), "V1"));
    // The report is the same as for the plain file (the fixture has lines of its own that are ignored): the BOM adds none.
    CHECK(reasons(edlMarked.report, ReportOutcome::Dropped, ReportCategory::Effect) ==
          reasons(edlPlain.report, ReportOutcome::Dropped, ReportCategory::Effect));
    // A small EDL with nothing to ignore: the title survives the BOM and the report says nothing was ignored.
    const std::string small =
        "TITLE: My Cut\nFCM: NON-DROP FRAME\n"
        "001  X        V     C        00:00:00:00 00:00:01:00 00:00:00:00 00:00:01:00\n";
    const ImportResult smallMarked = importText(bom + small, Format::Edl);
    CHECK(sequenceAt(smallMarked, 0).name == "My Cut");
    CHECK_FALSE(hasEntry(smallMarked.report, ReportOutcome::Dropped, ReportCategory::Effect, "Ignored, not CMX3600 events or comments"));
    CHECK(sequenceAt(importText(small, Format::Edl), 0).name == "My Cut");
    writeFile(dir / "promo.edl", bom + edl);
    const auto edlFile = ImportRouter::importFile(dir / "promo.edl");
    REQUIRE(edlFile);
    CHECK(sequenceAt(*edlFile, 0).name == "Promo_30s_v2");

    // Binary formats are not text: their bytes are never trimmed (an AAF is still found by its magic).
    CHECK(ImportRouter::detectData(std::string("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8), "x.bin").format == Format::Aaf);
}
