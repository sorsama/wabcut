// AI-13 end to end on real media (full builds, FFmpeg for the camera files): three cameras (test pictures with the
// room's sound, started at different times) and two microphones (WAV, each hearing its speaker and the other's bleed).
// workflow.podcast syncs them by their sound, lays out the episode, measures the mix's loudness, removes the long
// pauses, switches cameras on the decoded microphone levels (and the cameras' faces), makes captions and chapters.
// The project is kept in <build>/test-output/multicam-media for a wabcut-cli run.

#include "MulticamTestUtil.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/multicam/MediaAngleSignals.h>
#include <wabcut/ai/workflow/WorkflowCommands.h>
#include <wabcut/ai/vision/Subjects.h>
#include <wabcut/audio/edit/AudioSync.h>
#include <wabcut/audio/edit/MediaAnalysis.h>
#include <wabcut/command/Engine.h>
#include <wabcut/media/Probe.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <set>

using namespace wabcut;
using namespace wabcut::ai::multicam;
using namespace wabcut::ai::multicam::test;
using command::Json;

namespace {

constexpr int kRate = 48000;
constexpr double kLength = 30.0;

/// Who talks when (master time): the host, the guest, a stretch of crosstalk.
const std::vector<Talk> kScript{{0.5, 8.0, 0}, {8.5, 17.0, 1}, {15.0, 17.0, 0}, {17.5, 24.0, 0}, {24.5, 29.5, 1}};

/// A voice: syllables of shaped noise and a pitched tone, a different rhythm per person (distinct enough to sync on).
std::vector<float> voice(int who) {
    std::vector<float> x(static_cast<std::size_t>(kLength * kRate), 0.0f);
    std::uint32_t seed = who == 0 ? 12345u : 98765u;
    const auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<double>(seed >> 8) / static_cast<double>(1u << 24);
    };
    const double pitch = who == 0 ? 120.0 : 210.0;
    for (const Talk& k : kScript) {
        if (k.who != who) {
            continue;
        }
        double t = k.start;
        while (t < k.end - 0.1) {
            const double length = 0.12 + 0.2 * rnd();
            const double gain = 0.5 + 0.5 * rnd();
            for (double s = t; s < std::min(t + length, k.end); s += 1.0 / kRate) {
                const double env = std::sin(std::numbers::pi * (s - t) / length);
                const auto i = static_cast<std::size_t>(s * kRate);
                if (i < x.size()) {
                    x[i] += static_cast<float>(gain * env * (0.6 * std::sin(2 * std::numbers::pi * pitch * s) + 0.4 * (2 * rnd() - 1)));
                }
            }
            t += length + 0.03 + 0.08 * rnd();
        }
    }
    return x;
}

void writeWav(const std::filesystem::path& file, const std::vector<float>& x) {
    std::ofstream out(file, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const auto bytes = static_cast<std::uint32_t>(x.size() * 2);
    out.write("RIFF", 4);
    u32(36 + bytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(kRate);
    u32(kRate * 2);
    u16(2);
    u16(16);
    out.write("data", 4);
    u32(bytes);
    for (const float v : x) {
        u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f))));
    }
}

/// `x` as heard by a device that started `start` seconds into the master clock (negative: before it), `seconds` long.
std::vector<float> recorded(const std::vector<float>& x, double start, double seconds, std::uint32_t noiseSeed) {
    std::vector<float> out(static_cast<std::size_t>(seconds * kRate));
    for (std::size_t i = 0; i < out.size(); ++i) {
        noiseSeed = noiseSeed * 1664525u + 1013904223u;
        const auto j = static_cast<std::int64_t>(i) + static_cast<std::int64_t>(std::llround(start * kRate));
        const float v = j >= 0 && j < static_cast<std::int64_t>(x.size()) ? x[static_cast<std::size_t>(j)] : 0.0f;
        out[i] = v + 0.002f * (static_cast<float>(noiseSeed >> 8) / static_cast<float>(1u << 24) - 0.5f);
    }
    return out;
}

MediaId addMedia(ProjectState& state, const std::filesystem::path& file, const std::string& name) {
    auto info = media::probe(pathToUtf8(file));
    INFO(pathToUtf8(file));
    REQUIRE(info);
    MediaItem item;
    item.id = state.ids.allocate<MediaId>();
    item.name = name;
    item.locator.path = pathToUtf8(file);
    item.info = *info;
    const MediaId id = item.id;
    state.media.push_back(std::make_shared<const MediaItem>(std::move(item)));
    return id;
}

