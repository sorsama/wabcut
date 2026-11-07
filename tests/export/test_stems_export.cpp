// 0.6 (#52): dialogue / music / effects stems with the master (Stems.h). Which roles a sequence writes, the stems'
// settings, names and soloed project copies; and end to end through export.queue: the stems are queued after the
// master, written next to it, and line up with it sample for sample (adding up to it when the master bus has no
// effects).

#include "ExportTestUtil.h"

#include <wabcut/command/Engine.h>
#include <wabcut/command/Registry.h>
#include <wabcut/export/ExportCommandServices.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportPresets.h>
#include <wabcut/export/Stems.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

using namespace exporttest;
using namespace wabcut;
using namespace wabcut::exporter;

namespace {

/// An audio track of `role` with one 1 s clip (none when `clips` is false).
TrackPtr audioTrack(std::uint64_t id, TrackRole role, bool clips = true, bool muted = false, bool solo = false) {
    auto t = std::make_shared<Track>();
    t->id = TrackId{id};
    t->kind = TrackKind::Audio;
    t->role = role;
    t->muted = muted;
    t->solo = solo;
    if (clips) {
        Clip c;
        c.id = ClipId{id * 10};
        c.timelineRange = TimeRange{Ticks{}, Ticks::fromSeconds(1)};
        t->clips.push_back(std::move(c));
    }
    return t;
}

const Track& trackOf(const ProjectState& state, SequenceId seq, std::size_t index) {
    return *state.findSequence(seq)->audioTracks[index];
}

} // namespace

TEST_CASE("Stems: the roles a sequence writes their names settings and soloed project copies", "[export][stems]") {
    TestProject p(640, 360, Rate{30, 1}, Ticks::fromSeconds(1));
    p.editSequence([](Sequence& s) {
        s.audioTracks = {audioTrack(1, TrackRole::Dialogue), audioTrack(2, TrackRole::Music),
                         audioTrack(3, TrackRole::Dialogue), audioTrack(4, TrackRole::Effects, true, true), // muted
                         audioTrack(5, TrackRole::None), audioTrack(6, TrackRole::Effects, false)};         // no clip
        EffectInstance limiter;
        limiter.effect = "wabcut.audio.hardLimiter";
        s.audioEffects.push_back(limiter);
    });
    // Dialogue (2 tracks), Music, Other - the muted and the empty Effects tracks write nothing.
    std::vector<StemRole> roles = stemRoles(p.seq());
    REQUIRE(roles.size() == 3);
    CHECK(roles[0].key == "dialogue");
    CHECK(roles[0].suffix == "DX");
    CHECK(roles[0].tracks == 2);
    CHECK(roles[1].key == "music");
    CHECK(roles[1].suffix == "MX");
    CHECK(roles[2].key == "other");
    CHECK(roles[2].suffix == "OTHER");
    CHECK(stemRoleFromKey("effects") == TrackRole::Effects);
    CHECK(stemRoleFromKey("other") == TrackRole::None);
    CHECK_FALSE(stemRoleFromKey("dx").has_value());

    // Names: next to the master, after its file.
    CHECK(stemOutput(std::filesystem::path("out") / "Episode 3.mp4", TrackRole::Dialogue) ==
          std::filesystem::path("out") / "Episode 3_DX.wav");
    CHECK(stemOutput("Episode 3.mov", TrackRole::None) == std::filesystem::path("Episode 3_OTHER.wav"));

    // Settings: WAV 24-bit at the master's rate and channel format, same range, nothing normalised.
    auto master = findPreset("youtube-1080p");
    REQUIRE(master.has_value());
    ExportSettings m = master->settings;
    m.audio.sampleRate = 44100;
    m.range = RangeMode::Custom;
    m.customRange = TimeRange{ms(500), ms(250)};
    m.effects.loudness.applied = true;
    m.captions.mode = CaptionExport::Srt;
    const ExportSettings s = stemSettings(m);
    CHECK(s.container == Container::Wav);
    CHECK_FALSE(s.video.enabled);
    CHECK(s.audio.enabled);
    CHECK(s.audio.codec == AudioCodec::Pcm24);
    CHECK(s.audio.sampleRate == 44100);
    CHECK(s.audio.channels == 0);
    CHECK(s.range == RangeMode::Custom);
    CHECK(s.customRange == m.customRange);
    CHECK_FALSE(s.effects.loudness.applied);
    CHECK(s.captions.mode == CaptionExport::None);
    {
        const auto valid = validate(s);
        INFO((valid ? std::string() : valid.error().message));
        CHECK(valid.has_value());
    }
    m.audio.enabled = false;
    CHECK(stemSettings(m).audio.sampleRate == 48000);

    // The project copy: that role's audible tracks soloed, the rest not; the master bus as asked.
    const ProjectState dx = stemProject(p.state, p.sequence, TrackRole::Dialogue, true);
    CHECK(trackOf(dx, p.sequence, 0).solo);
    CHECK_FALSE(trackOf(dx, p.sequence, 1).solo);
    CHECK(trackOf(dx, p.sequence, 2).solo);
    CHECK_FALSE(trackOf(dx, p.sequence, 4).solo);
    CHECK(dx.findSequence(p.sequence)->audioEffects.size() == 1);
    const ProjectState mx = stemProject(p.state, p.sequence, TrackRole::Music, false);
    CHECK(trackOf(mx, p.sequence, 1).solo);
    CHECK_FALSE(trackOf(mx, p.sequence, 0).solo);
    CHECK(mx.findSequence(p.sequence)->audioEffects.empty());
    CHECK(p.seq().audioEffects.size() == 1); // the project itself is untouched

    // Tracks the master does not play stay out: with a soloed Music track, only Music is written.
    p.editSequence([](Sequence& seq) {
        auto music = std::make_shared<Track>(*seq.audioTracks[1]);
        music->solo = true;
        seq.audioTracks[1] = std::move(music);
    });
    roles = stemRoles(p.seq());
    REQUIRE(roles.size() == 1);
    CHECK(roles[0].role == TrackRole::Music);
    CHECK_FALSE(trackOf(stemProject(p.state, p.sequence, TrackRole::Dialogue, true), p.sequence, 0).solo);
}

