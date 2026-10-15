// Wave-5 review fixes in the voice-over audio layer (A.8):
//   * A5-P2-12: recoverWav() rewrites only takes WavWriter made (their journal, `<file>.journal`): another recorder's WAV
//     with chunks after "data" stays byte-identical, and nothing past what a WAV header can describe (4 GiB) is changed;
//   * A5-P2-11: AudioInput::start() frees the monitor ring it replaces only once a readMonitor() call (the output
//     callback) that may be reading it is over.

#include <wabcut/audio/AudioInput.h>
#include <wabcut/audio/WavWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stop_token>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio;

namespace {

std::filesystem::path scratchDir(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / "wabcut-tests" / "voice-over-recovery" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

std::vector<std::uint8_t> bytesOf(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeBytes(const std::filesystem::path& file, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) {
        b[at + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(v >> (8 * k));
    }
}

void append(std::vector<std::uint8_t>& b, const char* fourcc) { b.insert(b.end(), fourcc, fourcc + 4); }

void append32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) {
        b.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
    }
}

void append16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}

/// Another recorder's WAV: a canonical 16-bit mono header over 100 frames, then a LIST/INFO chunk after the data (as
/// many recorders write their metadata). Its header is right: its length is not its data.
std::vector<std::uint8_t> foreignWav() {
    std::vector<std::uint8_t> b;
    const std::uint32_t dataBytes = 200;
    const std::uint32_t listBytes = 4 + 8 + 6; // "INFO", then "ISFT" with 6 bytes of text
    append(b, "RIFF");
    append32(b, 4 + (8 + 16) + (8 + dataBytes) + (8 + listBytes));
    append(b, "WAVE");
    append(b, "fmt ");
    append32(b, 16);
    append16(b, 1);         // PCM
    append16(b, 1);         // mono
    append32(b, 48000);
    append32(b, 48000 * 2); // byte rate
    append16(b, 2);         // block align
    append16(b, 16);
    append(b, "data");
    append32(b, dataBytes);
    for (std::uint32_t i = 0; i < dataBytes; ++i) {
        b.push_back(static_cast<std::uint8_t>(i * 7));
    }
    append(b, "LIST");
    append32(b, listBytes);
    append(b, "INFO");
    append(b, "ISFT");
    append32(b, 6);
    const char software[6] = {'O', 't', 'h', 'e', 'r', '\0'};
    b.insert(b.end(), software, software + 6);
    return b;
}

AudioInputConfig nullInput(int channels, int sampleRate, double monitorSeconds) {
    AudioInputConfig c;
    c.backend = AudioBackend::Null;
    c.channels = channels;
    c.sampleRate = sampleRate;
    c.ringSeconds = 0.05;
    c.monitorRingSeconds = monitorSeconds;
    return c;
}

} // namespace

TEST_CASE("recoverWav leaves a WAV another recorder wrote byte-identical with a LIST chunk after its data",
          "[audio][voiceover][wav][review]") {
    // Fails today (A5-P2-12): recoverWav took any canonical WAV whose length was not its data for a crashed take and
    // rewrote its sizes to cover the LIST chunk, so the metadata played as audio.
    const auto dir = scratchDir("foreign");
    const auto file = dir / "interview.wav";
    const std::vector<std::uint8_t> original = foreignWav();
    writeBytes(file, original);
    REQUIRE(bytesOf(file) == original);
    CHECK_FALSE(readWavJournal(file));
    const auto recovered = recoverWav(file);
    REQUIRE_FALSE(recovered);
    CHECK(recovered.error().code == ErrorCode::InvalidArgument);
    CHECK(bytesOf(file) == original);
}