/// Faces in a camera's own time: `who` 0 / 1 / both, lips moving while they talk.
ai::vision::SubjectAnalysis facesOf(std::vector<int> people, double start) {
    ai::vision::SubjectAnalysis a;
    a.frameWidth = 640;
    a.frameHeight = 360;
    a.start = Ticks{};
    a.end = secs(kLength);
    int k = 0;
    for (double s = 0.0; s < kLength; s += 0.2, ++k) {
        ai::vision::SubjectSample sample;
        sample.time = secs(s);
        for (std::size_t f = 0; f < people.size(); ++f) {
            ai::vision::SubjectBox box;
            box.box = ai::vision::Box{0.2f + 0.4f * static_cast<float>(f), 0.2f, 0.3f, 0.45f};
            box.score = 0.9f;
            box.track = static_cast<int>(f);
            box.activity = talking(kScript, people[f], s + start) ? ((k % 2) ? 0.5f : 0.3f) : 0.03f;
            sample.faces.push_back(box);
        }
        a.samples.push_back(std::move(sample));
    }
    return a;
}

} // namespace

TEST_CASE("The podcast workflow on real files: synced by sound, switched on the microphones' levels", "[ai][multicam][media]") {
#ifndef WABCUT_MULTICAM_FFMPEG
    SKIP("no ffmpeg in this build to make the camera files");
#else
    const std::filesystem::path dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "multicam-media";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::vector<float> host = voice(0);
    const std::vector<float> guest = voice(1);
    std::vector<float> hostMic(host.size()), guestMic(host.size()), room(host.size());
    for (std::size_t i = 0; i < host.size(); ++i) {
        hostMic[i] = 0.35f * host[i] + 0.03f * guest[i];
        guestMic[i] = 0.35f * guest[i] + 0.03f * host[i];
        room[i] = 0.12f * (host[i] + guest[i]);
    }
    writeWav(dir / "Host mic.wav", recorded(hostMic, 0.0, kLength, 1));
    writeWav(dir / "Guest mic.wav", recorded(guestMic, 0.0, kLength, 2));
    // The cameras started at different times: B 1.5 s after the mics, C 0.8 s before them.
    const std::vector<std::pair<std::string, double>> cameras{{"Cam A", 0.0}, {"Cam B", 1.5}, {"Cam C", -0.8}};
    for (std::size_t c = 0; c < cameras.size(); ++c) {
        const auto& [name, start] = cameras[c];
        const auto wav = dir / (name + " sound.wav");
        writeWav(wav, recorded(room, start, kLength - 1.6, 10 + static_cast<std::uint32_t>(c)));
        // The picture: 160x90 at 5 fps, a camera colour with a moving bar (raw RGB for FFmpeg).
        const auto raw = dir / (name + ".rgb");
        {
            std::ofstream frames(raw, std::ios::binary);
            const int w = 160, h = 90, n = static_cast<int>((kLength - 1.6) * 5);
            std::vector<unsigned char> frame(static_cast<std::size_t>(w * h * 3));
            for (int f = 0; f < n; ++f) {
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        unsigned char* p = &frame[static_cast<std::size_t>((y * w + x) * 3)];
                        const bool bar = (x + f * 4) % w < 12;
                        p[0] = static_cast<unsigned char>(bar ? 240 : 60 + 60 * static_cast<int>(c));
                        p[1] = static_cast<unsigned char>(bar ? 240 : 40 + y);
                        p[2] = static_cast<unsigned char>(bar ? 240 : 160 - 50 * static_cast<int>(c));
                    }
                }
                frames.write(reinterpret_cast<const char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
            }
        }
        const std::string command = std::string("\"\"") + WABCUT_MULTICAM_FFMPEG + "\" -loglevel error -y -f rawvideo -pix_fmt rgb24 -s 160x90 -r 5 -i \"" +
                                    pathToUtf8(raw) + "\" -i \"" + pathToUtf8(wav) + "\" -c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -shortest \"" +
                                    pathToUtf8(dir / (name + ".mp4")) + "\"\"";
        REQUIRE(std::system(command.c_str()) == 0);
    }

    ProjectState state;
    auto seq = wabcut::edit::addSequence(state, wabcut::edit::SequenceSettings{});
    REQUIRE(seq);
    state = std::move(seq->state);
    std::vector<MediaId> cams;
    for (const auto& [name, start] : cameras) {
        cams.push_back(addMedia(state, dir / (name + ".mp4"), name + ".mp4"));
    }
    const MediaId hostId = addMedia(state, dir / "Host mic.wav", "Host mic.wav");
    const MediaId guestId = addMedia(state, dir / "Guest mic.wav", "Guest mic.wav");
    const std::filesystem::path project = dir / "podcast-media.wabcut";

    // Transcripts of the microphones (as the transcription would make them), faces of the cameras (as AI-9 would).
    auto transcripts = std::make_shared<ai::edit::MapTranscriptProvider>();
    transcripts->add(hostId, 0, wordsOf(kScript, 0));
    transcripts->add(guestId, 0, wordsOf(kScript, 1));
    auto faces = std::make_shared<ai::vision::MapSubjectProvider>();
    faces->add(cams[0], facesOf({0}, 0.0));
    faces->add(cams[1], facesOf({1}, 1.5));
    faces->add(cams[2], facesOf({0, 1}, -0.8));
    auto audioServices = audio::edit::makeMediaAudioServices(transcripts, std::make_shared<ai::AnalysisCache>(dir / "cache"));
    auto services = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
        std::make_shared<ai::edit::TranscriptServices>(transcripts), std::make_shared<SpeakerServices>(makeMediaAngleSignals(faces, project)),
        std::make_shared<command::MulticamServices>(std::make_shared<audio::edit::MulticamAudioSync>(audioServices->sync)), audioServices});

    EditSession session(state);
    command::CommandEngine engine(session, ai::edit::editingCommands(), command::EnginePolicy{command::CapabilitySet::all(), 64, services});
    Json media = Json::array();
    for (const MediaId m : cams) {
        media.push_back(m.value);
    }
    media.push_back(hostId.value);
    media.push_back(guestId.value);
    const auto t0 = std::chrono::steady_clock::now();
    const auto result = engine.executeJson(Json{{"command", "workflow.podcast"},
                                                {"params", Json{{"media", media}, {"name", "Episode 1"}, {"minChapterSeconds", 10}, {"run", true}}}});
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    INFO(command::toJson(result).dump(2));
    REQUIRE(result.ok());
    std::set<std::string> done;
    for (const Json& r : result.data["results"]) {
        if (r["status"] == "done") {
            done.insert(r["step"].get<std::string>());
        }
    }
    CHECK(done == std::set<std::string>{"multicam", "sequence", "loudness", "cleanup", "autoswitch", "captions", "chapters"});
    const auto context = ai::workflow::contextFromJson(result.data["context"]);
    const ProjectSnapshot after = session.snapshot();
    const Sequence* source = after->findSequence(context.multicam);
    REQUIRE(source != nullptr);
    REQUIRE(source->multicam);
    // Synced by sound: each camera's clip starts where its recording began on the microphones' clock.
    const auto startOf = [&](std::size_t angle) {
        const Track* t = source->findTrack(source->multicam->angles[angle].video);
        REQUIRE(t != nullptr);
        REQUIRE_FALSE(t->clips.empty());
        return t->clips.front().start() - t->clips.front().sourceIn;
    };
    const double tolerance = 0.05;
    CHECK(std::abs((startOf(1) - startOf(0)).toSeconds() - 1.5) < tolerance);
    CHECK(std::abs((startOf(2) - startOf(0)).toSeconds() + 0.8) < tolerance);
    // Switched to whoever speaks: the host's camera while the host talks, the guest's while the guest does.
    const Sequence* episode = after->findSequence(context.sequence);
    REQUIRE(episode != nullptr);
    std::set<std::size_t> shown;
    for (const Clip& c : episode->videoTracks.front()->clips) {
        shown.insert(c.angles.first());
    }
    CHECK(shown == std::set<std::size_t>{0, 1, 2});
    Json cuts = Json::array();
    for (const Json& r : result.data["results"]) {
        if (r["step"] == "autoswitch") {
            cuts = r["report"]["cuts"];
        }
    }
    std::size_t toHost = 0, toGuest = 0;
    for (const Json& c : cuts) {
        toHost += c["angle"] == 1 && c["reason"] == "speaker" ? 1 : 0;
        toGuest += c["angle"] == 2 && c["reason"] == "speaker" ? 1 : 0;
    }
    CHECK(toHost >= 1);
    CHECK(toGuest >= 1);
    CHECK_FALSE(episode->captionTracks.empty());
    REQUIRE(io::saveProject(*after, project));
    std::printf("[perf] AI-13 podcast workflow on real files (3 cameras + 2 mics, 30 s): %.2f s, %zu cuts\n", elapsed, cuts.size());
#endif
}
