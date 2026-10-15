#include "TestMedia.h"

#include <wabcut/media/AudioDecoder.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <vector>

using namespace wabcut;
using namespace wabcut::media;
using namespace wabcut::media::test;

namespace {

std::unique_ptr<AudioDecoder> openOrSkip(std::string_view name, Rate rate = kRate48kHz, int channels = 2) {
    if (!haveMedia(name)) {
        SKIP("test clip " << name << " was not generated");
    }
    auto decoder = AudioDecoder::open(mediaPath(name), -1, rate, channels);
    INFO(name << ": " << (decoder ? "" : decoder.error().message));
    REQUIRE(decoder.has_value());
    return std::move(*decoder);
}

// ramp_48k.wav: sample n is L = ((n mod 65536) - 32768) / 32768, R = (((n + 12345) mod 65536) - 32768) /
// 32768.
float rampLeft(std::int64_t n) {
    return static_cast<float>(static_cast<int>(n % 65536) - 32768) / 32768.0f;
}
float rampRight(std::int64_t n) {
    return rampLeft(n + 12345);
}

/// Reads `frames` stereo samples at sample position `start` and checks every value against the ramp exactly.
void checkRamp(AudioDecoder& decoder, std::int64_t start, std::int64_t frames,
               Ticks offsetWithinSample = {}) {
    std::vector<float> buf(static_cast<std::size_t>(frames * 2), -9.0f);
    auto got = decoder.read(samplesToTicks(start, kRate48kHz) + offsetWithinSample, buf);
    REQUIRE(got.has_value());
    const std::int64_t total = 5 * 48000;
    const std::int64_t expectedReal = std::clamp<std::int64_t>(std::min(start + frames, total), 0, total) -
                                      std::clamp<std::int64_t>(start, 0, total);
    const auto realFrames = std::max<std::int64_t>(expectedReal, 0);
    CHECK(static_cast<std::int64_t>(got->framesFromStream) == realFrames);
    CHECK(static_cast<std::int64_t>(*got) == realFrames); // transitional size_t conversion
    // The decoded frames start at max(start, 0); everything before them is leading silence.
    CHECK(static_cast<std::int64_t>(got->leadingSilence) ==
          (realFrames > 0 ? std::max<std::int64_t>(0, -start) : (start + frames <= 0 ? frames : 0)));
    CHECK(got->endOfStream == (start + frames > total));
    std::int64_t mismatches = 0;
    for (std::int64_t i = 0; i < frames; ++i) {
        const std::int64_t n = start + i;
        const bool real = n >= 0 && n < total;
        const float l = real ? rampLeft(n) : 0.0f;
        const float r = real ? rampRight(n) : 0.0f;
        if (buf[static_cast<std::size_t>(2 * i)] != l || buf[static_cast<std::size_t>(2 * i + 1)] != r) {
            if (mismatches++ < 5) {
                UNSCOPED_INFO("sample " << n << ": got " << buf[static_cast<std::size_t>(2 * i)] << "/"
                                        << buf[static_cast<std::size_t>(2 * i + 1)] << ", expected " << l
                                        << "/" << r);
            }
        }
    }
    CHECK(mismatches == 0);
}

} // namespace

