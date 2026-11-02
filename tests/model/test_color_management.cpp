// I.9: colour management in the model (ColorManagement.h): the project's working space and output, a video clip's input
// colour space - names, bounds, edits, change detection, three-way merge, and project files (absent until set: other
// projects stay byte-identical; a file that sets them needs the new schema).

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/ColorManagement.h>
#include <wabcut/model/ColorManagementOps.h>
#include <wabcut/model/Merge.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaa|
    A1 tgt |Aaaaaaaa|
)";

std::string jsonOf(const ProjectState& state) {
    auto json = io::toJson(state);
    REQUIRE(json);
    return *json;
}

ProjectState roundTrip(const ProjectState& state) {
    auto back = io::fromJson(jsonOf(state));
    REQUIRE(back);
    return *back;
}

} // namespace

TEST_CASE("Colour management names, display names, menu order and bounds", "[model][colormanagement]") {
    REQUIRE(allColorManagementModes().size() == 7); // 0.6: Wide Gamut (Minimal Tone Mapping)
    for (const ColorManagementMode m : allColorManagementModes()) {
        CHECK(colorManagementModeFromString(toString(m)) == m);
        CHECK_FALSE(displayName(m).empty());
        CHECK_FALSE(description(m).empty());
    }
    CHECK(toString(ColorManagementMode::Aces13) == "aces");
    CHECK(toString(ColorManagementMode::HdrPq) == "hdr-pq");
    CHECK_FALSE(colorManagementModeFromString("ACES").has_value()); // ids are exact
    CHECK(allInputColorSpaces().size() == 27);
    for (const InputColorSpace s : allInputColorSpaces()) {
        CHECK(inputColorSpaceFromString(toString(s)) == s);
        CHECK_FALSE(displayName(s).empty());
    }
    CHECK(toString(InputColorSpace::SLog3SGamut3Cine) == "s-log3-cine");
    CHECK_FALSE(inputColorSpaceFromString("auto").has_value()); // Auto is "no override", not a space

    ColorManagement m;
    CHECK(m.isDefault());
    CHECK_FALSE(m.isHdr());
    CHECK_FALSE(colorManagementProblem(m).has_value());
    m.peakNits = 50.0;
    CHECK(colorManagementProblem(m).has_value());
    m.peakNits = 1000.0;
    m.referenceWhiteNits = 1000.0;
    CHECK(colorManagementProblem(m).has_value()); // the reference white must be below the peak
    m.referenceWhiteNits = 203.0;
    m.peakNits = std::numeric_limits<double>::quiet_NaN();
    CHECK(colorManagementProblem(m).has_value());
}

TEST_CASE("Colour setups are Premiere's: Direct Rec.709 first and the default, then HDR, Wide Gamut, off (0.2)",
          "[model][colormanagement]") {
    // Premiere Pro's Sequence Settings > Color > Color Setup menu, in its order.
    std::vector<std::string_view> names;
    for (const ColorManagementMode m : allColorManagementModes()) {
        names.push_back(displayName(m));
    }
    CHECK(names == std::vector<std::string_view>{"Direct Rec.709 (SDR)", "Direct HLG (HDR)", "Direct PQ (HDR)",
                                                 "Wide Gamut (Tone Mapped)", "Wide Gamut (Minimal Tone Mapping)",
                                                 "Wide Gamut (No Tone Mapping)", "Disable Color Management"});
    CHECK(ColorManagement{}.mode == ColorManagementMode::DisplayRec709);
    CHECK(toString(ColorManagementMode::WideGamut) == "wide-gamut");
    CHECK(toString(ColorManagementMode::Disabled) == "off");
    // Managed working spaces: HDR and Wide Gamut; Direct Rec.709 and "off" keep the display-referred pipeline.
    for (const ColorManagementMode m : allColorManagementModes()) {
        ColorManagement cm;
        cm.mode = m;
        const bool managed = m == ColorManagementMode::HdrPq || m == ColorManagementMode::HdrHlg ||
                             m == ColorManagementMode::Aces13 || m == ColorManagementMode::WideGamut ||
                             m == ColorManagementMode::WideGamutMinimal;
        CHECK(cm.managed() == managed);
    }
    CHECK(isCameraLog(InputColorSpace::SLog3SGamut3Cine));
    CHECK(isCameraLog(InputColorSpace::RedLog3G10));
    CHECK(isCameraLog(InputColorSpace::AcesCct));
    CHECK_FALSE(isCameraLog(InputColorSpace::Rec2100Pq));
    CHECK_FALSE(isCameraLog(InputColorSpace::AcesCg));
}

