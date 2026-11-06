// 0.7, stream C, batch C2: the "Streaming (-14 LUFS)" loudness standard (card #2). The names and targets are pure; one
// case exports the generated clips with it and measures the file.

#include "ExportTestUtil.h"
#include "ExportLoudness.h"

#include <wabcut/audio/dsp/Loudness.h>
#include <wabcut/export/ExportEffects.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportPresets.h>
#include <wabcut/export/ExportSettings.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

using namespace exporttest;
using namespace wabcut;
using namespace wabcut::exporter;

namespace {

ExportSettings preset(std::string_view id) {
    const auto p = findPreset(id);
    REQUIRE(p.has_value());
    return p->settings;
}

std::string textOf(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("The Streaming loudness standard is -14 LUFS with a -1 dBTP ceiling and keeps the others as they were (002)",
          "[export][loudness]") {
    LoudnessNormalization n;
    // The three standards that were there keep their numbers and their names.
    n.standard = LoudnessStandard::EbuR128;
    CHECK(n.effectiveTarget() == -23.0);
    CHECK(n.effectiveTruePeak() == -1.0);
    n.standard = LoudnessStandard::AtscA85;
    CHECK(n.effectiveTarget() == -24.0);
    CHECK(n.effectiveTruePeak() == -2.0);
    n.standard = LoudnessStandard::Custom;
    n.targetLufs = -16.0;
    n.maxTruePeakDbtp = -1.5;
    CHECK(n.effectiveTarget() == -16.0);
    CHECK(n.effectiveTruePeak() == -1.5);
    CHECK(loudnessStandardName(LoudnessStandard::EbuR128) == "ebuR128");
    CHECK(loudnessStandardName(LoudnessStandard::AtscA85) == "atscA85");
    CHECK(loudnessStandardName(LoudnessStandard::Custom) == "custom");
    // The new one ignores the Custom fields, as the other standards do.
    n.standard = LoudnessStandard::Streaming;
    CHECK(n.effectiveTarget() == -14.0);
    CHECK(n.effectiveTruePeak() == -1.0);
    CHECK(loudnessStandardName(LoudnessStandard::Streaming) == "streaming");
    for (const LoudnessStandard s : {LoudnessStandard::EbuR128, LoudnessStandard::AtscA85, LoudnessStandard::Custom,
                                     LoudnessStandard::Streaming}) {
        CHECK(loudnessStandardFromName(loudnessStandardName(s)) == s);
    }
    CHECK_FALSE(loudnessStandardFromName("youtube").has_value());
}

TEST_CASE("A preset with the Streaming loudness standard round-trips through its JSON (002)", "[export][loudness]") {
    ExportSettings s = preset("youtube-1080p");
    s.effects.loudness.applied = true;
    s.effects.loudness.standard = LoudnessStandard::Streaming;
    const Json json = settingsToJson(s);
    CHECK(json["effects"]["loudness"]["standard"] == "streaming");
    const auto back = settingsFromJson(json);
    REQUIRE(back.has_value());
    CHECK(back->effects.loudness.applied);
    CHECK(back->effects.loudness.standard == LoudnessStandard::Streaming);
    CHECK(back->effects.loudness.effectiveTarget() == -14.0);
    CHECK(settingsToJson(*back) == json);
    // An older name still reads; an unknown one is refused (not silently turned into another standard).
    Json older = json;
    older["effects"]["loudness"]["standard"] = "ebuR128";
    REQUIRE(settingsFromJson(older).has_value());
    CHECK(settingsFromJson(older)->effects.loudness.standard == LoudnessStandard::EbuR128);
    Json unknown = json;
    unknown["effects"]["loudness"]["standard"] = "spotify";
    CHECK_FALSE(settingsFromJson(unknown).has_value());
}

TEST_CASE("The loudness log names the Streaming standard and its target (002)", "[export][loudness]") {
    LoudnessNormalization n;
    n.applied = true;
    n.standard = LoudnessStandard::Streaming;
    wabcut::exporter::detail::LoudnessMeasurement m;
    m.integratedLufs = -19.3;
    m.truePeakDbtp = -6.0;
    m.targetLufs = -14.0;
    m.gainDb = 5.3;
    m.normalized = true;
    const std::string text = wabcut::exporter::detail::loudnessLogText(m, n, "mix.wav");
    CHECK(text.find("Streaming platforms (-14 LUFS)") != std::string::npos);
    CHECK(text.find("Target loudness: -14.0 LUFS") != std::string::npos);
    CHECK(text.find("max true peak -1.0 dBTP") != std::string::npos);
}

TEST_CASE("Loudness Normalization with the Streaming standard lands on -14 LUFS under -1 dBTP (002)",
          "[export][media][audio][loudness]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject project = threeLayerTimeline(640, 360, Rate{30, 1}, Ticks::fromSeconds(2));
    const auto dir = freshOutputDir("loudness-streaming");
    ExportSettings s = preset("audio-wav");
    s.effects.loudness.applied = true;
    s.effects.loudness.standard = LoudnessStandard::Streaming;
    s.effects.loudness.toleranceLu = 0.5;
    s.effects.loudness.writeLog = true;
    ExportJob job(makeRequest(project, s, dir / "stream.wav"));
    const auto r = job.run();
    REQUIRE(r.has_value());
    REQUIRE(r->loudnessLufs.has_value());
    CHECK(textOf(r->loudnessLog).find("Streaming platforms (-14 LUFS)") != std::string::npos);
    const auto decoded = decodeAudio(dir / "stream.wav");
    REQUIRE(decoded.has_value());
    audio::dsp::LoudnessMeter meter(decoded->channels, decoded->sampleRate);
    meter.add(decoded->samples.data(), decoded->samples.size() / static_cast<std::size_t>(decoded->channels));
    const auto measured = meter.result();
    REQUIRE(measured.hasIntegrated());
    metric("loudness normalization (Streaming): " + std::to_string(measured.integratedLufs) + " LUFS (target -14), true peak " +
           std::to_string(measured.truePeakDbtp) + " dBTP (ceiling -1)");
    CHECK(std::abs(measured.integratedLufs - (-14.0)) < 1.0);
    CHECK(measured.truePeakDbtp < -1.0 + 0.3);
}
