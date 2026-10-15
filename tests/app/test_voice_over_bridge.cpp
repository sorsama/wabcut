// Wave-5 review fixes in the VoiceOver bridge (A.8), without a window or a device: the playback on the Null engine and
// the bridge on the Null input (WABCUT_AUDIO_BACKEND=null), the test pulling the output and feeding the microphone.
//   * A5-P2-18: a pass opens the audio output itself, in a project with no media file that has sound;
//   * A5-P1-7: the bridge going away during a pass (the app quits) keeps the take and places it;
//   * A5-P2-13: a take that was never placed is offered when its project opens, and placed where it was recorded;
//   * A5-P2-12 / S5-P2-11: the recovery scan leaves other WAVs byte-identical and does not follow links.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "VoiceOverBridge.h"

#include <wabcut/audio/AudioEngine.h>
#include <wabcut/audio/AudioInput.h>
#include <wabcut/audio/WavWriter.h>
#include <wabcut/editor/PlaybackSession.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>

#include <QTemporaryDir>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

constexpr const char* kPicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaa|
    A2     |.........................|
)";

/// A project saved as <temp>/Film.wabcut whose media have no files (nothing is audible), its playback and the
/// VoiceOver bridge, with A1 armed and no count-in, pre- or post-roll: a pass records from the playhead (0) as soon as
/// the transport plays.
struct VoiceOverRig {
    QTemporaryDir dir;
    Timeline tl{kPicture};
    ProjectController controller;
    std::unique_ptr<PlaybackBridge> playback;
    std::unique_ptr<VoiceOverBridge> voiceOver;

    VoiceOverRig() {
        REQUIRE(dir.isValid());
        qputenv("WABCUT_AUDIO_BACKEND", "null"); // the Null engine and the Null input: no device opens
        controller.resetProject(tl.state, tl.seq);
        controller.setProjectFile(projectFile());
        playback = std::make_unique<PlaybackBridge>(&controller);
        voiceOver = std::make_unique<VoiceOverBridge>();
        voiceOver->setController(&controller);
        voiceOver->setPlayback(playback.get());
        voiceOver->setOptions(QVariantMap{{QStringLiteral("countIn"), 0},
                                          {QStringLiteral("prerollSeconds"), 0.0},
                                          {QStringLiteral("postrollSeconds"), 0.0}});
        REQUIRE(voiceOver->setRecordArm(trackKey(tl.track("A1")), true).isEmpty());
    }
    ~VoiceOverRig() {
        voiceOver.reset();
        playback.reset();
        qunsetenv("WABCUT_AUDIO_BACKEND");
    }

    std::filesystem::path folder() const { return std::filesystem::path(dir.path().toStdWString()); }
    std::filesystem::path projectFile() const { return folder() / "Film.wabcut"; }
    std::filesystem::path takesFolder() const { return VoiceOverBridge::takesFolder(projectFile()); }

    /// 10 ms: the output pulled as its device would, 10 ms of voice delivered to the microphone, the events run.
    void step() {
        std::vector<float> out(480 * 2);
        REQUIRE(playback->session()->engine().pull(out));
        REQUIRE(voiceOver->input().push(std::vector<float>(480, 0.25f)));
        spin(10);
    }
    /// Steps until the pass has recorded `seconds` (at most 3 s of steps).
    bool recordFor(double seconds) {
        for (int i = 0; i < 300 && voiceOver->recordedSeconds() < seconds; ++i)
            step();
        return voiceOver->recordedSeconds() >= seconds;
    }
    /// The WAVs anywhere under the project's folder.
    std::vector<std::filesystem::path> wavs() const {
        std::vector<std::filesystem::path> out;
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(folder(), ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->path().extension() == ".wav" && !it->is_symlink())
                out.push_back(it->path());
        }
        return out;
    }
    /// The media item of the project that plays `file`, or null.
    const MediaItem* mediaOf(const std::filesystem::path& file) const {
        for (const MediaPtr& m : controller.snapshot()->media) {
            if (pathFromUtf8(m->locator.path).lexically_normal() == file.lexically_normal())
                return m.get();
        }
        return nullptr;
    }
};