TEST_CASE("Stems: export.queue writes the dialogue and music stems after the master sample-aligned with it",
          "[export][media][stems]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject project = threeLayerTimeline(640, 360, Rate{30, 1}, Ticks::fromSeconds(1));
    project.editSequence([](Sequence& s) {
        auto dialogue = std::make_shared<Track>(*s.audioTracks[0]); // the ramp, kept under full scale with the sine
        dialogue->role = TrackRole::Dialogue;
        dialogue->volume = ParamValue(-12.0);
        s.audioTracks[0] = std::move(dialogue);
        auto music = std::make_shared<Track>(*s.audioTracks[1]); // the sine, from 1/8 of the sequence
        music->role = TrackRole::Music;
        s.audioTracks[1] = std::move(music);
    });
    const auto dir = freshOutputDir("stems");
    auto services = ExportCommandServices::standalone(dir / "project.wabcut", dir / "presets", syntheticVideoSourceFactory());
    EditSession session(project.state);
    command::EnginePolicy policy;
    policy.services = services;
    command::CommandEngine engine(session, command::builtinCommands(), policy);
    const auto run = [&](const std::string& name, command::Json params, bool dryRun = false) {
        command::CommandRequest request;
        request.command = name;
        request.params = std::move(params);
        request.dryRun = dryRun;
        return engine.execute(request);
    };

    // A dry run lists the master and its stems and queues nothing.
    auto r = run("export.quick", {{"preset", "audio-wav"}, {"output", pathToUtf8(dir / "dry.wav")}, {"stems", "role"}}, true);
    REQUIRE(r.ok());
    REQUIRE(r.data["exports"].size() == 3);
    CHECK(r.data["exports"][1]["stem"] == "dialogue");
    CHECK(r.data["exports"][1]["output"] == pathToUtf8(dir / "dry_DX.wav"));
    CHECK(r.data["exports"][2]["output"] == pathToUtf8(dir / "dry_MX.wav"));
    CHECK(r.data["exports"][2]["extension"] == ".wav");
    CHECK(run("export.queue.list", command::Json::object()).data["jobs"].empty());

    // Two destinations: one set of stems, after the first; then the second destination.
    r = run("export.queue", {{"presets", {"audio-wav", "audio-aiff"}},
                             {"outputs", {pathToUtf8(dir / "master.wav"), pathToUtf8(dir / "master.aif")}},
                             {"stems", "role"}});
    REQUIRE(r.ok());
    const command::Json exports = r.data["exports"];
    REQUIRE(exports.size() == 4);
    CHECK(exports[0]["output"] == pathToUtf8(dir / "master.wav"));
    CHECK(exports[1]["output"] == pathToUtf8(dir / "master_DX.wav"));
    CHECK(exports[2]["output"] == pathToUtf8(dir / "master_MX.wav"));
    CHECK(exports[3]["output"] == pathToUtf8(dir / "master.aif"));
    const command::Json finished = services->waitForJobs();
    REQUIRE(finished.size() == 4);
    for (const command::Json& job : finished) {
        INFO(job.dump());
        CHECK(job["state"] == "done");
    }

    const auto master = decodeAudio(dir / "master.wav");
    const auto dx = decodeAudio(dir / "master_DX.wav");
    const auto mx = decodeAudio(dir / "master_MX.wav");
    REQUIRE(master.has_value());
    REQUIRE(dx.has_value());
    REQUIRE(mx.has_value());
    CHECK(dx->codec == "pcm_s24le");
    CHECK(dx->sampleRate == master->sampleRate);
    CHECK(dx->channels == master->channels);
    // Sample-aligned: the same length, and dialogue + music is the master to the 24-bit step.
    REQUIRE(dx->samples.size() == master->samples.size());
    REQUIRE(mx->samples.size() == master->samples.size());
    REQUIRE(master->samples.size() >= 48000u * 2 - 2);
    constexpr double kQuantum = 1.0 / 8388608.0;
    double worst = 0.0;
    for (std::size_t i = 0; i < master->samples.size(); ++i) {
        worst = std::max(worst, std::fabs(static_cast<double>(dx->samples[i]) + mx->samples[i] - master->samples[i]));
    }
    CHECK(worst <= 3.0 * kQuantum);
    // Each stem is its role only: the music is silent before the sine starts (1/8 s) while the dialogue plays.
    double musicEarly = 0.0;
    double dialogueEarly = 0.0;
    for (std::size_t k = 100; k < 5000; ++k) {
        musicEarly = std::max(musicEarly, static_cast<double>(std::fabs(mx->samples[k * 2])));
        dialogueEarly = std::max(dialogueEarly, static_cast<double>(std::fabs(dx->samples[k * 2])));
    }
    CHECK(musicEarly == 0.0);
    CHECK(dialogueEarly > 0.001);

    // Stems of a sequence without audio tracks: none, the master alone.
    CHECK(services->stemExports(project.state, project.sequence, {"audio-wav", command::Json::object(), ""},
                                pathToUtf8(dir / "x.wav"), true)
              .size() == 2);
    TestProject silent(640, 360, Rate{30, 1}, Ticks::fromSeconds(1));
    CHECK(services->stemExports(silent.state, silent.sequence, {"audio-wav", command::Json::object(), ""},
                                pathToUtf8(dir / "x.wav"), true)
              .empty());
}
