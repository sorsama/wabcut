// Wave-5 review fixes in the recorder (A.8):
//   * A5-P1-7: a pass that ends without stop() (the app quits, the project closes, its owner goes) keeps its take: the
//     recorder's destructor ends it as stop() does; only cancel() deletes the file;
//   * A5-P2-13: the take's journal (WavWriter's sidecar) carries the provenance given at start(), for recovery;
//   * abandon(): a pass whose engine went first closes its file as it is, without touching the engine.

#include "VoiceOverBooth.h"

#include <wabcut/audio/WavWriter.h>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::test;

namespace {

SyntheticSignal ramp() {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Ramp;
    s.frequency = 0.1;
    s.amplitude = 0.5f;
    return s;
}

Project programProject() {
    Project p;
    p.media('A', ramp());
    p.addTrack({{'A', Ticks{}, Ticks::fromSeconds(60), Ticks{}}});
    return p;
}

/// A take from 2 s with no punch-out (it runs until the pass ends), a 1 s count-in and a 0.5 s pre-roll.
VoiceOverOptions manualPass(const std::filesystem::path& file) {
    VoiceOverOptions o;
    o.punchIn = Ticks::fromSeconds(2);
    o.preroll = ms(500);
    o.postroll = ms(250);
    o.countIn = 1;
    o.file = file;
    o.bitsPerSample = 32;
    o.provenance = R"({"kind":"wabcut.voiceOverTake","sequence":7,"track":9,"punchIn":21168000000})";
    return o;
}

} // namespace

TEST_CASE("Destroying the recorder during a pass keeps the take: the WAV and its journal stay and hold what was heard",
          "[playback][voiceover][review]") {
    // Fails today (A5-P1-7): ~VoiceOverRecorder() cancel()ed the pass, and cancel() deleted the WAV, so quitting the app
    // mid-take lost it (a crash kept it).
    Project p = programProject();
    Booth booth(p, {.outputLatency = 480, .reportedInputLatency = 240, .actualInputLatency = 240});
    const auto dir = voiceOverScratch("destroyed");
    const auto file = dir / "take.wav";
    const VoiceOverOptions options = manualPass(file);
    const auto original = booth.rig.engine.source();
    REQUIRE(booth.recorder->start(options));
    REQUIRE(booth.runUntil(VoiceOverState::Recording, 3.0));
    booth.run(500);
    REQUIRE(booth.recorder->status().recorded > ms(400));

    booth.recorder.reset();
    // Ended as stop() ends a pass: the program's source back, the transport paused.
    CHECK(booth.rig.engine.source() == original);
    CHECK(booth.rig.engine.isPaused());
    REQUIRE(std::filesystem::exists(file));
    const auto recovered = audio::recoverWav(file);
    REQUIRE(recovered);
    CHECK(recovered->frames > 48000 * 4 / 10);
    CHECK_FALSE(recovered->repaired); // finished: its header covers it
    CHECK(wavSamples(file).size() == static_cast<std::size_t>(recovered->frames)); // mono: every frame readable
    // The journal, with the provenance given at start() (A5-P2-13).
    const auto journal = audio::readWavJournal(file);
    REQUIRE(journal);
    CHECK(journal->note == options.provenance);
    CHECK(journal->format == audio::WavFormat{48000, 1, 32});
}

TEST_CASE("cancel() deletes the take and its journal; a pass stopped before the punch-in leaves neither",
          "[playback][voiceover][review]") {
    Project p = programProject();
    Booth booth(p, {.outputLatency = 256, .reportedInputLatency = 128, .actualInputLatency = 128});
    const auto dir = voiceOverScratch("cancel-journal");

    const auto cancelled = dir / "cancelled.wav";
    REQUIRE(booth.recorder->start(manualPass(cancelled)));
    CHECK(std::filesystem::exists(audio::wavJournalPath(cancelled))); // written at start()
    REQUIRE(booth.runUntil(VoiceOverState::Recording, 3.0));
    booth.run(200);
    booth.recorder->cancel();
    CHECK_FALSE(std::filesystem::exists(cancelled));
    CHECK_FALSE(std::filesystem::exists(audio::wavJournalPath(cancelled)));

    const auto early = dir / "early.wav";
    VoiceOverOptions late = manualPass(early);
    late.punchIn = Ticks::fromSeconds(30);
    REQUIRE(booth.recorder->start(late));
    booth.run(300);
    const auto none = booth.recorder->stop();
    REQUIRE_FALSE(none);
    CHECK(none.error().code == ErrorCode::NotFound);
    CHECK_FALSE(std::filesystem::exists(early));
    CHECK_FALSE(std::filesystem::exists(audio::wavJournalPath(early)));

    // A finished take keeps its journal: its owner removes it.
    const auto kept = dir / "kept.wav";
    REQUIRE(booth.recorder->start(manualPass(kept)));
    REQUIRE(booth.runUntil(VoiceOverState::Recording, 3.0));
    booth.run(200);
    REQUIRE(booth.recorder->stop());
    CHECK(std::filesystem::exists(kept));
    CHECK(std::filesystem::exists(audio::wavJournalPath(kept)));
}

TEST_CASE("abandon() closes the take as it is and leaves the engine alone (its owner tore it down first)",
          "[playback][voiceover][review]") {
    Project p = programProject();
    Booth booth(p, {.outputLatency = 480, .reportedInputLatency = 240, .actualInputLatency = 240});
    const auto dir = voiceOverScratch("abandoned");
    const auto file = dir / "take.wav";
    const auto original = booth.rig.engine.source();
    REQUIRE(booth.recorder->start(manualPass(file)));
    REQUIRE(booth.runUntil(VoiceOverState::Recording, 3.0));
    booth.run(300);
    booth.recorder->abandon();
    CHECK(booth.recorder->status().state == VoiceOverState::Idle);
    CHECK(booth.rig.engine.source() != original); // not touched: the engine may have been gone
    CHECK_FALSE(booth.recorder->stop());           // no pass any more
    booth.rig.engine.setSource(original);          // (this engine is still here: the program back)
    REQUIRE(std::filesystem::exists(file));
    REQUIRE(std::filesystem::exists(audio::wavJournalPath(file))); // for the recovery
    const auto recovered = audio::recoverWav(file);
    REQUIRE(recovered);
    CHECK(recovered->frames > 0);
}