TEST_CASE("More camera log inputs have ids and names and are camera logs (0.6)", "[model][colormanagement]") {
    struct Expected {
        InputColorSpace space;
        std::string_view id;
        std::string_view name;
    };
    const Expected added[] = {
        {InputColorSpace::DLogDGamut, "d-log", "DJI D-Log / D-Gamut"},
        {InputColorSpace::FLogFGamut, "f-log", "Fujifilm F-Log / F-Gamut"},
        {InputColorSpace::FLog2FGamut, "f-log2", "Fujifilm F-Log2 / F-Gamut"},
        {InputColorSpace::NLogNGamut, "n-log", "Nikon N-Log / N-Gamut"},
        {InputColorSpace::SLog2SGamut, "s-log2", "Sony S-Log2 / S-Gamut"},
        {InputColorSpace::SLog2SGamut3Cine, "s-log2-cine", "Sony S-Log2 / S-Gamut3.Cine"},
        {InputColorSpace::CanonLogCinemaGamut, "canon-log", "Canon Log / Cinema Gamut"},
        {InputColorSpace::LLogRec2020, "l-log", "Leica L-Log / Rec.2020"},
        {InputColorSpace::RedLogFilm, "red-log-film", "RED Log Film / REDWideGamutRGB"},
    };
    const auto menu = allInputColorSpaces();
    for (const Expected& e : added) {
        INFO(e.id);
        CHECK(toString(e.space) == e.id);
        CHECK(inputColorSpaceFromString(e.id) == e.space);
        CHECK(displayName(e.space) == e.name);
        CHECK(isCameraLog(e.space));
        CHECK(std::find(menu.begin(), menu.end(), e.space) != menu.end());
        // Auto-detection reads it from a media's metadata.
        VideoStreamInfo stream;
        stream.pixelFormat = "yuv420p10le";
        stream.logColorSpace = std::string(e.id);
        CHECK(detectedLogSpace(stream) == e.space);
        CHECK(autoInputColorSpace(stream) == e.space);
    }
    // Appended after the 0.5 spaces, so the stored values of older projects keep their meaning.
    CHECK(static_cast<int>(InputColorSpace::DLogDGamut) == static_cast<int>(InputColorSpace::AcesCct) + 1);
    CHECK_FALSE(inputColorSpaceFromString("d-log-m").has_value()); // DJI publishes no D-Log M equation: not a space
    CHECK_FALSE(inputColorSpaceFromString("D-Log").has_value());   // ids are exact
}