TEST_CASE("Audio: WAV decodes sample-exactly, sequentially and after seeks", "[media][audio]") {
    auto decoder = openOrSkip("ramp_48k.wav");
    CHECK(decoder->outputRate() == kRate48kHz);
    CHECK(decoder->outputChannels() == 2);
    CHECK(decoder->streamInfo().codec == "pcm_s16le");

    SECTION("sequential blocks do not seek") {
        checkRamp(*decoder, 0, 1000);
        const auto seeks = decoder->stats().seeks;
        for (std::int64_t start = 1000; start < 3 * 48000; start += 1000) {
            checkRamp(*decoder, start, 1000);
        }
        CHECK(decoder->stats().seeks == seeks);
    }
    SECTION("random access") {
        for (const std::int64_t start :
             {200000LL, 17LL, 65530LL, 131072LL, 1LL, 239000LL, 100000LL, 99999LL}) {
            checkRamp(*decoder, start, 777);
        }
    }
    SECTION("a time inside a sample selects that sample") {
        checkRamp(*decoder, 12345, 64, Ticks{kRate48kHz.ticksPerUnit() - 1});
        checkRamp(*decoder, 54321, 64, Ticks{1});
    }
    SECTION("silence before the start and past the end") {
        checkRamp(*decoder, -500, 1000);
        checkRamp(*decoder, 5 * 48000 - 300, 1000);
        checkRamp(*decoder, 6 * 48000, 100);
        checkRamp(*decoder, 10, 100); // and back
    }
}

TEST_CASE("Audio: AAC reads after a seek equal a sequential decode", "[media][audio]") {
    auto decoder = openOrSkip("h264_1080p60_aac.mp4");
    CHECK(decoder->streamInfo().codec == "aac");
    const std::int64_t total = 6 * 48000;
    std::vector<float> sequential(static_cast<std::size_t>(total * 2));
    for (std::int64_t pos = 0; pos < total; pos += 4800) {
        auto got = decoder->read(samplesToTicks(pos, kRate48kHz),
                                 std::span(sequential).subspan(static_cast<std::size_t>(pos * 2), 4800 * 2));
        REQUIRE(got.has_value());
        CHECK(*got == 4800);
    }
    // 0.5 sin(2 pi 440 t) on the left channel: RMS 0.354 (the AAC encoder is lossy, so no sample-wise check).
    double energy = 0.0;
    for (std::int64_t n = 48000; n < 5 * 48000; ++n) {
        energy +=
            double{sequential[static_cast<std::size_t>(2 * n)]} * sequential[static_cast<std::size_t>(2 * n)];
    }
    CHECK(std::abs(std::sqrt(energy / (4 * 48000)) - 0.5 / std::numbers::sqrt2) < 0.01);

    const auto readAt = [](AudioDecoder& d, std::int64_t start) {
        std::vector<float> buf(4096 * 2);
        auto got = d.read(samplesToTicks(start, kRate48kHz), buf);
        REQUIRE(got.has_value());
        REQUIRE(*got == 4096);
        return buf;
    };
    auto fresh = openOrSkip("h264_1080p60_aac.mp4");
    for (const std::int64_t start : {123456LL, 48000LL, 1LL, 250000LL, 77777LL, 0LL, 200000LL}) {
        const std::vector<float> buf = readAt(*fresh, start);
        float maxDiff = 0.0f;
        for (std::size_t i = 0; i < buf.size(); ++i) {
            maxDiff =
                std::max(maxDiff, std::abs(buf[i] - sequential[static_cast<std::size_t>(start * 2) + i]));
        }
        INFO("seek to sample " << start);
        // Same sample grid; only AAC's noise-substitution generator differs from the sequential pass (~ -100
        // dBFS).
        CHECK(maxDiff <= 1e-4f);
        if (start <= 1) {
            CHECK(maxDiff == 0.0f); // from the stream start both passes decode identically
        }
    }
    CHECK(fresh->stats().seeks >= 5);

    // What comes out after a seek depends on the seek point only, not on what was played before.
    auto a = openOrSkip("h264_1080p60_aac.mp4");
    auto b = openOrSkip("h264_1080p60_aac.mp4");
    (void)readAt(*a, 200000);
    (void)readAt(*a, 30000);
    CHECK(readAt(*a, 150000) == readAt(*b, 150000));
}