std::vector<std::uint8_t> bytesOf(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeBytes(const std::filesystem::path& file, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int k = 0; k < 4; ++k)
        b[at + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(v >> (8 * k));
}

/// What a crash leaves: the header as it was when the file was made, and a frame cut in half at the end.
void crash(const std::filesystem::path& file) {
    auto b = bytesOf(file);
    REQUIRE(b.size() > 44);
    put32(b, 4, 36);
    put32(b, 40, 0);
    b.push_back(0x7f);
    writeBytes(file, b);
}

/// Another recorder's WAV: 16-bit mono, 100 frames, then a LIST/INFO chunk after the data.
std::vector<std::uint8_t> foreignWav() {
    std::vector<std::uint8_t> b(44, 0);
    std::memcpy(b.data(), "RIFF", 4);
    std::memcpy(b.data() + 8, "WAVEfmt ", 8);
    put32(b, 16, 16);
    b[20] = 1; // PCM
    b[22] = 1; // mono
    put32(b, 24, 48000);
    put32(b, 28, 96000);
    b[32] = 2;
    b[34] = 16;
    std::memcpy(b.data() + 36, "data", 4);
    put32(b, 40, 200);
    for (int i = 0; i < 200; ++i)
        b.push_back(static_cast<std::uint8_t>(i * 7));
    const char list[] = "LIST\x0e\x00\x00\x00INFOISFT\x02\x00\x00\x00X";
    b.insert(b.end(), list, list + sizeof(list)); // 22 bytes: the literal's terminating zero is the text's pad byte
    put32(b, 4, static_cast<std::uint32_t>(b.size() - 8));
    return b;
}

} // namespace

TEST_CASE("VoiceOver starts a pass in a project with no audible media: the pass opens the audio output itself",
          "[app][voiceover][review]") {
    // Fails today (A5-P2-18): the output opened only on play, and only when a media file had sound, so start() said
    // "Recording needs the audio output" over silent media and stills.
    VoiceOverRig r;
    REQUIRE(r.playback->session());
    CHECK_FALSE(r.playback->session()->hasSound());
    const bool started = r.voiceOver->start();
    INFO(r.voiceOver->lastError().toStdString());
    REQUIRE(started);
    CHECK(r.voiceOver->recording());
    CHECK(r.playback->session()->hasSound());
    CHECK(r.voiceOver->inputOpen());
    CHECK(r.voiceOver->targetTrack() == QStringLiteral("A1"));
    REQUIRE(r.wavs().size() == 1);
    CHECK(r.wavs().front().parent_path().lexically_normal() == r.takesFolder().lexically_normal()); // the project's own
    r.voiceOver->cancel();
    CHECK_FALSE(r.voiceOver->recording());
    CHECK(r.wavs().empty()); // cancel() is the one way a take goes
}

TEST_CASE("VoiceOver going away during a pass keeps the take and places it (the app quits mid-take)",
          "[app][voiceover][review]") {
    // Fails today (A5-P1-7): ~VoiceOverBridge() reset the recorder, whose destructor cancel()ed the pass: the WAV was
    // deleted. (Also needs A5-P2-18 to start the pass here.)
    VoiceOverRig r;
    REQUIRE(r.voiceOver->start());
    REQUIRE(r.recordFor(0.2));
    const auto files = r.wavs();
    REQUIRE(files.size() == 1);
    const std::filesystem::path take = files.front();

    r.voiceOver.reset();
    REQUIRE(std::filesystem::exists(take));
    const auto recovered = audio::recoverWav(take);
    REQUIRE(recovered);
    CHECK(recovered->frames > 0);
    CHECK_FALSE(recovered->repaired);
    // The project was still there: the take is in it, on A1.
    const MediaItem* media = r.mediaOf(take);
    REQUIRE(media);
    CHECK(media->recording.has_value());
    CHECK(r.controller.session().undoLabel() == "Voice-over Record");
}