TEST_CASE("Input colour space ids of older projects keep their meaning (0.6)", "[model][colormanagement][io]") {
    // Every 0.5 id, its enum value and its display name, as the 0.5 files and commands used them.
    struct Old {
        std::string_view id;
        InputColorSpace space;
        int value;
        std::string_view name;
    };
    const Old old[] = {
        {"rec709", InputColorSpace::Rec709, 0, "Rec.709"},
        {"srgb", InputColorSpace::Srgb, 1, "sRGB"},
        {"linear-rec709", InputColorSpace::LinearRec709, 2, "Linear Rec.709"},
        {"rec2020", InputColorSpace::Rec2020, 3, "Rec.2020 (SDR)"},
        {"rec2100-pq", InputColorSpace::Rec2100Pq, 4, "Rec.2100 PQ"},
        {"rec2100-hlg", InputColorSpace::Rec2100Hlg, 5, "Rec.2100 HLG"},
        {"s-log3", InputColorSpace::SLog3SGamut3, 6, "Sony S-Log3 / S-Gamut3"},
        {"s-log3-cine", InputColorSpace::SLog3SGamut3Cine, 7, "Sony S-Log3 / S-Gamut3.Cine"},
        {"v-log", InputColorSpace::VLogVGamut, 8, "Panasonic V-Log / V-Gamut"},
        {"logc3", InputColorSpace::LogC3, 9, "ARRI LogC3 (EI 800) / AWG3"},
        {"logc4", InputColorSpace::LogC4, 10, "ARRI LogC4 / AWG4"},
        {"canon-log3", InputColorSpace::CanonLog3, 11, "Canon Log 3 / Cinema Gamut"},
        {"apple-log", InputColorSpace::AppleLog, 12, "Apple Log"},
        {"acescg", InputColorSpace::AcesCg, 13, "ACEScg"},
        {"aces2065-1", InputColorSpace::Aces2065, 14, "ACES2065-1"},
        {"canon-log2", InputColorSpace::CanonLog2, 15, "Canon Log 2 / Cinema Gamut"},
        {"red-log3g10", InputColorSpace::RedLog3G10, 16, "RED Log3G10 / REDWideGamutRGB"},
        {"acescct", InputColorSpace::AcesCct, 17, "ACEScct"},
    };
    for (const Old& o : old) {
        INFO(o.id);
        CHECK(inputColorSpaceFromString(o.id) == o.space);
        CHECK(static_cast<int>(o.space) == o.value);
        CHECK(toString(o.space) == o.id);
        CHECK(displayName(o.space) == o.name);
    }

    // A clip saved with an old id loads as before; one with a new id is written by id and read back.
    Timeline tl(kPicture);
    const ClipId one[] = {tl.clip("V1@0")};
    auto old2 = ok(edit::setClipInputColorSpace(tl.state, tl.seq, one, InputColorSpace::CanonLog2));
    const std::string json = jsonOf(old2.state);
    CHECK(json.find("\"inputColorSpace\": \"canon-log2\"") != std::string::npos);
    CHECK(roundTrip(old2.state).findSequence(tl.seq)->findClip(one[0])->clip().inputColorSpace() == InputColorSpace::CanonLog2);
    auto fuji = ok(edit::setClipInputColorSpace(tl.state, tl.seq, one, InputColorSpace::FLog2FGamut));
    CHECK(jsonOf(fuji.state).find("\"inputColorSpace\": \"f-log2\"") != std::string::npos);
    const ProjectState back = roundTrip(fuji.state);
    CHECK(back == fuji.state);
    CHECK(back.findSequence(tl.seq)->findClip(one[0])->clip().inputColorSpace() == InputColorSpace::FLog2FGamut);
}

TEST_CASE("Auto reads the camera log the media's metadata names when log is auto-detected (0.2)", "[model][colormanagement]") {
    VideoStreamInfo stream;
    stream.pixelFormat = "yuv422p10le";
    CHECK_FALSE(detectedLogSpace(stream).has_value());
    CHECK(autoInputColorSpace(stream) == InputColorSpace::Rec709);
    stream.logColorSpace = "s-log3-cine";
    CHECK(detectedLogSpace(stream) == InputColorSpace::SLog3SGamut3Cine);
    CHECK(autoInputColorSpace(stream) == InputColorSpace::SLog3SGamut3Cine);
    CHECK(autoInputColorSpace(stream, false) == InputColorSpace::Rec709); // auto-detect off: the tags alone
    stream.logColorSpace = "rec2100-pq"; // not a camera log: never "detected"
    CHECK_FALSE(detectedLogSpace(stream).has_value());
    stream.logColorSpace = "nonsense";
    CHECK_FALSE(detectedLogSpace(stream).has_value());
}