TEST_CASE("Audio: resampling 48 kHz to 44.1 kHz keeps the time base", "[media][audio]") {
    auto decoder = openOrSkip("sine_48k.wav", kRate44_1kHz, 2);
    // sine_48k.wav: L = 0.5 sin(2 pi 1000 t), R = 0.25 sin(2 pi 1500 t). Output sample m is at t = m / 44100.
    const auto checkSine = [&](AudioDecoder& d, std::int64_t start, std::int64_t frames) {
        std::vector<float> buf(static_cast<std::size_t>(frames * 2));
        auto got = d.read(samplesToTicks(start, kRate44_1kHz), buf);
        REQUIRE(got.has_value());
        CHECK(static_cast<std::int64_t>(*got) == frames);
        double maxError = 0.0;
        for (std::int64_t i = std::max<std::int64_t>(0, 32 - start); i < frames; ++i) {
            // (The first 32 output samples of the stream are skipped: the resampler's filter has no input
            // before t = 0 to work with, an edge effect of the file, not of the decoder.)
            const double t = static_cast<double>(start + i) / 44100.0;
            const double l = 0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * t);
            const double r = 0.25 * std::sin(2.0 * std::numbers::pi * 1500.0 * t);
            maxError = std::max({maxError, std::abs(buf[static_cast<std::size_t>(2 * i)] - l),
                                 std::abs(buf[static_cast<std::size_t>(2 * i + 1)] - r)});
        }
        INFO("start " << start << ": max error " << maxError);
        // A one-sample misalignment would be ~0.07; resampler ripple and 16-bit quantisation are far below
        // 1e-3.
        CHECK(maxError < 1e-3);
        return buf;
    };
    const auto sequentialA = checkSine(*decoder, 0, 44100); // includes the stream start
    const auto sequentialB = checkSine(*decoder, 44100, 44100);
    const auto seeked = checkSine(*decoder, 100000, 2000);
    checkSine(*decoder, 3, 500);

    // Reading after a seek lands on the same output grid as a sequential decode.
    auto fresh = openOrSkip("sine_48k.wav", kRate44_1kHz, 2);
    std::vector<float> again(2000 * 2);
    REQUIRE(fresh->read(samplesToTicks(44100 + 777, kRate44_1kHz), again).has_value());
    float maxDiff = 0.0f;
    for (std::size_t i = 0; i < again.size(); ++i) {
        maxDiff = std::max(maxDiff, std::abs(again[i] - sequentialB[777 * 2 + i]));
    }
    CHECK(maxDiff < 1e-5f);
    CHECK(seeked.size() == 4000);
    CHECK(sequentialA.size() == 88200);
}

TEST_CASE("Audio: channel mapping and other codecs", "[media][audio]") {
    SECTION("stereo to mono downmix") {
        auto decoder = openOrSkip("sine_48k.wav", kRate48kHz, 1);
        std::vector<float> buf(4800);
        auto got = decoder->read(Ticks::fromSeconds(1), buf);
        REQUIRE(got.has_value());
        CHECK(*got == 4800);
        float peak = 0.0f;
        for (const float v : buf) {
            peak = std::max(peak, std::abs(v));
        }
        CHECK(peak > 0.2f);
        CHECK(peak < 1.0f);
    }
    SECTION("PCM s24le in MOV") {
        auto decoder = openOrSkip("prores_422.mov");
        CHECK(decoder->streamInfo().codec == "pcm_s24le");
        std::vector<float> buf(48000 * 2);
        auto got = decoder->read(Ticks{0}, buf);
        REQUIRE(got.has_value());
        CHECK(*got == 48000);
        // 0.5 sin(2 pi 440 t) on the left channel, exactly representable timing at 48 kHz.
        for (int n : {0, 1, 100, 12345, 47999}) {
            const double expected = 0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * n / 48000.0);
            CHECK(std::abs(buf[static_cast<std::size_t>(2 * n)] - expected) < 1e-4);
        }
    }
}

