// p02 audio (audio UI tidy, Notion 3f812c52): the timeline shades the part of an audio clip whose waveform peaks are
// still being decoded (PeakSource::buildingUntil, MediaPeakSource's progressive partial results), and new peaks rebuild
// only the audio tracks holding clips of the media that changed (PeakSource::mediaChangedSince), not every waveform.
// "Show Audio Waveforms Using Clip Volume" scales the waveforms by each clip's Volume (and Audio Gain).

#include "TimelineHarness.h"

#include "PeakSource.h"
#include "TimelineView.h"

#include <catch2/catch_test_macros.hpp>
#include <wabcut/model/FinishingOps.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

/// Flat peaks; says which media changed (by serial) and which one is still building.
class FakePeaks final : public PeakSource {
public:
    bool peaks(MediaId, StreamRef, Ticks, Ticks, std::span<float> out) const override {
        std::fill(out.begin(), out.end(), 0.5f);
        return true;
    }
    std::optional<Ticks> buildingUntil(MediaId media, StreamRef) const override {
        if (media == building)
            return until;
        return std::nullopt;
    }
    std::optional<std::vector<std::uint64_t>> mediaChangedSince(std::uint64_t serial, std::uint64_t& now) const override {
        now = m_serial;
        std::vector<std::uint64_t> out;
        for (const auto& [media, at] : m_changed) {
            if (at > serial)
                out.push_back(media);
        }
        return out;
    }
    void touch(MediaId media) { m_changed[media.value] = ++m_serial; }

    MediaId building;
    Ticks until;

private:
    std::uint64_t m_serial = 1;
    std::map<std::uint64_t, std::uint64_t> m_changed;
};

constexpr const char* kPicture = R"(
    V1 |Aaaaaaaaaaaaaaaaaaaa|
    A1 |Aaaaaaaaaaaaaaaaaaaa|
    A2 |Bbbbbbbbbbbbbbbbbbbb|
    A3 |Cccccccccccccccccccc|
)";

} // namespace

TEST_CASE("New peaks rebuild only the audio tracks of their media and building peaks are shaded", "[app][timeline][waveform]") {
    Rig rig(kPicture);
    auto fake = std::make_shared<FakePeaks>();
    rig.view->setPeakSource(fake);
    rig.harness.frame();
    CHECK(rig.view->stats().fullRebuild);
    CHECK(rig.view->stats().buildingWaveforms == 0);

    const SequencePtr seq = rig.controller.currentSequence();
    REQUIRE(seq->audioTracks.size() == 3);
    const Clip& onA2 = seq->audioTracks[1]->clips.front();
    const MediaId mediaB = onA2.media;
    REQUIRE(mediaB != seq->audioTracks[0]->clips.front().media);

    SECTION("peaks of one media rebuild the one track that plays it") {
        fake->touch(mediaB);
        rig.view->refreshPeaks();
        const int before = rig.view->stats().frames;
        rig.harness.frame();
        REQUIRE(rig.view->stats().frames > before);
        CHECK_FALSE(rig.view->stats().fullRebuild);
        CHECK(rig.view->stats().rebuiltTracks == 1);

        // Nothing new: a redraw rebuilds no track.
        rig.view->refreshPeaks();
        rig.view->update();
        rig.harness.frame();
        CHECK_FALSE(rig.view->stats().fullRebuild);
        CHECK(rig.view->stats().rebuiltTracks == 0);
    }
    SECTION("the part still building is shaded until its peaks are complete") {
        fake->building = mediaB;
        fake->until = onA2.sourceIn + Ticks{onA2.sourceDuration().value / 2};
        fake->touch(mediaB);
        rig.view->refreshPeaks();
        rig.harness.frame();
        CHECK(rig.view->stats().rebuiltTracks == 1);
        CHECK(rig.view->stats().buildingWaveforms == 1);
        // Complete: the track rebuilds without the shade.
        fake->building = MediaId{};
        fake->touch(mediaB);
        rig.view->refreshPeaks();
        rig.harness.frame();
        CHECK(rig.view->stats().rebuiltTracks == 1);
        CHECK(rig.view->stats().buildingWaveforms == 0);
    }
    SECTION("Show Audio Waveforms Using Clip Volume scales them by the clip's Volume") {
        CHECK(rig.view->stats().loudestWaveform == 0.5f);
        // A2's clip at -6 dB, the others at -12 dB: the loudest waveform is A2's, 6 dB down.
        const SequenceId seqId = rig.controller.currentSequenceId();
        for (std::size_t i = 0; i < seq->audioTracks.size(); ++i) {
            const ClipId id = seq->audioTracks[i]->clips.front().id;
            const double db = i == 1 ? -6.0 : -12.0;
            REQUIRE(rig.controller.session().apply("Volume", [&](const ProjectState& state) {
                return edit::setParameter(state, seqId, id, edit::ParamRef::of(ParamId::VolumeDb), ParamValue(db));
            }));
        }
        rig.view->setVolumeWaveforms(true);
        rig.harness.frame();
        CHECK(rig.view->stats().fullRebuild);
        CHECK(std::abs(rig.view->stats().loudestWaveform - 0.5f * 0.501187f) < 1e-3f);
        rig.view->setVolumeWaveforms(false);
        rig.harness.frame();
        CHECK(rig.view->stats().loudestWaveform == 0.5f);
    }
    SECTION("a source that cannot say which media changed rebuilds every waveform") {
        rig.view->setPeakSource(std::make_shared<PlaceholderPeakSource>());
        rig.harness.frame();
        rig.view->refreshPeaks();
        rig.harness.frame();
        CHECK(rig.view->stats().fullRebuild);
    }
}
