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
    REQUIRE(allColorManagementModes().size() == 6);
    for (const ColorManagementMode m : allColorManagementModes()) {
        CHECK(colorManagementModeFromString(toString(m)) == m);
        CHECK_FALSE(displayName(m).empty());
        CHECK_FALSE(description(m).empty());
    }
    CHECK(toString(ColorManagementMode::Aces13) == "aces");
    CHECK(toString(ColorManagementMode::HdrPq) == "hdr-pq");
    CHECK_FALSE(colorManagementModeFromString("ACES").has_value()); // ids are exact
    CHECK(allInputColorSpaces().size() == 18);
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
                                                 "Wide Gamut (Tone Mapped)", "Wide Gamut (No Tone Mapping)",
                                                 "Disable Color Management"});
    CHECK(ColorManagement{}.mode == ColorManagementMode::DisplayRec709);
    CHECK(toString(ColorManagementMode::WideGamut) == "wide-gamut");
    CHECK(toString(ColorManagementMode::Disabled) == "off");
    // Managed working spaces: HDR and Wide Gamut; Direct Rec.709 and "off" keep the display-referred pipeline.
    for (const ColorManagementMode m : allColorManagementModes()) {
        ColorManagement cm;
        cm.mode = m;
        const bool managed = m == ColorManagementMode::HdrPq || m == ColorManagementMode::HdrHlg ||
                             m == ColorManagementMode::Aces13 || m == ColorManagementMode::WideGamut;
        CHECK(cm.managed() == managed);
    }
    CHECK(isCameraLog(InputColorSpace::SLog3SGamut3Cine));
    CHECK(isCameraLog(InputColorSpace::RedLog3G10));
    CHECK(isCameraLog(InputColorSpace::AcesCct));
    CHECK_FALSE(isCameraLog(InputColorSpace::Rec2100Pq));
    CHECK_FALSE(isCameraLog(InputColorSpace::AcesCg));
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