TEST_CASE("Audio: errors", "[media][audio]") {
    auto missing = AudioDecoder::open(mediaPath("missing.wav"), -1, kRate48kHz, 2);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::NotFound);

    if (!haveMedia("vfr.mp4") || !haveMedia("ramp_48k.wav")) {
        SKIP("test clips were not generated");
    }
    auto videoOnly = AudioDecoder::open(mediaPath("vfr.mp4"), -1, kRate48kHz, 2);
    REQUIRE_FALSE(videoOnly.has_value());
    CHECK(videoOnly.error().code == ErrorCode::NotFound);

    CHECK(AudioDecoder::open(mediaPath("ramp_48k.wav"), -1, Rate{0, 1}, 2).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(AudioDecoder::open(mediaPath("ramp_48k.wav"), -1, Rate{44101, 2}, 2).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(AudioDecoder::open(mediaPath("ramp_48k.wav"), -1, kRate48kHz, 0).error().code ==
          ErrorCode::InvalidArgument);

    auto decoder = AudioDecoder::open(mediaPath("ramp_48k.wav"), -1, kRate48kHz, 2);
    REQUIRE(decoder.has_value());
    std::vector<float> odd(3);
    auto bad = (*decoder)->read(Ticks{0}, odd);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == ErrorCode::InvalidArgument);
    std::vector<float> empty;
    CHECK((*decoder)->read(Ticks{0}, empty).value() == 0);
}

TEST_CASE("Audio: channel layouts and explicit read results", "[media][audio]") {
    CHECK(ChannelLayout::mono().channels == 1);
    CHECK(ChannelLayout::stereo().name() == "stereo");
    CHECK(ChannelLayout::surround51().name() == "5.1");
    CHECK(ChannelLayout::surround71().channels == 8);
    CHECK(ChannelLayout::fromCount(3).name() == "3 channels");
    CHECK_FALSE(ChannelLayout::fromCount(0).isValid());
    CHECK_FALSE(ChannelLayout::fromCount(17).isValid());

    if (!haveMedia("sine_48k.wav")) {
        SKIP("test clip was not generated");
    }
    SECTION("5.1 output from stereo input") {
        auto decoder =
            AudioDecoder::open(mediaPath("sine_48k.wav"), -1, kRate48kHz, ChannelLayout::surround51());
        REQUIRE(decoder.has_value());
        CHECK((*decoder)->outputLayout() == ChannelLayout::surround51());
        CHECK((*decoder)->outputChannels() == 6);
        std::vector<float> buf(4800 * 6);
        auto got = (*decoder)->read(Ticks::fromSeconds(1), buf);
        REQUIRE(got.has_value());
        CHECK(got->framesFromStream == 4800);
        float front = 0.0f;
        float lfe = 0.0f;
        for (std::size_t i = 0; i < 4800; ++i) {
            front = std::max(front, std::abs(buf[i * 6])); // L carries the left input
            lfe = std::max(lfe, std::abs(buf[i * 6 + 3])); // nothing is routed to the LFE
        }
        CHECK(front > 0.4f);
        CHECK(lfe == 0.0f);
    }
    SECTION("end of stream is explicit and sticky") {
        auto decoder = AudioDecoder::open(mediaPath("sine_48k.wav"), -1, kRate48kHz, ChannelLayout::stereo());
        REQUIRE(decoder.has_value());
        std::vector<float> buf(1000 * 2);
        auto tail = (*decoder)->read(samplesToTicks(5 * 48000 - 400, kRate48kHz), buf);
        REQUIRE(tail.has_value());
        CHECK(tail->framesFromStream == 400);
        CHECK(tail->leadingSilence == 0);
        CHECK(tail->endOfStream);
        auto after = (*decoder)->read(samplesToTicks(5 * 48000 + 600, kRate48kHz), buf);
        REQUIRE(after.has_value());
        CHECK(after->framesFromStream == 0);
        CHECK(after->endOfStream);
        auto inside = (*decoder)->read(Ticks::fromSeconds(2), buf);
        REQUIRE(inside.has_value());
        CHECK_FALSE(inside->endOfStream);
        CHECK(inside->framesFromStream == 1000);
    }
}
