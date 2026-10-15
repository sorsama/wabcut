#include "TestMedia.h"

#include <wabcut/media/WaveformPeaks.h>

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <vector>

using namespace wabcut;
using namespace wabcut::media;
using namespace wabcut::media::test;

namespace {

void requireMedia(std::string_view name) {
    if (!haveMedia(name)) {
        SKIP("test clip " << name << " was not generated");
    }
}

// ramp_48k.wav sample n (see gen_test_media.py) as the decoder delivers it: int16 / 32768.
float rampValue(std::int64_t n, int channel) {
    const std::int64_t k = channel == 0 ? n : n + 12345;
    return static_cast<float>(static_cast<int>(k % 65536) - 32768) / 32768.0f;
}

std::int16_t qMin(float v) {
    return static_cast<std::int16_t>(std::clamp(std::floor(v * 32767.0f), -32768.0f, 32767.0f));
}
std::int16_t qMax(float v) {
    return static_cast<std::int16_t>(std::clamp(std::ceil(v * 32767.0f), -32768.0f, 32767.0f));
}

bool samePeaks(const WaveformPeaks& a, const WaveformPeaks& b) {
    if (a.sampleRate != b.sampleRate || a.channels != b.channels || a.sampleCount != b.sampleCount ||
        a.levels.size() != b.levels.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.levels.size(); ++i) {
        if (a.levels[i].samplesPerBin != b.levels[i].samplesPerBin ||
            a.levels[i].binCount != b.levels[i].binCount || a.levels[i].peaks != b.levels[i].peaks) {
            return false;
        }
    }
    return true;
}

/// Checks that `partial` is an exact prefix of `whole`: whole finest bins, each level holding only complete bins,
/// each equal to the final one.
void checkPrefix(const WaveformPeaks& partial, const WaveformPeaks& whole) {
    REQUIRE(partial.levels.size() == whole.levels.size());
    CHECK(partial.sampleRate == whole.sampleRate);
    CHECK(partial.channels == whole.channels);
    CHECK_FALSE(partial.fromCache);
    CHECK(partial.sampleCount > 0);
    CHECK(partial.sampleCount <= whole.sampleCount);
    CHECK(partial.sampleCount % whole.levels.front().samplesPerBin == 0);
    for (std::size_t li = 0; li < whole.levels.size(); ++li) {
        const PeakLevel& p = partial.levels[li];
        const PeakLevel& f = whole.levels[li];
        INFO("level " << li << ": " << p.binCount << " of " << f.binCount << " bins");
        CHECK(p.samplesPerBin == f.samplesPerBin);
        CHECK(p.binCount == partial.sampleCount / p.samplesPerBin);
        REQUIRE(p.binCount <= f.binCount);
        REQUIRE(p.peaks.size() == static_cast<std::size_t>(p.binCount * partial.channels * 2));
        CHECK(std::equal(p.peaks.begin(), p.peaks.end(), f.peaks.begin()));
    }
}

/// Every snapshot a computation delivers (zero interval: one per decoded block).
struct Snapshots {
    std::vector<std::shared_ptr<const WaveformPeaks>> seen;
    WaveformPartials partials() {
        WaveformPartials p;
        p.interval = std::chrono::milliseconds(0);
        p.onPartial = [this](std::shared_ptr<const WaveformPeaks> s) { seen.push_back(std::move(s)); };
        return p;
    }
};

} // namespace

