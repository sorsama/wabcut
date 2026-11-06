// Stream A, batch A7 (0.7), export module: a soloed video track is the only one that renders (card 106), so the export's
// used-media check and the pre-export check read the solo like the picture does (review fix).

#include "ExportTestUtil.h"

#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportPresets.h>
#include <wabcut/export/PreExportCheck.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>

using namespace exporttest;
using namespace wabcut;
using namespace wabcut::exporter;

namespace {

constexpr Rate kRate30{30, 1};

ExportSettings presetNamed(std::string_view id) {
    const auto p = findPreset(id);
    REQUIRE(p.has_value());
    return p->settings;
}

/// threeLayerTimeline with V1's file offline and V2 (the ProRes clip) soloed.
TestProject offlineUnderSolo() {
    TestProject project = threeLayerTimeline(640, 360, kRate30, Ticks::fromSeconds(1));
    const MediaId h264 = project.state.media[0]->id;
    project.editMedia(h264, [](MediaItem& m) { m.locator.offline = true; });
    project.editSequence([](Sequence& s) {
        Track v2 = *s.videoTracks[1];
        v2.solo = true;
        s.videoTracks[1] = std::make_shared<const Track>(std::move(v2));
    });
    return project;
}

} // namespace

TEST_CASE("Video solo: an offline file on a track that does not render does not stop the export (106)", "[export][solo][video]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const auto dir = freshOutputDir("a7-video-solo");
    SECTION("with V2 soloed, V1's offline file is not used") {
        const TestProject project = offlineUnderSolo();
        const auto r = ExportJob(makeRequest(project, presetNamed("match-source"), dir / "solo.mp4")).run();
        INFO((r ? std::string() : r.error().message));
        CHECK(r.has_value());
    }
    SECTION("without the solo the same offline file stops the export (the control)") {
        TestProject project = offlineUnderSolo();
        project.editSequence([](Sequence& s) {
            Track v2 = *s.videoTracks[1];
            v2.solo = false;
            s.videoTracks[1] = std::make_shared<const Track>(std::move(v2));
        });
        const auto r = ExportJob(makeRequest(project, presetNamed("match-source"), dir / "plain.mp4")).run();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::NotFound);
        CHECK(r.error().message.find("h264_1080p60_aac.mp4") != std::string::npos);
    }
    SECTION("the soloed track's own offline file still stops it") {
        TestProject project = offlineUnderSolo();
        const MediaId prores = project.state.media[1]->id;
        project.editMedia(prores, [](MediaItem& m) { m.locator.offline = true; });
        const auto r = ExportJob(makeRequest(project, presetNamed("match-source"), dir / "both.mp4")).run();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().message.find("prores_422.mov") != std::string::npos);
    }
}

TEST_CASE("Video solo: the pre-export check ignores the tracks the solo hides (106)", "[export][solo][check]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    ExportSettings settings = presetNamed("youtube-1080p");
    settings.video.width.reset();
    settings.video.height.reset();
    const TestProject project = offlineUnderSolo();
    const auto rows = preExportCheck(project.state, project.seq(), settings);
    // V1's offline file is not rendered: no Offline media row for it.
    CHECK(std::none_of(rows.begin(), rows.end(), [](const CheckIssue& r) { return r.kind == CheckKind::OfflineMedia; }));
    // The picture is V2 alone (L/4 .. 3L/4): the start and the end of the sequence are gaps, which V1 would have covered.
    CHECK(std::any_of(rows.begin(), rows.end(), [](const CheckIssue& r) { return r.kind == CheckKind::Gap; }));
}