TEST_CASE("A voice-over take that was never placed is offered when its project opens and placed where it was recorded",
          "[app][voiceover][recovery][review]") {
    // Fails today (A5-P2-13): nothing called recoverTakes(), there was nothing for a prompt to show and no way to put a
    // take back where it was recorded; takes of every project in a folder shared one Voice-over folder.
    VoiceOverRig r;
    REQUIRE(r.voiceOver->start());
    REQUIRE(r.recordFor(0.2));
    r.voiceOver->stop();
    const auto files = r.wavs();
    REQUIRE(files.size() == 1);
    const std::filesystem::path take = files.front();
    REQUIRE(r.mediaOf(take));
    // The take leaves the project (as if the app had stopped before placing it), then a crash cuts the file.
    REQUIRE(r.controller.undo());
    REQUIRE_FALSE(r.mediaOf(take));
    crash(take);
    const auto crashed = bytesOf(take);

    int offers = 0;
    QObject::connect(r.voiceOver.get(), &VoiceOverBridge::recoveredTakesChanged, [&] { ++offers; });
    r.controller.setProjectFile(r.folder() / "Other.wabcut"); // another project...
    CHECK(r.voiceOver->recoveredTakes().isEmpty());
    r.controller.setProjectFile(r.projectFile()); // ...and this one again
    const QVariantList rows = r.voiceOver->recoveredTakes();
    REQUIRE(rows.size() == 1);
    CHECK(offers >= 1);
    const QVariantMap row = rows.front().toMap();
    CHECK(row.value(QStringLiteral("interrupted")).toBool());
    CHECK(row.value(QStringLiteral("seconds")).toDouble() >= 0.19);
    CHECK(row.value(QStringLiteral("track")).toString() == QStringLiteral("A1"));
    CHECK(row.value(QStringLiteral("placeable")).toBool());
    CHECK(bytesOf(take) == crashed); // listing is read only (S5-P2-11)

    CHECK(r.voiceOver->placeRecoveredTake(row.value(QStringLiteral("path")).toString()).isEmpty());
    CHECK(r.voiceOver->recoveredTakes().isEmpty());
    const MediaItem* media = r.mediaOf(take);
    REQUIRE(media);
    CHECK(media->recording.has_value());
    const SequencePtr seq = r.controller.currentSequence();
    const Track* a1 = seq->findTrack(r.tl.track("A1"));
    REQUIRE(a1);
    bool onA1 = false;
    for (const Clip& clip : a1->clips)
        onA1 = onA1 || (clip.media == media->id && clip.timelineRange.start == Ticks{});
    CHECK(onA1); // at the punch-in, on the armed track
    const auto repaired = audio::inspectWav(take);
    REQUIRE(repaired);
    CHECK_FALSE(repaired->needsRepair); // repaired when it was placed
}

TEST_CASE("The voice-over recovery scan leaves a WAV another recorder wrote byte-identical with its LIST chunk",
          "[app][voiceover][recovery][review]") {
    // Fails today (A5-P2-12, S5-P2-11): recoverTakes() "repaired" every unused WAV in <project folder>/Voice-over, so
    // the LIST chunk became samples.
    VoiceOverRig r;
    const std::vector<std::uint8_t> foreign = foreignWav();
    const std::string name = "Fixture A1 20261012-093000.wav"; // even named as a take
    for (const std::filesystem::path& folder : {r.folder() / "Voice-over", r.takesFolder()}) {
        std::filesystem::create_directories(folder);
        writeBytes(folder / name, foreign);
    }
    const QVariantList listed = r.voiceOver->recoverTakes();
    CHECK(listed.isEmpty());
    CHECK(r.voiceOver->recoveredTakes().isEmpty());
    CHECK(bytesOf(r.folder() / "Voice-over" / name) == foreign);
    CHECK(bytesOf(r.takesFolder() / name) == foreign);
}

TEST_CASE("The voice-over recovery scan does not follow a link to a WAV outside the folder",
          "[app][voiceover][recovery][review]") {
    // Fails today (S5-P2-11): the scan followed links (is_regular_file) and rewrote the file they pointed to.
    VoiceOverRig r;
    const std::string name = "Fixture A1 20261012-093000.wav";
    const std::filesystem::path elsewhere = r.folder() / "elsewhere";
    std::filesystem::create_directories(elsewhere);
    const std::filesystem::path target = elsewhere / name;
    // A take of this project in every other respect: its journal says where it goes.
    const std::string note = R"({"kind":"wabcut.voiceOverTake","sequence":)" + std::to_string(r.tl.seq.value) +
                             R"(,"track":)" + std::to_string(r.tl.track("A1").value) + R"(,"trackLabel":"A1","punchIn":0})";
    {
        auto writer = audio::WavWriter::create(target, audio::WavFormat{48000, 1, 24}, 24000, note);
        REQUIRE(writer);
        REQUIRE((*writer)->write(std::vector<float>(4800, 0.25f)));
        REQUIRE((*writer)->finish());
    }
    crash(target);
    const auto before = bytesOf(target);
    const std::filesystem::path legacy = r.folder() / "Voice-over";
    std::filesystem::create_directories(legacy);
    std::filesystem::create_directories(r.takesFolder());
    try {
        for (const std::filesystem::path& folder : {legacy, r.takesFolder()}) {
            std::filesystem::create_symlink(target, folder / name);
            std::filesystem::copy_file(audio::wavJournalPath(target), audio::wavJournalPath(folder / name));
        }
    } catch (const std::filesystem::filesystem_error& e) {
        SKIP("cannot create a symbolic link here (Windows without Developer Mode): " << e.what());
    }
    CHECK(r.voiceOver->recoverTakes().isEmpty());
    CHECK(bytesOf(target) == before);
    // The same take as a plain file in the folder is offered (only the link stood in the way).
    std::filesystem::remove(r.takesFolder() / name);
    std::filesystem::copy_file(target, r.takesFolder() / name);
    CHECK(r.voiceOver->recoverTakes().size() == 1);
    CHECK(bytesOf(target) == before);
}