TEST_CASE("Peaks: exact min/max pyramid of a known signal", "[media][peaks]") {
    requireMedia("ramp_48k.wav");
    auto peaks = computeWaveformPeaks(mediaPath("ramp_48k.wav"));
    REQUIRE(peaks.has_value());
    CHECK(peaks->sampleRate == kRate48kHz);
    CHECK(peaks->channels == 2);
    CHECK(peaks->sampleCount == 240000);
    REQUIRE(peaks->levels.size() == 3);
    CHECK(peaks->levels[0].samplesPerBin == 256);
    CHECK(peaks->levels[1].samplesPerBin == 2048);
    CHECK(peaks->levels[2].samplesPerBin == 16384);
    CHECK(peaks->levels[0].binCount == 938); // ceil(240000 / 256)
    CHECK(peaks->levels[1].binCount == 118);
    CHECK(peaks->levels[2].binCount == 15);
    for (const PeakLevel& level : peaks->levels) {
        CHECK(level.peaks.size() == static_cast<std::size_t>(level.binCount * 2 * 2));
    }

    // Every finest bin equals the brute-force min/max over its samples (the last one is partial).
    const PeakLevel& fine = peaks->levels[0];
    std::int64_t mismatches = 0;
    for (std::int64_t bin = 0; bin < fine.binCount; ++bin) {
        for (int c = 0; c < 2; ++c) {
            float lo = 1.0f;
            float hi = -1.0f;
            for (std::int64_t n = bin * 256; n < std::min<std::int64_t>((bin + 1) * 256, 240000); ++n) {
                lo = std::min(lo, rampValue(n, c));
                hi = std::max(hi, rampValue(n, c));
            }
            if (fine.minAt(bin, c, 2) != qMin(lo) || fine.maxAt(bin, c, 2) != qMax(hi)) {
                ++mismatches;
            }
        }
    }
    CHECK(mismatches == 0);

    // Coarser levels reduce the finer one exactly.
    for (std::size_t li = 1; li < peaks->levels.size(); ++li) {
        const PeakLevel& coarse = peaks->levels[li];
        const std::int64_t factor = coarse.samplesPerBin / fine.samplesPerBin;
        for (std::int64_t bin = 0; bin < coarse.binCount; ++bin) {
            for (int c = 0; c < 2; ++c) {
                std::int16_t lo = 32767;
                std::int16_t hi = -32768;
                for (std::int64_t f = bin * factor; f < std::min(fine.binCount, (bin + 1) * factor); ++f) {
                    lo = std::min(lo, fine.minAt(f, c, 2));
                    hi = std::max(hi, fine.maxAt(f, c, 2));
                }
                CHECK(coarse.minAt(bin, c, 2) == lo);
                CHECK(coarse.maxAt(bin, c, 2) == hi);
            }
        }
    }

    CHECK(peaks->levelFor(100.0)->samplesPerBin == 256);
    CHECK(peaks->levelFor(4096.0)->samplesPerBin == 2048);
    CHECK(peaks->levelFor(1e9)->samplesPerBin == 16384);
}

TEST_CASE("Peaks: sine amplitude and compressed audio", "[media][peaks]") {
    requireMedia("sine_48k.wav");
    auto sine = computeWaveformPeaks(mediaPath("sine_48k.wav"));
    REQUIRE(sine.has_value());
    const PeakLevel& coarse = sine->levels.back();
    CHECK(coarse.maxAt(3, 0, 2) >= 16300); // 0.5 full scale
    CHECK(coarse.maxAt(3, 0, 2) <= 16400);
    CHECK(coarse.minAt(3, 0, 2) <= -16300);
    CHECK(coarse.maxAt(3, 1, 2) >= 8150); // 0.25 full scale
    CHECK(coarse.maxAt(3, 1, 2) <= 8250);

    requireMedia("h264_1080p60_aac.mp4");
    auto aac = computeWaveformPeaks(mediaPath("h264_1080p60_aac.mp4"));
    REQUIRE(aac.has_value());
    CHECK(aac->sampleRate == kRate48kHz);
    CHECK(aac->channels == 2);
    CHECK(std::abs(aac->sampleCount - 480000) <= 1024); // 10 s; AAC priming is trimmed
}

