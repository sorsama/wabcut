// A.4: DeepFilterNet3's signal processing without the model (wabcut/ai/audio/DeepFilter.h): the ERB bank, the STFT's
// perfect reconstruction with no delay (the take lines up with the source sample for sample), the features'
// normalisation states, the mask and the deep filter's frame alignment.

#include <wabcut/ai/audio/DeepFilter.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <numeric>
#include <random>
#include <vector>

using namespace wabcut;
using namespace wabcut::ai::audio;
namespace dfn = wabcut::ai::audio::dfn;

namespace {

std::vector<float> noise(std::size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-0.5f, 0.5f);
    std::vector<float> out(n);
    for (float& s : out) {
        s = d(rng);
    }
    return out;
}

float maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    float m = 0.0f;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        m = std::max(m, std::fabs(a[i] - b[i]));
    }
    return m;
}

dfn::Network identity() {
    return [](const dfn::Features& f) -> std::expected<dfn::NetworkOutput, Error> { return dfn::identityOutput(f.frames); };
}

} // namespace

TEST_CASE("DeepFilterNet ERB bank: 32 bands over the 481 bins, at least two each, widening with frequency",
          "[ai][audio][deepfilter]") {
    const std::vector<int> w = dfn::erbWidths();
    REQUIRE(w.size() == 32);
    CHECK(std::accumulate(w.begin(), w.end(), 0) == dfn::kBins);
    for (std::size_t i = 0; i < w.size(); ++i) {
        CHECK(w[i] >= dfn::kMinErbBins);
    }
    CHECK(w.front() == 2); // the lowest bands are the enforced minimum
    CHECK(w.back() > w[w.size() / 2]);
    // The window is power complementary at a 50 % hop (what makes the overlap-add exact).
    const std::vector<float>& win = dfn::window();
    for (int i = 0; i < dfn::kHop; ++i) {
        CHECK(win[static_cast<std::size_t>(i)] * win[static_cast<std::size_t>(i)] +
                  win[static_cast<std::size_t>(i + dfn::kHop)] * win[static_cast<std::size_t>(i + dfn::kHop)] ==
              Catch::Approx(1.0).margin(1e-6));
    }
}

TEST_CASE("DeepFilterNet STFT: analysis then synthesis gives the signal back, with no delay", "[ai][audio][deepfilter]") {
    for (const std::size_t n : {std::size_t{1}, std::size_t{479}, std::size_t{480}, std::size_t{4801}, std::size_t{48000}}) {
        const std::vector<float> x = noise(n, static_cast<unsigned>(n));
        const auto spectra = dfn::analyse(x, dfn::framesFor(n));
        const std::vector<float> y = dfn::synthesise(spectra, n);
        REQUIRE(y.size() == n);
        CHECK(maxDiff(x, y) < 1e-5f);
    }
    // A sine's energy lands in its bin with the analysis normalisation libDF uses (1 / (960^2 / 960)).
    std::vector<float> sine(48000);
    for (std::size_t i = 0; i < sine.size(); ++i) {
        sine[i] = std::sin(2.0f * std::numbers::pi_v<float> * 1000.0f * static_cast<float>(i) / 48000.0f); // bin 20
    }
    const auto spectra = dfn::analyse(sine, 10);
    const dfn::Complex* frame = spectra.frame(5);
    int peak = 0;
    for (int k = 0; k < dfn::kBins; ++k) {
        if (std::abs(frame[k]) > std::abs(frame[peak])) {
            peak = k;
        }
    }
    CHECK(peak == 20);
}

TEST_CASE("DeepFilterNet pipeline: an identity network returns the input sample-aligned; gains and the deep filter apply "
          "to the right frames",
          "[ai][audio][deepfilter]") {
    const std::vector<float> x = noise(48000 + 123, 5);
    auto same = dfn::enhanceChannel(x, identity());
    REQUIRE(same);
    CHECK(maxDiff(x, *same) < 1e-5f);

    // Half the gain everywhere, and the deep filter's tap on frame t (order index 2) at 0.5: half the signal.
    auto half = dfn::enhanceChannel(x, [](const dfn::Features& f) -> std::expected<dfn::NetworkOutput, Error> {
        dfn::NetworkOutput o = dfn::identityOutput(f.frames);
        for (float& g : o.gains) {
            g = 0.5f;
        }
        for (std::size_t i = 0; i < o.coefs.size(); i += 2) {
            o.coefs[i] *= 0.5f;
        }
        return o;
    });
    REQUIRE(half);
    for (std::size_t i = 0; i < x.size(); i += 997) {
        CHECK((*half)[i] == Catch::Approx(0.5f * x[i]).margin(1e-5));
    }

    // The deep filter's tap on the previous frame (order index 1) delays the low band by one hop (480 samples): the
    // network's frame t applies to spectrum t - 2, and order index i reads noisy frame t - 2 - 2 + i.
    std::vector<float> lowTone(24000);
    for (std::size_t i = 0; i < lowTone.size(); ++i) {
        lowTone[i] = 0.3f * std::sin(2.0f * std::numbers::pi_v<float> * 300.0f * static_cast<float>(i) / 48000.0f);
    }
    auto delayed = dfn::enhanceChannel(lowTone, [](const dfn::Features& f) -> std::expected<dfn::NetworkOutput, Error> {
        dfn::NetworkOutput o = dfn::identityOutput(f.frames);
        for (std::size_t t = 0; t < static_cast<std::size_t>(f.frames); ++t) {
            for (std::size_t bin = 0; bin < dfn::kDfBins; ++bin) {
                float* c = o.coefs.data() + (t * dfn::kDfBins + bin) * dfn::kDfOrder * 2;
                c[2 * 2] = 0.0f;
                c[1 * 2] = 1.0f;
            }
        }
        return o;
    });
    REQUIRE(delayed);
    for (std::size_t i = 2000; i < 20000; i += 501) {
        CHECK((*delayed)[i] == Catch::Approx(lowTone[i - dfn::kHop]).margin(2e-3));
    }

    // A network returning the wrong number of frames is an error, not a crash.
    auto wrong = dfn::enhanceChannel(x, [](const dfn::Features& f) -> std::expected<dfn::NetworkOutput, Error> {
        return dfn::identityOutput(f.frames - 1);
    });
    CHECK_FALSE(wrong);
}

TEST_CASE("DeepFilterNet features: libDF's normalisation states from their initial values", "[ai][audio][deepfilter]") {
    // Silence: every band's power is 0, so the ERB feature is (-100 dB - state) / 40 with the state moving from its
    // initial value (-60 dB for band 0) towards -100 dB by alpha = 0.99 per frame.
    const std::vector<float> silence(4800, 0.0f);
    const auto f = dfn::features(dfn::analyse(silence, 4));
    REQUIRE(f.frames == 4);
    REQUIRE(f.erb.size() == 4u * 32u);
    REQUIRE(f.spec.size() == 2u * 4u * 96u);
    float state = -60.0f;
    for (int t = 0; t < 4; ++t) {
        const float v = std::log10(1e-10f) * 10.0f;
        state = v * (1.0f - dfn::kNormAlpha) + state * dfn::kNormAlpha;
        CHECK(f.erb[static_cast<std::size_t>(t) * 32] == Catch::Approx((v - state) / 40.0f).margin(1e-5));
    }
    for (const float s : f.spec) {
        CHECK(s == 0.0f);
    }
}