TEST_CASE("Project files: auto-detect off and a media's detected log are written only when set (0.2)",
          "[model][colormanagement][io]") {
    Timeline tl(kPicture);
    CHECK(jsonOf(tl.state).find("autoDetectLog") == std::string::npos);
    CHECK(jsonOf(tl.state).find("logColorSpace") == std::string::npos);

    ProjectState s = tl.state;
    ColorManagement off;
    off.mode = ColorManagementMode::WideGamut;
    off.autoDetectLog = false;
    s = *edit::setColorManagement(s, off);
    MediaItem item = *s.media.front();
    REQUIRE_FALSE(item.info.video.empty());
    item.info.video.front().logColorSpace = "logc4";
    s.media.front() = std::make_shared<const MediaItem>(std::move(item));
    const std::string json = jsonOf(s);
    CHECK(json.find("\"autoDetectLog\": false") != std::string::npos);
    CHECK(json.find("\"logColorSpace\": \"logc4\"") != std::string::npos);
    CHECK(json.find("\"wide-gamut\"") != std::string::npos);
    const ProjectState back = roundTrip(s);
    CHECK(back == s);
    CHECK_FALSE(back.colorManagement.autoDetectLog);
    CHECK(back.media.front()->info.video.front().logColorSpace == "logc4");

    // A detected log must be a camera log id: anything else fails to load.
    std::string bad = json;
    bad.replace(bad.find("\"logc4\""), 7, "\"srgb\"");
    CHECK_FALSE(io::fromJson(bad).has_value());
}

TEST_CASE("Auto input colour space follows the stream's colour tags", "[model][colormanagement]") {
    ColorInfo c;
    CHECK(autoInputColorSpace(c, false) == InputColorSpace::Rec709);
    CHECK(autoInputColorSpace(c, true) == InputColorSpace::Srgb); // an untagged RGB picture (a still)
    c.transfer = TransferFunction::PQ;
    c.primaries = ColorPrimaries::BT2020;
    CHECK(autoInputColorSpace(c, false) == InputColorSpace::Rec2100Pq);
    c.transfer = TransferFunction::HLG;
    CHECK(autoInputColorSpace(c, false) == InputColorSpace::Rec2100Hlg);
    c.transfer = TransferFunction::BT709;
    CHECK(autoInputColorSpace(c, false) == InputColorSpace::Rec2020);
    c.transfer = TransferFunction::Linear;
    CHECK(autoInputColorSpace(c, false) == InputColorSpace::LinearRec709);
    VideoStreamInfo stream;
    stream.pixelFormat = "rgba";
    CHECK(autoInputColorSpace(stream) == InputColorSpace::Srgb);
    stream.pixelFormat = "yuv420p10le";
    CHECK(autoInputColorSpace(stream) == InputColorSpace::Rec709);
    CHECK(isRgbPixelFormat("gbrp10le"));
    CHECK_FALSE(isRgbPixelFormat("nv12"));
}

TEST_CASE("Colour management edits: the project setting and a clip's input colour space", "[model][colormanagement]") {
    Timeline tl(kPicture);
    ColorManagement aces;
    aces.mode = ColorManagementMode::Aces13;
    auto set = edit::setColorManagement(tl.state, aces);
    REQUIRE(set);
    CHECK(set->colorManagement == aces);
    requireValid(*set);
    const ChangeSet change = diff(tl.state, *set);
    CHECK(change.colorManagementChanged);
    CHECK_FALSE(change.empty());
    CHECK(change.sequences.empty());

    ColorManagement bad = aces;
    bad.peakNits = 20000.0;
    CHECK_FALSE(edit::setColorManagement(tl.state, bad).has_value());

    // A clip's input colour space: one content change, only on the video clip.
    const ClipId v = tl.clip("V1@0");
    const ClipId a = tl.clip("A1@0");
    const ClipId one[] = {v};
    auto logged = ok(edit::setClipInputColorSpace(*set, tl.seq, one, InputColorSpace::SLog3SGamut3Cine));
    const Clip& clip = logged.state.findSequence(tl.seq)->findClip(v)->clip();
    CHECK(clip.inputColorSpace() == InputColorSpace::SLog3SGamut3Cine);
    const ChangeSet clipChange = diff(*set, logged.state);
    REQUIRE(clipChange.sequences.size() == 1);
    CHECK(clipChange.sequences[0].roles.has(PropertyRole::Effects)); // the picture changes, not the decoded frames
    CHECK_FALSE(clipChange.sequences[0].roles.has(PropertyRole::Source));
    const ClipId audio[] = {a};
    CHECK_FALSE(edit::setClipInputColorSpace(*set, tl.seq, audio, InputColorSpace::VLogVGamut).has_value());
    // Back to Auto.
    auto automatic = ok(edit::setClipInputColorSpace(logged.state, tl.seq, one, std::nullopt));
    CHECK_FALSE(automatic.state.findSequence(tl.seq)->findClip(v)->clip().inputColorSpace().has_value());
}