TEST_CASE("Peaks: disk cache, background job, validation", "[media][peaks]") {
    requireMedia("ramp_48k.wav");
    const auto cacheDir = scratchDir("peaks-cache");
    WaveformPeaksService service(cacheDir);

    auto job = service.request(mediaPath("ramp_48k.wav"));
    const auto& first = job.wait();
    REQUIRE(first.has_value());
    CHECK_FALSE(first->fromCache);
    CHECK(job.progress() == 1.0f);
    const auto file = service.cacheFileFor(mediaPath("ramp_48k.wav"), {});
    CHECK(std::filesystem::is_regular_file(file));
    CHECK(file.parent_path() == cacheDir / "peaks");

    auto second = service.getOrCompute(mediaPath("ramp_48k.wav"));
    REQUIRE(second.has_value());
    CHECK(second->fromCache);
    CHECK(samePeaks(*first, *second));

    // Deterministic: recomputing from scratch gives the same bytes.
    auto recomputed = computeWaveformPeaks(mediaPath("ramp_48k.wav"));
    REQUIRE(recomputed.has_value());
    CHECK(samePeaks(*first, *recomputed));

    WaveformOptions bad;
    bad.samplesPerBin = {256, 1000};
    CHECK(computeWaveformPeaks(mediaPath("ramp_48k.wav"), bad).error().code == ErrorCode::InvalidArgument);
    bad.samplesPerBin = {};
    CHECK(service.getOrCompute(mediaPath("ramp_48k.wav"), bad).error().code == ErrorCode::InvalidArgument);
    CHECK(service.getOrCompute(mediaPath("missing.wav")).error().code == ErrorCode::NotFound);
    if (haveMedia("vfr.mp4")) {
        CHECK(computeWaveformPeaks(mediaPath("vfr.mp4")).error().code ==
              ErrorCode::NotFound); // no audio stream
    }
}

TEST_CASE("Peaks: partial snapshots are in-order exact prefixes of the final pyramid", "[media][peaks]") {
    requireMedia("ramp_48k.wav");
    // 5 s at 48 kHz = 240000 samples: four blocks of 65536, so three snapshots (none for the last block, the result
    // follows it).
    Snapshots snapshots;
    auto peaks = computeWaveformPeaks(mediaPath("ramp_48k.wav"), {}, {}, {}, snapshots.partials());
    REQUIRE(peaks.has_value());
    REQUIRE(snapshots.seen.size() == 3);
    std::int64_t previous = 0;
    for (const auto& s : snapshots.seen) {
        REQUIRE(s != nullptr);
        CHECK(s->sampleCount > previous); // in order, growing
        previous = s->sampleCount;
        checkPrefix(*s, *peaks);
    }
    CHECK(snapshots.seen.front()->sampleCount == 65536);
    CHECK(snapshots.seen.front()->levels[1].binCount == 32);
    CHECK(snapshots.seen.front()->levels[2].binCount == 4); // only complete 16384-sample bins
    CHECK(snapshots.seen.back()->sampleCount == 3 * 65536);
    CHECK(snapshots.seen.back()->sampleCount < peaks->sampleCount);

    // The result does not depend on the snapshots.
    auto plain = computeWaveformPeaks(mediaPath("ramp_48k.wav"));
    REQUIRE(plain.has_value());
    CHECK(samePeaks(*peaks, *plain));

    // Compressed audio (AAC priming, a stream start of its own): the same rules.
    if (haveMedia("h264_1080p60_aac.mp4")) {
        Snapshots aac;
        auto full = computeWaveformPeaks(mediaPath("h264_1080p60_aac.mp4"), {}, {}, {}, aac.partials());
        REQUIRE(full.has_value());
        CHECK(aac.seen.size() >= 6); // 10 s = 7.3 blocks
        for (const auto& s : aac.seen) {
            checkPrefix(*s, *full);
        }
        auto again = computeWaveformPeaks(mediaPath("h264_1080p60_aac.mp4"));
        REQUIRE(again.has_value());
        CHECK(samePeaks(*full, *again));
    }
}

