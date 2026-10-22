// 0.2 media formats through the whole export (evaluator, decoders, compositor, encoder): interlaced footage deinterlaced
// in a progressive sequence - both fields at the field rate, the dominant one at the frame rate, the other with Reverse
// Field Dominance (model/FieldOptions.h) - and a spanned clip (GoPro chapters) exported frame for frame across the join.
// The fixtures carry each picture's frame number as a barcode; in mpeg2_interlaced_tff.mpg the upper field of frame n
// is pattern frame 2n and the lower field pattern frame 2n + 1, so the barcode an exported frame shows says which field
// it came from.

#include "ExportTestUtil.h"

#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportSettings.h>
#include <wabcut/model/FieldOptions.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/SpannedClip.h>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>
#include <vector>

using namespace exporttest;
using namespace wabcut;
using namespace wabcut::exporter;

namespace {

std::vector<int> barcodes(const std::filesystem::path& file) {
    std::vector<int> codes;
    std::string error;
    const bool decoded = decodeVideo(file, [&](const YuvFrame& f) { codes.push_back(barcodeOf(f)); }, &error);
    INFO(error);
    REQUIRE(decoded);
    return codes;
}

/// `project` exported at its size and rate (Match Source, high quality, no audio) through the rendered source.
std::vector<int> exported(const TestProject& project, const std::string& dir) {
    auto p = findPreset("match-source");
    REQUIRE(p.has_value());
    ExportSettings s = p->settings;
    s.audio.enabled = false;
    s.video.quality = 12;
    const auto out = freshOutputDir(dir) / "out.mp4";
    ExportRequest request = makeRequest(project, s, out);
    request.videoSource = {}; // the rendered source
    const auto result = ExportJob(std::move(request)).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    return barcodes(out);
}

std::string firstWrong(const std::vector<int>& codes, const std::function<int(std::size_t)>& expected, int* wrong) {
    std::string first;
    *wrong = 0;
    for (std::size_t k = 0; k < codes.size(); ++k) {
        if (codes[k] != expected(k) && (*wrong)++ == 0) {
            first = std::format("frame {} shows {}, expected {}", k, codes[k], expected(k));
        }
    }
    return first;
}

} // namespace

TEST_CASE("Interlaced footage exports deinterlaced: both fields at the field rate, one at the frame rate",
          "[export][media][gpu][fields]") {
    if (!haveMedia("mpeg2_interlaced_tff.mpg")) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const Ticks length = Ticks::fromSeconds(1) - framesToTicks(2, Rate{30000, 1001});
    SECTION("59.94p: frame k shows pattern frame k (upper field, then lower field)") {
        TestProject project(640, 480, Rate{60000, 1001}, length);
        const MediaId m = project.addMedia("mpeg2_interlaced_tff.mpg");
        project.addVideoTrack(m, Ticks{}, length, Ticks{});
        const std::vector<int> codes = exported(project, "fields-bob");
        REQUIRE(codes.size() >= 50);
        int wrong = 0;
        INFO(firstWrong(codes, [](std::size_t k) { return static_cast<int>(k); }, &wrong));
        CHECK(wrong == 0);
    }
    SECTION("29.97p: frame k shows its upper field (pattern frame 2k); Reverse Field Dominance the lower (2k + 1)") {
        for (const bool reverse : {false, true}) {
            TestProject project(640, 480, Rate{30000, 1001}, length);
            const MediaId m = project.addMedia("mpeg2_interlaced_tff.mpg");
            Clip clip = project.clip(m, StreamKind::Video, Ticks{}, length, Ticks{});
            if (reverse) {
                ClipContent content = clip.content();
                content.fieldOptions.reverseFieldDominance = true;
                clip.setContent(std::move(content));
            }
            project.addTrack(TrackKind::Video, {clip});
            const std::vector<int> codes = exported(project, reverse ? "fields-reverse" : "fields-upper");
            REQUIRE(codes.size() >= 25);
            int wrong = 0;
            INFO("reverse " << reverse << ": "
                            << firstWrong(codes, [&](std::size_t k) { return static_cast<int>(2 * k + (reverse ? 1 : 0)); },
                                          &wrong));
            CHECK(wrong == 0);
        }
    }
}

TEST_CASE("A spanned clip exports frame for frame across the join of its parts", "[export][media][gpu][spanned]") {
    if (!haveMedia("GX010042.MP4") || !haveMedia("GX020042.MP4")) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    SpannedClipSpec spec;
    spec.directory = mediaDir();
    spec.parts = {{"GX010042.MP4", 1'000'000}, {"GX020042.MP4", 1'000'000}};
    const Ticks length = Ticks::fromSeconds(2);
    TestProject project(640, 360, Rate{30, 1}, length);
    const MediaId m = project.addMedia("GX010042.MP4");
    project.editMedia(m, [&](MediaItem& item) {
        item.locator.path = spannedClipPath(spec);
        item.info.duration = length;
        item.info.video.front().duration = length;
        item.info.video.front().frameCount = 60;
    });
    project.addVideoTrack(m, Ticks{}, length, Ticks{});
    const std::vector<int> codes = exported(project, "spanned");
    REQUIRE(codes.size() == 60);
    int wrong = 0;
    INFO(firstWrong(codes, [](std::size_t k) { return static_cast<int>(k); }, &wrong));
    CHECK(wrong == 0);
}