TEST_CASE("Project files: colour management and input colour spaces are written only when set", "[model][colormanagement][io]") {
    Timeline tl(kPicture);
    const std::string plain = jsonOf(tl.state);
    CHECK(plain.find("colorManagement") == std::string::npos);
    CHECK(plain.find("inputColorSpace") == std::string::npos);
    CHECK(plain.find("\"schemaVersion\": " + std::to_string(io::kSchemaVersion)) != std::string::npos);
    CHECK(roundTrip(tl.state) == tl.state);

    ColorManagement hdr;
    hdr.mode = ColorManagementMode::HdrPq;
    hdr.peakNits = 4000.0;
    hdr.referenceWhiteNits = 100.0;
    auto s = edit::setColorManagement(tl.state, hdr);
    REQUIRE(s);
    const ClipId one[] = {tl.clip("V1@0")};
    auto r = ok(edit::setClipInputColorSpace(*s, tl.seq, one, InputColorSpace::AppleLog));
    const std::string json = jsonOf(r.state);
    CHECK(json.find("\"colorManagement\"") != std::string::npos);
    CHECK(json.find("\"hdr-pq\"") != std::string::npos);
    CHECK(json.find("\"inputColorSpace\": \"apple-log\"") != std::string::npos);
    const ProjectState back = roundTrip(r.state);
    CHECK(back == r.state);
    CHECK(back.colorManagement == hdr);

    // Unknown names and out-of-range values are refused, not half-loaded.
    std::string unknownMode = json;
    unknownMode.replace(unknownMode.find("\"hdr-pq\""), 8, "\"hdr-xx\"");
    CHECK_FALSE(io::fromJson(unknownMode).has_value());
    std::string unknownSpace = json;
    unknownSpace.replace(unknownSpace.find("\"apple-log\""), 11, "\"red-log3g\"");
    CHECK_FALSE(io::fromJson(unknownSpace).has_value());
    std::string tooBright = json;
    tooBright.replace(tooBright.find("4000"), 4, "90000");
    CHECK_FALSE(io::fromJson(tooBright).has_value());
}

TEST_CASE("A file of the previous schema loads with display-referred Rec.709 and Auto, unchanged", "[model][colormanagement][io]") {
    Timeline tl(kPicture);
    std::string json = jsonOf(tl.state);
    const std::string current = "\"schemaVersion\": " + std::to_string(io::kSchemaVersion);
    json.replace(json.find(current), current.size(), "\"schemaVersion\": " + std::to_string(io::kSchemaVersion - 1));
    const auto loaded = io::fromJson(json);
    REQUIRE(loaded);
    CHECK(loaded->colorManagement.isDefault());
    CHECK(*loaded == tl.state);
    // Saved again, it is the current file of the same project (only the version differs).
    CHECK(jsonOf(*loaded) == jsonOf(tl.state));
}

TEST_CASE("Three-way merge of the colour management", "[model][colormanagement][merge]") {
    Timeline tl(kPicture);
    ColorManagement aces;
    aces.mode = ColorManagementMode::Aces13;
    ColorManagement hlg;
    hlg.mode = ColorManagementMode::HdrHlg;
    const ProjectState base = tl.state;
    const ProjectState mine = *edit::setColorManagement(base, aces);
    const ProjectState theirs = *edit::setColorManagement(base, hlg);

    // One side changed it: taken without a conflict.
    auto one = mergeProjects(base, base, theirs);
    REQUIRE(one);
    CHECK(one->merged.colorManagement == hlg);
    CHECK(one->conflicts.empty());
    // Both changed it differently: a conflict, mine by default, theirs when chosen.
    auto both = mergeProjects(base, mine, theirs);
    REQUIRE(both);
    REQUIRE(both->conflicts.size() == 1);
    CHECK(both->conflicts[0].id == "colorManagement");
    CHECK(both->merged.colorManagement == aces);
    auto chosen = mergeProjects(base, mine, theirs, {{"colorManagement", MergeChoice::Theirs}});
    REQUIRE(chosen);
    CHECK(chosen->merged.colorManagement == hlg);
}