TEST_CASE("Peaks: service snapshots arrive before the result and leave the cache file byte-identical",
          "[media][peaks]") {
    requireMedia("ramp_48k.wav");
    const std::string path = mediaPath("ramp_48k.wav");
    const auto bytesOf = [](const std::filesystem::path& file) {
        std::ifstream in(file, std::ios::binary);
        return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };

    // Reference: the same request without snapshots.
    const auto plainDir = scratchDir("peaks-plain");
    std::filesystem::path plainFile;
    {
        WaveformPeaksService plain(plainDir);
        REQUIRE(plain.request(path).wait().has_value());
        plainFile = plain.cacheFileFor(path, {});
    }

    std::mutex mutex;
    std::vector<std::int64_t> order; // the snapshots' sample counts, then -1 for the result
    const auto progressiveDir = scratchDir("peaks-progressive");
    std::filesystem::path progressiveFile;
    std::optional<WaveformPeaks> computed;
    {
        WaveformPeaksService service(progressiveDir, 3);
        WaveformPartials partials;
        partials.interval = std::chrono::milliseconds(0);
        partials.onPartial = [&](std::shared_ptr<const WaveformPeaks> s) {
            std::lock_guard lock(mutex);
            order.push_back(s->sampleCount);
        };
        auto job = service.request(
            path, {},
            [&](const std::expected<WaveformPeaks, Error>& r) {
                std::lock_guard lock(mutex);
                order.push_back(r ? -1 : -2);
            },
            partials);
        const auto& result = job.wait();
        REQUIRE(result.has_value());
        CHECK_FALSE(result->fromCache);
        computed = *result;
        progressiveFile = service.cacheFileFor(path, {});
    } // joins the workers: every callback has run
    const std::vector<std::int64_t> expectedOrder{65536, 2 * 65536, 3 * 65536, -1};
    CHECK(order == expectedOrder);
    REQUIRE(std::filesystem::is_regular_file(plainFile));
    REQUIRE(std::filesystem::is_regular_file(progressiveFile));
    CHECK(bytesOf(plainFile) == bytesOf(progressiveFile));

    // A cache hit completes at once: no snapshots.
    WaveformPeaksService again(progressiveDir);
    std::atomic<int> snapshots{0};
    WaveformPartials counting;
    counting.interval = std::chrono::milliseconds(0);
    counting.onPartial = [&](std::shared_ptr<const WaveformPeaks>) { ++snapshots; };
    auto cached = again.request(path, {}, {}, counting).wait();
    REQUIRE(cached.has_value());
    CHECK(cached->fromCache);
    CHECK(samePeaks(*cached, *computed));
    CHECK(snapshots.load() == 0);
}

TEST_CASE("Peaks: a request cancelled mid-way stops after its snapshot and writes no cache file", "[media][peaks]") {
    requireMedia("ramp_48k.wav");
    const std::string path = mediaPath("ramp_48k.wav");

    // The free function: a stop request made from the first snapshot ends the computation.
    {
        std::stop_source stop;
        int seen = 0;
        WaveformPartials partials;
        partials.interval = std::chrono::milliseconds(0);
        partials.onPartial = [&](std::shared_ptr<const WaveformPeaks>) {
            ++seen;
            stop.request_stop();
        };
        auto r = computeWaveformPeaks(path, {}, stop.get_token(), {}, partials);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::Cancelled);
        CHECK(seen == 1);
    }

    // The service: Job::cancel() while the worker is inside the first snapshot.
    std::mutex mutex;
    std::condition_variable changed;
    bool inSnapshot = false;
    bool released = false;
    int snapshots = 0;
    WaveformPartials partials;
    partials.interval = std::chrono::milliseconds(0);
    partials.onPartial = [&](std::shared_ptr<const WaveformPeaks>) {
        std::unique_lock lock(mutex);
        ++snapshots;
        inSnapshot = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
    };
    WaveformPeaksService service(scratchDir("peaks-cancel"));
    auto job = service.request(path, {}, {}, partials);
    bool reached = false;
    {
        std::unique_lock lock(mutex);
        reached = changed.wait_for(lock, std::chrono::seconds(20), [&] { return inSnapshot; });
    }
    job.cancel();
    {
        std::lock_guard lock(mutex);
        released = true; // always, so the worker never stays blocked
    }
    changed.notify_all();
    REQUIRE(reached);
    const auto& result = job.wait();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::Cancelled);
    {
        std::lock_guard lock(mutex);
        CHECK(snapshots == 1);
    }
    CHECK_FALSE(std::filesystem::exists(service.cacheFileFor(path, {})));
}