TEST_CASE("WavWriter journals its takes; recoverWav repairs a take with its journal and leaves one without it alone",
          "[audio][voiceover][wav][review]") {
    const auto dir = scratchDir("journal");
    const auto file = dir / "take.wav";
    const std::string note = R"({"kind":"test","sequence":7})";
    {
        auto writer = WavWriter::create(file, WavFormat{48000, 1, 24}, 24000, note);
        REQUIRE(writer);
        REQUIRE((*writer)->write(std::vector<float>(4800, 0.25f)));
        REQUIRE((*writer)->finish());
    }
    // The journal is written at create() and outlives finish(): it is what makes the file one recoverWav may change.
    CHECK(wavJournalPath(file) == dir / "take.wav.journal");
    REQUIRE(std::filesystem::exists(wavJournalPath(file)));
    const auto journal = readWavJournal(file);
    REQUIRE(journal);
    CHECK(journal->format == WavFormat{48000, 1, 24});
    CHECK(journal->note == note);

    // The crash: the header as it was at the start, a frame cut in half at the end.
    auto crashed = bytesOf(file);
    put32(crashed, 4, 36);
    put32(crashed, 40, 0);
    crashed.resize(44 + 4000 * 3 + 2);

    // Without its journal it is not known to be a take: left as it is.
    const auto copy = dir / "copy.wav";
    writeBytes(copy, crashed);
    const auto refused = recoverWav(copy);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::InvalidArgument);
    CHECK(bytesOf(copy) == crashed);

    // With it: inspectWav() reports what a repair would do and changes nothing; recoverWav() repairs.
    writeBytes(file, crashed);
    const auto inspected = inspectWav(file);
    REQUIRE(inspected);
    CHECK(inspected->needsRepair);
    CHECK_FALSE(inspected->repaired);
    CHECK(inspected->frames == 4000);
    CHECK(bytesOf(file) == crashed);
    const auto repaired = recoverWav(file);
    REQUIRE(repaired);
    CHECK(repaired->repaired);
    CHECK(repaired->frames == 4000);
    CHECK(std::filesystem::file_size(file) == 44 + 4000 * 3);

    // A journal of another format (a different file put there under the take's name) does not count.
    writeBytes(file, crashed);
    {
        std::ofstream other(wavJournalPath(file), std::ios::binary | std::ios::trunc);
        other << "wabcut-wav-journal 1\n44100 2 16\n" << note;
    }
    CHECK_FALSE(recoverWav(file));
    CHECK(bytesOf(file) == crashed);

    // Removed with the take.
    removeWavJournal(file);
    CHECK_FALSE(std::filesystem::exists(wavJournalPath(file)));
    CHECK_FALSE(readWavJournal(file));
    removeWavJournal(file); // nothing to remove: no error
}

TEST_CASE("recoverWav does not follow a link: the WAV it points to is left as it is", "[audio][voiceover][wav][review]") {
    // Fails today (S5-P2-11): is_regular_file() followed the link, and the target (journal or not) was rewritten.
    const auto dir = scratchDir("link");
    std::filesystem::create_directories(dir / "elsewhere");
    const auto target = dir / "elsewhere" / "take.wav";
    {
        auto writer = WavWriter::create(target, WavFormat{48000, 1, 24}, 24000);
        REQUIRE(writer);
        REQUIRE((*writer)->write(std::vector<float>(1000, 0.25f)));
        REQUIRE((*writer)->finish());
    }
    auto crashed = bytesOf(target);
    put32(crashed, 40, 0);
    writeBytes(target, crashed);
    const auto link = dir / "take.wav";
    try {
        std::filesystem::create_symlink(target, link);
    } catch (const std::filesystem::filesystem_error& e) {
        SKIP("cannot create a symbolic link here (Windows without Developer Mode): " << e.what());
    }
    // A journal next to the link too, so only the link itself stands in the way.
    std::filesystem::copy_file(wavJournalPath(target), wavJournalPath(link));
    CHECK_FALSE(recoverWav(link));
    CHECK_FALSE(inspectWav(link));
    CHECK(bytesOf(target) == crashed);
}