TEST_CASE("Project files: gamut compression the output colour space and SDR Conform round-trip and are written only when set (0.6)",
          "[model][colormanagement][io]") {
    Timeline tl(kPicture);
    // The names.
    REQUIRE(allOutputColorSpaces().size() == 6);
    for (const OutputColorSpace s : allOutputColorSpaces()) {
        CHECK(outputColorSpaceFromString(toString(s)) == s);
        CHECK_FALSE(displayName(s).empty());
    }
    CHECK(toString(OutputColorSpace::P3D65) == "p3-d65");
    CHECK(toneMapOperatorFromString("bt2390") == ToneMapOperator::Bt2390);
    CHECK(gamutMappingFromString("aces-rgc") == GamutMapping::AcesRgc);
    CHECK(toString(ColorManagementMode::WideGamutMinimal) == "wide-gamut-minimal");
    // Defaults write nothing new: a Wide Gamut project saves as it did.
    ColorManagement wide;
    wide.mode = ColorManagementMode::WideGamut;
    auto plain = edit::setColorManagement(tl.state, wide);
    REQUIRE(plain);
    const std::string before = jsonOf(*plain);
    CHECK(before.find("gamutCompression") == std::string::npos);
    CHECK(before.find("outputSpace") == std::string::npos);
    CHECK(before.find("sdrConform") == std::string::npos);

    ColorManagement m;
    m.mode = ColorManagementMode::WideGamutMinimal;
    m.gamutCompression = true;
    m.gamutCompressionStrength = 0.5;
    m.outputSpace = OutputColorSpace::Rec2020;
    m.sdrConform.enabled = true;
    m.sdrConform.targetPeakNits = 203.0;
    m.sdrConform.paperWhiteNits = 150.0;
    m.sdrConform.toneMap = ToneMapOperator::Aces;
    m.sdrConform.gamut = GamutMapping::Clip;
    m.sdrConform.legalRange = true;
    CHECK(m.compressesGamut());
    auto set = edit::setColorManagement(tl.state, m);
    REQUIRE(set);
    const std::string json = jsonOf(*set);
    CHECK(json.find("\"wide-gamut-minimal\"") != std::string::npos);
    CHECK(json.find("\"outputSpace\": \"rec2020\"") != std::string::npos);
    const ProjectState back = roundTrip(*set);
    CHECK(back == *set);
    CHECK(back.colorManagement == m);
    // A Direct Rec.709 project with only an output colour space keeps it too.
    ColorManagement direct;
    direct.outputSpace = OutputColorSpace::P3D65;
    CHECK_FALSE(direct.isDefault());
    auto p3 = edit::setColorManagement(tl.state, direct);
    REQUIRE(p3);
    CHECK(roundTrip(*p3).colorManagement.outputSpace == OutputColorSpace::P3D65);
    // Bad values are refused, not half-loaded.
    std::string unknownSpace = json;
    unknownSpace.replace(unknownSpace.find("\"rec2020\""), 9, "\"rec601\"");
    CHECK_FALSE(io::fromJson(unknownSpace).has_value());
    std::string badPeak = json;
    badPeak.replace(badPeak.find("\"targetPeakNits\": 203"), 22, "\"targetPeakNits\": 9000");
    CHECK_FALSE(io::fromJson(badPeak).has_value());
    ColorManagement strong = m;
    strong.gamutCompressionStrength = 1.5;
    CHECK_FALSE(edit::setColorManagement(tl.state, strong).has_value());
    // An edit and its undo: the change set says the colour management changed.
    const ChangeSet changes = diff(tl.state, *set);
    CHECK(changes.colorManagementChanged);
}
