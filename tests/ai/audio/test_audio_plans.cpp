// A.4: cutting long takes into helper requests and putting them back (wabcut/ai/audio/Plans.h): the chunks tile the
// take, the crossfades sum to one, and a model with memory (a one-pole low-pass that starts from zero in every
// request, like DeepFilterNet's recurrent state) joins without a click when chunks have pre-roll - and clicks without.

#include <wabcut/ai/audio/Plans.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>
#include <span>
#include <vector>

using namespace wabcut::ai::audio;

namespace {

std::vector<float> signal(std::int64_t frames, int channels, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-0.4f, 0.4f);
    std::vector<float> out(static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels));
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = 0.3f * std::sin(static_cast<float>(i) * 0.01f) + d(rng) * 0.1f;
    }
    return out;
}

/// The fake helper's model: y += 0.002 (x - y) per channel, from 0 at the request's first frame.
std::vector<float> lowpass(std::span<const float> x, int channels) {
    std::vector<float> y(x.size());
    std::vector<float> state(static_cast<std::size_t>(channels), 0.0f);
    for (std::size_t i = 0; i < x.size(); ++i) {
        float& s = state[i % static_cast<std::size_t>(channels)];
        s += 0.002f * (x[i] - s);
        y[i] = s;
    }
    return y;
}

/// Runs `plan` with `model` per request and reassembles the output.
template <class Model>
std::vector<float> assemble(const std::vector<Segment>& plan, const std::vector<float>& input, int channels, Model model) {
    OverlapAssembler assembler(channels);
    std::vector<float> out;
    const auto ch = static_cast<std::size_t>(channels);
    for (const Segment& s : plan) {
        const std::span<const float> read(input.data() + static_cast<std::size_t>(s.readBegin) * ch,
                                          static_cast<std::size_t>(s.readEnd - s.readBegin) * ch);
        const std::vector<float> processed = model(read);
        const auto finished = assembler.push(s, processed);
        out.insert(out.end(), finished.begin(), finished.end());
    }
    return out;
}

float maxStep(const std::vector<float>& x, int channels, std::int64_t at) {
    // The largest sample-to-sample change in a window around `at` (a click shows as a jump).
    float m = 0.0f;
    const auto ch = static_cast<std::size_t>(channels);
    for (std::int64_t f = std::max<std::int64_t>(at - 50, 1); f < at + 50 && static_cast<std::size_t>(f) * ch < x.size(); ++f) {
        m = std::max(m, std::fabs(x[static_cast<std::size_t>(f) * ch] - x[static_cast<std::size_t>(f - 1) * ch]));
    }
    return m;
}

} // namespace

TEST_CASE("Chunk plans tile the take: crossfades match, pre- and post-roll stay inside it", "[ai][audio][plans]") {
    ChunkOptions o;
    o.body = 1000;
    o.preroll = 300;
    o.postroll = 50;
    o.crossfade = 40;
    for (const std::int64_t frames : {std::int64_t{1}, std::int64_t{999}, std::int64_t{1000}, std::int64_t{1001},
                                      std::int64_t{1020}, std::int64_t{5555}}) {
        const auto plan = planChunks(frames, o);
        REQUIRE_FALSE(plan.empty());
        CHECK(plan.front().outBegin == 0);
        CHECK(plan.front().fadeIn == 0);
        CHECK(plan.back().outEnd == frames);
        CHECK(plan.back().fadeOut == 0);
        for (std::size_t i = 0; i < plan.size(); ++i) {
            const Segment& s = plan[i];
            CHECK(s.readBegin >= 0);
            CHECK(s.readEnd <= frames);
            CHECK(s.readBegin <= s.outBegin);
            CHECK(s.readEnd >= s.outEnd);
            if (i > 0) {
                CHECK(s.outBegin == plan[i - 1].outEnd - plan[i - 1].fadeOut);
                CHECK(s.fadeIn == plan[i - 1].fadeOut);
                CHECK(s.readBegin == std::max<std::int64_t>(0, s.outBegin - o.preroll));
            }
        }
    }
    CHECK(planChunks(0, o).empty());
    // Stem segments: the model's length, a quarter overlap, the last one shorter.
    const auto segments = planSegments(10000, 4000, 1000);
    REQUIRE(segments.size() == 3);
    CHECK(segments[1].readBegin == 3000);
    CHECK(segments[1].fadeIn == 1000);
    CHECK(segments[2].readEnd == 10000);
    CHECK(segments[2].fadeOut == 0);
}

TEST_CASE("The assembler crossfades to the exact signal when requests change nothing", "[ai][audio][plans]") {
    for (const int channels : {1, 2}) {
        const std::int64_t frames = 7777;
        const std::vector<float> input = signal(frames, channels, 3);
        ChunkOptions o;
        o.body = 1000;
        o.preroll = 200;
        o.postroll = 30;
        o.crossfade = 64;
        const auto same = assemble(planChunks(frames, o), input, channels, [](std::span<const float> x) {
            return std::vector<float>(x.begin(), x.end());
        });
        REQUIRE(same.size() == input.size());
        for (std::size_t i = 0; i < input.size(); ++i) {
            REQUIRE(std::fabs(same[i] - input[i]) < 1e-6f);
        }
        const auto stems = assemble(planSegments(frames, 3000, 750), input, channels, [](std::span<const float> x) {
            return std::vector<float>(x.begin(), x.end());
        });
        REQUIRE(stems.size() == input.size());
        for (std::size_t i = 0; i < input.size(); ++i) {
            REQUIRE(std::fabs(stems[i] - input[i]) < 1e-6f);
        }
    }
}

TEST_CASE("Chunk seams of a model with memory: no click with pre-roll and crossfade, a click without",
          "[ai][audio][plans]") {
    const int channels = 2;
    const std::int64_t frames = 48000;
    const std::vector<float> input = signal(frames, channels, 9);
    const std::vector<float> reference = lowpass(input, channels); // one request for everything

    ChunkOptions good;
    good.body = 10000;
    good.preroll = 6000; // 12 time constants of the low-pass
    good.postroll = 500;
    good.crossfade = 480;
    const auto plan = planChunks(frames, good);
    REQUIRE(plan.size() == 5);
    const auto chunked = assemble(plan, input, channels, [&](std::span<const float> x) { return lowpass(x, channels); });
    REQUIRE(chunked.size() == reference.size());
    float worst = 0.0f;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        worst = std::max(worst, std::fabs(chunked[i] - reference[i]));
    }
    CHECK(worst < 1e-4f); // the seams are where the reference is
    for (std::size_t i = 1; i < plan.size(); ++i) {
        CHECK(maxStep(chunked, channels, plan[i].outBegin) <= maxStep(reference, channels, plan[i].outBegin) * 1.01f + 1e-5f);
    }

    // The same requests with no pre-roll and no crossfade: each restarts the low-pass from zero, a jump at every seam.
    ChunkOptions naive;
    naive.body = 10000;
    naive.preroll = 0;
    naive.postroll = 0;
    naive.crossfade = 0;
    const auto clicky = assemble(planChunks(frames, naive), input, channels, [&](std::span<const float> x) { return lowpass(x, channels); });
    CHECK(maxStep(clicky, channels, 10000) > 20.0f * maxStep(reference, channels, 10000));
}