TEST_CASE("recoverWav refuses a take larger than a WAV header can describe (4 GiB) and leaves it as it is",
          "[audio][voiceover][wav][review]") {
    // Fails today (A5-P2-12): the data size wrapped in the 32-bit header field and the "repair" wrote a wrong size.
    const auto dir = scratchDir("huge");
    std::error_code ec;
    const auto space = std::filesystem::space(dir, ec);
    if (ec || space.available < (std::uintmax_t{16} << 30)) {
        SKIP("needs 16 GiB of free disk space for a 4 GiB file");
    }
    const auto file = dir / "long take.wav";
    {
        auto writer = WavWriter::create(file, WavFormat{48000, 1, 16}, 24000);
        REQUIRE(writer);
        REQUIRE((*writer)->write(std::vector<float>(480, 0.1f)));
        REQUIRE((*writer)->finish());
    }
    // A take of more than 4 GiB of samples (12 hours of 48 kHz stereo 32-bit float would be): the file is extended
    // without writing it (the file system does not zero it until it is read).
    const std::uintmax_t huge = 44 + 0x100000000ULL + 2;
    std::filesystem::resize_file(file, huge, ec);
    if (ec) {
        SKIP("the file system cannot hold a 4 GiB file: " << ec.message());
    }
    std::vector<std::uint8_t> head(44);
    {
        std::ifstream in(file, std::ios::binary);
        in.read(reinterpret_cast<char*>(head.data()), 44);
    }
    const auto recovered = recoverWav(file);
    CHECK_FALSE(recovered);
    if (!recovered) {
        CHECK(recovered.error().code == ErrorCode::InvalidArgument);
    }
    std::vector<std::uint8_t> after(44);
    {
        std::ifstream in(file, std::ios::binary);
        in.read(reinterpret_cast<char*>(after.data()), 44);
    }
    CHECK(after == head);
    CHECK(std::filesystem::file_size(file) == huge);
    std::filesystem::remove(file, ec);
    removeWavJournal(file);
}

TEST_CASE("AudioInput start() frees the monitor ring it replaces only after the readMonitor() reading it is over",
          "[audio][voiceover][monitor][review]") {
    // Fails today (A5-P2-11), as a crash: start() freed the monitor ring while the output callback could still be inside
    // readMonitor() copying from it (the input device switched during a pass with monitoring). Here the output thread
    // starts a read of a large ring (6 MB to copy, about a millisecond; the 8 MB block goes straight back to the
    // system when freed, so a read after the free faults) and the input restarts at once, many times. Under ASan the
    // use-after-free is reported every time.
    const AudioInputConfig large = nullInput(2, 384000, 2.0); // 768000 frames, rounded up to 2^20
    const AudioInputConfig small = nullInput(2, 48000, 0.01);
    AudioInput input;
    std::atomic<int> request{0}; // 1: read the monitor once now; 0: done
    std::atomic<std::uint64_t> framesRead{0};
    std::jthread output([&](std::stop_token stop) {
        std::vector<float> out(2 * (std::size_t{1} << 20));
        while (!stop.stop_requested()) {
            if (request.load(std::memory_order_acquire) == 1) {
                framesRead.fetch_add(input.readMonitor(out, 0), std::memory_order_relaxed);
                request.store(0, std::memory_order_release);
            } else {
                std::this_thread::yield();
            }
        }
    });
    input.setMonitorEnabled(true);
    const std::vector<float> block(2 * 768000, 0.5f);
    for (int i = 0; i < 20; ++i) {
        REQUIRE(input.start(large));
        REQUIRE(input.push(block));
        request.store(1, std::memory_order_release);
        // The output callback is copying from the large ring: the device switches under it.
        input.stop();
        REQUIRE(input.start(small));
        while (request.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
        input.stop();
    }
    output.request_stop();
    output.join();
    // Whatever each read got (the old ring when it started first, else the new empty one), a ring that is in use now
    // still works: what is pushed is monitored.
    REQUIRE(input.start(small));
    REQUIRE(input.push(std::vector<float>(2 * 100, 0.25f)));
    std::vector<float> got(2 * 100, 0.0f);
    CHECK(input.readMonitor(got, 0) == 100);
    CHECK(got[0] == 0.25f);
    CHECK(got[199] == 0.25f);
    input.stop();
}
