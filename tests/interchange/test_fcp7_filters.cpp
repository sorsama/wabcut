// Colour grades and audio EQ / dynamics through FCP7 XML, both ways (0.6, card #66): a Premiere-shaped XML with
// Lumetri Color, colour correctors, RGB Curves, ProcAmp, Parametric Equalizer, Dynamics and compressors lands on
// Wabcut's grade and audio effects; FCP7's own EQ, compressor and colour corrector filters too; unknown filters are
// named; and Wabcut's grade, EQ and dynamics export as Premiere's filters that come back exactly (and, read without
// Wabcut's own parameters as another editor would, with every mapped value).

#include "InterchangeTestSupport.h"
#include "TimelineFixture.h"

#include "FilterMap.h"

#include <wabcut/interchange/Export.h>
#include <wabcut/interchange/Import.h>
#include <wabcut/model/FinishingOps.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <regex>
#include <span>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::interchange;
using namespace wabcut::interchange::test;
using wabcut::test::ok;

namespace {

constexpr ReportOutcome C = ReportOutcome::Carried;
constexpr ReportOutcome A = ReportOutcome::Approximated;
constexpr ReportOutcome D = ReportOutcome::Dropped;

const EffectInstance& effectAt(const Clip& clip, std::size_t index) {
    REQUIRE(index < clip.effects().size());
    return clip.effects()[index];
}

double param(const EffectInstance& e, std::string_view key) {
    const ParamValue* v = e.find(key);
    INFO(e.effect << " " << key);
    REQUIRE(v != nullptr);
    return v->constant();
}

bool near(double a, double b, double eps = 1e-6) { return std::abs(a - b) < eps; }

std::string xmeml25(const std::string& videoFilters, const std::string& audioFilters) {
    const std::string file = "<file id=\"f1\"><name>A.mov</name><pathurl>file:///m/A.mov</pathurl><rate><timebase>25</timebase>"
                             "</rate><duration>1000</duration><media><video/><audio><channelcount>1</channelcount></audio>"
                             "</media></file>";
    return R"(<?xml version="1.0" encoding="UTF-8"?><xmeml version="4"><sequence id="s1"><name>T</name><rate><timebase>25</timebase>
<ntsc>FALSE</ntsc></rate><media><video><track><clipitem id="v"><name>A</name><rate><timebase>25</timebase></rate><start>0</start>
<end>100</end><in>0</in><out>100</out>)" +
           file + videoFilters + R"(</clipitem></track></video><audio><track><clipitem id="a"><name>A</name><rate><timebase>25</timebase>
</rate><start>0</start><end>100</end><in>0</in><out>100</out><file id="f1"/><sourcetrack><mediatype>audio</mediatype>
<trackindex>1</trackindex></sourcetrack>)" +
           audioFilters + "</clipitem></track></audio></media></sequence></xmeml>";
}

std::string filter(const std::string& name, const std::string& id, const std::string& mediaType, const std::string& params) {
    return "<filter><effect><name>" + name + "</name><effectid>" + id + "</effectid><effecttype>filter</effecttype><mediatype>" +
           mediaType + "</mediatype>" + params + "</effect></filter>";
}

std::string p(const std::string& name, const std::string& value, const std::string& id = {}) {
    return "<parameter><parameterid>" + (id.empty() ? name : id) + "</parameterid><name>" + name + "</name><value>" + value +
           "</value></parameter>";
}

} // namespace

TEST_CASE("FCP7 XML: Premiere's Lumetri colour correctors RGB Curves and ProcAmp land on Wabcut's grade (0.6)",
          "[interchange][fcp7][filters]") {
    const ImportResult r = importFixture("premiere_color_audio_filters.xml");
    const Sequence& s = sequenceAt(r, 0);
    const Clip& v = track(s, "V1").clips[0];
    REQUIRE(v.effects().size() == 4); // the third-party filter is not imported
    // Lumetri Color -> Color Grade: Basic Correction, Creative and Vignette.
    const EffectInstance& lumetri = effectAt(v, 0);
    CHECK(lumetri.effect == "wabcut.color.grade");
    CHECK(param(lumetri, "basic.temperature") == 12.0);
    CHECK(param(lumetri, "basic.tint") == -5.0);
    CHECK(param(lumetri, "basic.contrast") == 20.0);
    CHECK(param(lumetri, "basic.highlights") == -30.0);
    CHECK(param(lumetri, "basic.shadows") == 25.0);
    CHECK(param(lumetri, "basic.whites") == 10.0);
    CHECK(param(lumetri, "basic.blacks") == -10.0);
    CHECK(param(lumetri, "basic.saturation") == 110.0);
    CHECK(param(lumetri, "creative.lookIntensity") == 80.0);
    CHECK(param(lumetri, "creative.fadedFilm") == 15.0);
    CHECK(param(lumetri, "creative.vibrance") == 10.0);
    CHECK(param(lumetri, "creative.saturation") == 90.0);
    CHECK(param(lumetri, "creative.tintBalance") == 5.0);
    CHECK(near(param(lumetri, "creative.shadowTint.hue"), 209.88, 0.05));
    CHECK(param(lumetri, "creative.shadowTint.amount") == 100.0);
    CHECK(param(lumetri, "vignette.amount") == -1.5);
    CHECK(param(lumetri, "vignette.midpoint") == 40.0);
    CHECK(param(lumetri, "vignette.roundness") == 10.0);
    CHECK(param(lumetri, "vignette.feather") == 60.0);
    // Exposure keyframes: media frames 50 and 100 of the clip.
    const Curve* exposure = lumetri.find("basic.exposure")->curve();
    REQUIRE(exposure != nullptr);
    REQUIRE(exposure->keys.size() == 2);
    CHECK(exposure->keys[0].time == frames(50, kRate25));
    CHECK(exposure->keys[1].value == 0.8);
    CHECK(hasEntry(r.report, A, ReportCategory::Effect, "'Lumetri Color' on 'Interview.mov' -> Color Grade: Shadow Tint colour"));
    CHECK(hasEntry(r.report, D, ReportCategory::Effect, "Look LUT 'SL BIG MINUS BLUE' (FCP7 XML has no LUT path"));
    CHECK(hasEntry(r.report, D, ReportCategory::Effect, "Sharpen 20"));
    // Fast Color Corrector -> the grade's saturation and a CDL for its levels and balance.
    const EffectInstance& fcc = effectAt(v, 1);
    CHECK(fcc.effect == "wabcut.color.grade");
    CHECK(param(fcc, "basic.saturation") == 120.0);
    CHECK(near(param(fcc, "wheels.gain.red"), 255.0 / 219.0));
    CHECK(near(param(fcc, "wheels.offset.green"), -16.0 * (255.0 / 219.0) / 255.0));
    CHECK(param(fcc, "wheels.gamma.blue") != 1.0); // the balance (20 at 200 degrees) pushes the midtones blue
    CHECK(param(fcc, "wheels.gamma.blue") < param(fcc, "wheels.gamma.red"));
    CHECK(hasEntry(r.report, A, ReportCategory::Effect, "levels became a CDL: slope 1.16"));
    // RGB Curves -> the grade's curves, as points 0..1.
    const EffectInstance& curves = effectAt(v, 2);
    CHECK(curves.effect == "wabcut.color.grade");
    const EffectData* master = curves.findData("curves.master");
    REQUIRE(master != nullptr);
    REQUIRE(master->points() != nullptr);
    REQUIRE(master->points()->size() == 4);
    CHECK(near((*master->points())[1].x, 64.0 / 255.0));
    CHECK(near((*master->points())[2].y, 210.0 / 255.0));
    REQUIRE(curves.findData("curves.blue") != nullptr);
    CHECK(curves.findData("curves.red") == nullptr);
    // ProcAmp: Wabcut's own, by Premiere's names, exact.
    const EffectInstance& procAmp = effectAt(v, 3);
    CHECK(procAmp.effect == "wabcut.adjust.procAmp");
    CHECK(param(procAmp, "brightness") == 8.0);
    CHECK(param(procAmp, "contrast") == 105.0);
    CHECK(param(procAmp, "saturation") == 95.0);
    CHECK(hasEntry(r.report, C, ReportCategory::Effect, "'ProcAmp' on 'Interview.mov' -> ProcAmp (static values)"));
    // The filter Wabcut does not have: named, with its parameter count.
    CHECK(hasEntry(r.report, D, ReportCategory::Effect,
                   "effect 'Film Look Pro' (3 parameters) on 'Interview.mov' is not supported: not imported"));
}

TEST_CASE("FCP7 XML: Premiere's Parametric Equalizer Dynamics and compressors land on Wabcut's audio effects (0.6)",
          "[interchange][fcp7][filters]") {
    const ImportResult r = importFixture("premiere_color_audio_filters.xml");
    const Sequence& s = sequenceAt(r, 0);
    const Clip& a = track(s, "A1").clips[0];
    REQUIRE(a.effects().size() == 4);
    const EffectInstance& eq = effectAt(a, 0);
    CHECK(eq.effect == "wabcut.audio.parametricEq");
    CHECK(param(eq, "lowShelfFrequency") == 120.0);
    CHECK(param(eq, "lowShelfGain") == -4.0);
    CHECK(param(eq, "band2Frequency") == 250.0);
    CHECK(param(eq, "band2Gain") == -3.5);
    CHECK(param(eq, "band2Q") == 1.4);
    CHECK(param(eq, "highPassEnabled") == 1.0);
    CHECK(param(eq, "highPassFrequency") == 80.0);
    const EffectInstance& dynamics = effectAt(a, 1);
    CHECK(dynamics.effect == "wabcut.audio.dynamics");
    CHECK(param(dynamics, "compressorEnable") == 1.0);
    CHECK(param(dynamics, "compressorThreshold") == -24.0);
    CHECK(param(dynamics, "compressorRatio") == 4.0);
    CHECK(param(dynamics, "makeUp") == 3.0);
    const EffectInstance& multiband = effectAt(a, 2);
    CHECK(multiband.effect == "wabcut.audio.multibandCompressor");
    CHECK(param(multiband, "band1Threshold") == -30.0);
    CHECK(param(multiband, "outputGain") == 2.0);
    const EffectInstance& single = effectAt(a, 3);
    CHECK(single.effect == "wabcut.audio.singleBandCompressor");
    CHECK(param(single, "threshold") == -18.0);
    CHECK(param(single, "ratio") == 6.0);
    CHECK(param(single, "attack") == 5.0);
    CHECK(hasEntry(r.report, C, ReportCategory::Effect, "'Dynamics' on 'Interview.mov' -> Dynamics (static values)"));
}

TEST_CASE("FCP7 XML: FCP7's own EQ compressor and colour corrector filters are carried (0.6)", "[interchange][fcp7][filters]") {
    const std::string video =
        filter("Color Corrector 3-way", "colorcorrector3way", "video",
               "<parameter><parameterid>blacks</parameterid><name>Blacks Balance</name><value><horiz>0.1</horiz><vert>0</vert>"
               "</value></parameter><parameter><parameterid>whites</parameterid><name>Whites Balance</name><value><horiz>0</horiz>"
               "<vert>0.2</vert></value></parameter>" +
                   p("Saturation", "130", "sat"));
    const std::string audio =
        filter("3 Band Equalizer", "3bandequalizer", "audio",
               p("Low Frequency", "200") + p("Low Gain", "3") + p("Mid Frequency", "2500") + p("Mid Gain", "-2") + p("High Gain", "4")) +
        filter("Parametric Equalizer", "parametricequalizer", "audio", p("Frequency", "3000") + p("Q", "2") + p("Gain", "-6")) +
        filter("Compressor/Limiter", "compressorlimiter", "audio",
               p("Threshold", "-20") + p("Ratio", "3") + p("Attack Time", "12") + p("Release Time", "300") + p("Preserve Volume", "TRUE"));
    const ImportResult r = importText(xmeml25(video, audio), Format::Fcp7Xml);
    const Sequence& s = sequenceAt(r, 0);
    const Clip& v = track(s, "V1").clips[0];
    REQUIRE(v.effects().size() == 1);
    const EffectInstance& grade = effectAt(v, 0);
    CHECK(grade.effect == "wabcut.color.grade");
    CHECK(param(grade, "basic.saturation") == 130.0);
    CHECK(param(grade, "wheels.lift.red") > 0.0);   // shadows pushed to red (balance at 0 degrees)
    CHECK(param(grade, "wheels.gain.green") > 1.0); // highlights pushed to 90 degrees, towards green
    CHECK(hasEntry(r.report, A, ReportCategory::Effect, "shadow balance"));
    const Clip& a = track(s, "A1").clips[0];
    REQUIRE(a.effects().size() == 3);
    const EffectInstance& eq3 = effectAt(a, 0);
    CHECK(eq3.effect == "wabcut.audio.eq3");
    CHECK(param(eq3, "lowFrequency") == 200.0);
    CHECK(param(eq3, "lowGain") == 3.0);
    CHECK(param(eq3, "midFrequency") == 2500.0);
    CHECK(param(eq3, "midGain") == -2.0);
    CHECK(param(eq3, "highGain") == 4.0);
    const EffectInstance& band = effectAt(a, 1);
    CHECK(band.effect == "wabcut.audio.parametricEq");
    CHECK(param(band, "band1Frequency") == 3000.0);
    CHECK(param(band, "band1Q") == 2.0);
    CHECK(param(band, "band1Gain") == -6.0);
    const EffectInstance& compressor = effectAt(a, 2);
    CHECK(compressor.effect == "wabcut.audio.singleBandCompressor");
    CHECK(param(compressor, "threshold") == -20.0);
    CHECK(param(compressor, "ratio") == 3.0);
    CHECK(param(compressor, "attack") == 12.0);
    CHECK(param(compressor, "release") == 300.0);
    CHECK(hasEntry(r.report, A, ReportCategory::Effect, "Preserve Volume"));
}

TEST_CASE("FCP7 XML: grades EQ and dynamics export as Premiere's filters and round-trip (0.6)", "[interchange][fcp7][filters]") {
    wabcut::test::Timeline tl(R"(
        V1 |Aaaaaaaaaa|
        A1 |Aaaaaaaaaa|
    )");
    ProjectState s = tl.state;
    const ClipId v = tl.clip("V1@0");
    const ClipId a = tl.clip("A1@0");
    s = ok(edit::addEffect(s, tl.seq, std::span(&v, 1), "wabcut.color.grade",
                           {EffectParam{"basic.exposure", ParamValue(0.75)}, EffectParam{"basic.contrast", ParamValue(-12.0)},
                            EffectParam{"basic.saturation", ParamValue(115.0)}, EffectParam{"creative.fadedFilm", ParamValue(20.0)},
                            EffectParam{"creative.saturation", ParamValue(80.0)}, EffectParam{"vignette.amount", ParamValue(-2.0)},
                            EffectParam{"wheels.gain.red", ParamValue(1.2)}}))
            .state;
    {
        const Clip& clip = track(*s.findSequence(tl.seq), "V1").clips[0];
        edit::EffectChange change;
        change.data = {EffectData{"curves.master", std::vector<EffectPoint>{{0.0, 0.0}, {0.5, 0.6}, {1.0, 1.0}}}};
        s = ok(edit::setEffect(s, tl.seq, v, clip.effects().front().id, change)).state;
    }
    s = ok(edit::addEffect(s, tl.seq, std::span(&a, 1), "wabcut.audio.eq3",
                           {EffectParam{"lowGain", ParamValue(-3.0)}, EffectParam{"midFrequency", ParamValue(2000.0)},
                            EffectParam{"midGain", ParamValue(2.5)}, EffectParam{"highGain", ParamValue(1.5)}}))
            .state;
    s = ok(edit::addEffect(s, tl.seq, std::span(&a, 1), "wabcut.audio.singleBandCompressor",
                           {EffectParam{"threshold", ParamValue(-22.0)}, EffectParam{"ratio", ParamValue(5.0)}}))
            .state;
    s = ok(edit::addEffect(s, tl.seq, std::span(&a, 1), "wabcut.audio.dynamics",
                           {EffectParam{"compressorEnable", ParamValue(1.0)}, EffectParam{"compressorThreshold", ParamValue(-30.0)}}))
            .state;
    s = ok(edit::addEffect(s, tl.seq, std::span(&a, 1), "wabcut.audio.multibandCompressor",
                           {EffectParam{"band2Threshold", ParamValue(-25.0)}}))
            .state;

    const auto exported = exportSequence(s, tl.seq, Format::Fcp7Xml);
    REQUIRE(exported.has_value());
    const std::string& xml = exported->data;
    // Premiere's filters, with Premiere's names and units.
    CHECK(xml.find("<effectid>Lumetri</effectid>") != std::string::npos); // PF: as Premiere writes it (empty name)
    CHECK(xml.find("<name>Exposure</name>") != std::string::npos);
    CHECK(xml.find("<name>Faded Film</name>") != std::string::npos);
    CHECK(xml.find("<name>Parametric Equalizer</name>") != std::string::npos);
    CHECK(xml.find("<name>Low Shelf Gain</name>") != std::string::npos);
    CHECK(xml.find("<name>Single-band Compressor</name>") != std::string::npos);
    CHECK(xml.find("<name>Dynamics</name>") != std::string::npos);
    CHECK(xml.find("<name>Multiband Compressor</name>") != std::string::npos);
    CHECK(hasEntry(exported->report, A, ReportCategory::Clip, "Lumetri Color: its wheels, curves settings are only read back by Wabcut"));

    const auto stacksOf = [](const ProjectState& project, SequenceId seq) {
        std::vector<EffectStack> out;
        project.findSequence(seq)->forEachTrack([&](const Track& t) {
            for (const Clip& c : t.clips) {
                EffectStack stack = c.effects();
                for (EffectInstance& e : stack) {
                    e.id = {};
                }
                out.push_back(std::move(stack));
            }
        });
        return out;
    };
    // Wabcut reads its own file back exactly (the "wabcut:" parameters).
    const ImportResult back = importText(xml, Format::Fcp7Xml);
    CHECK(stacksOf(back.project, back.sequences.front()) == stacksOf(s, tl.seq));

    // Another editor keeps only the native parameters: the mapped values still come back.
    const std::string native = std::regex_replace(xml, std::regex("<parameter authoringApp=\"Wabcut\">[\\s\\S]*?</parameter>"), "");
    REQUIRE(native.find("wabcut:") == std::string::npos);
    const ImportResult other = importText(native, Format::Fcp7Xml);
    const Sequence& o = sequenceAt(other, 0);
    const EffectInstance& grade = effectAt(track(o, "V1").clips[0], 0);
    CHECK(grade.effect == "wabcut.color.grade");
    CHECK(param(grade, "basic.exposure") == 0.75);
    CHECK(param(grade, "basic.contrast") == -12.0);
    CHECK(param(grade, "basic.saturation") == 115.0);
    CHECK(param(grade, "creative.fadedFilm") == 20.0);
    CHECK(param(grade, "creative.saturation") == 80.0);
    CHECK(param(grade, "vignette.amount") == -2.0);
    CHECK(grade.find("wheels.gain.red") == nullptr); // only Wabcut's parameters carry the wheels
    const Clip& audio = track(o, "A1").clips[0];
    REQUIRE(audio.effects().size() == 4);
    const EffectInstance& eq = effectAt(audio, 0);
    CHECK(eq.effect == "wabcut.audio.parametricEq"); // Premiere's equalizer: the 3-band EQ's bands on its shelves and band 1
    CHECK(param(eq, "lowShelfGain") == -3.0);
    CHECK(param(eq, "band1Frequency") == 2000.0);
    CHECK(param(eq, "band1Gain") == 2.5);
    CHECK(param(eq, "highShelfGain") == 1.5);
    CHECK(param(effectAt(audio, 1), "threshold") == -22.0);
    CHECK(param(effectAt(audio, 1), "ratio") == 5.0);
    CHECK(param(effectAt(audio, 2), "compressorThreshold") == -30.0);
    CHECK(param(effectAt(audio, 3), "band2Threshold") == -25.0);
}

TEST_CASE("FCP7 XML: effect data text survives and malformed data is refused (0.6)", "[interchange][fcp7][filters]") {
    using namespace wabcut::interchange::detail;
    const EffectData curve{"curves.red", std::vector<EffectPoint>{{0.0, 0.1}, {1.0, 0.9}}};
    const auto back = effectDataFromText("curves.red", effectDataToText(curve));
    REQUIRE(back.has_value());
    CHECK(*back == curve);
    CHECK(effectDataFromText("creative.look", "text:builtin:Kodak 2383").has_value());
    CHECK_FALSE(effectDataFromText("basic.inputLut", "text:asset:12").has_value()); // another project's asset
    CHECK_FALSE(effectDataFromText("curves.red", "points:1 nan").has_value());
    CHECK_FALSE(effectDataFromText("curves.red", "points:").has_value());
    CHECK_FALSE(effectDataFromText("curves.red", "garbage").has_value());
}

// --- PF: what Premiere Pro really writes (a trimmed real export, fixtures/premiere_lumetri_real.xml) -----------------

namespace {

/// The fixture with parameter `id`'s <value> of the Lumetri filter replaced by `value`.
std::string withLumetriValue(const std::string& xml, const std::string& id, const std::string& value) {
    return std::regex_replace(xml, std::regex("(<parameterid>" + id + "</parameterid>[\\s\\S]*?<value>)[^<]*"), "$01" + value, // "$01": a value starting with a digit is not read as group 10
                              std::regex_constants::format_first_only);
}

/// A Lumetri state block (base64) with a LUT file, an edited tone curve and a moved midtone wheel.
constexpr const char* kEditedBlob =
    "PEx1bWV0cmk+CiAgPGtleWZyYW1lPgogICAgPHNoYWRlcj4KICAgICAgPG5hbWU+IkxVVCI8L25hbWU+CiAgICAgIDxwYXJhbWV0ZXJzPgogICAgICAgIDxf"
    "X0VtYmVkPiIxIjwvX19FbWJlZD4KICAgICAgICA8TFVUPiJDOi9Mb29rcy9UZWFsIE9yYW5nZS5jdWJlIjwvTFVUPgogICAgICA8L3BhcmFtZXRlcnM+CiAg"
    "ICA8L3NoYWRlcj4KICAgIDxzaGFkZXI+CiAgICAgIDxuYW1lPiJUb25lQ3VydmVzIjwvbmFtZT4KICAgICAgPHBhcmFtZXRlcnM+CiAgICAgICAgPFRvbmVD"
    "dXJ2ZXNMdW1hPiJjdWJpYyx2YWx1ZTowQDA7MC42QDAuNTsxQDEiPC9Ub25lQ3VydmVzTHVtYT4KICAgICAgPC9wYXJhbWV0ZXJzPgogICAgPC9zaGFkZXI+"
    "CiAgICA8c2hhZGVyPgogICAgICA8bmFtZT4iV2hlZWxzIjwvbmFtZT4KICAgICAgPHBhcmFtZXRlcnM+CiAgICAgICAgPERpZmZXaGl0ZT4iTjEwMCI8L0Rp"
    "ZmZXaGl0ZT4KICAgICAgICA8TWlkdG9uZXMuWD4iRDAuMiI8L01pZHRvbmVzLlg+CiAgICAgIDwvcGFyYW1ldGVycz4KICAgIDwvc2hhZGVyPgogIDwva2V5"
    "ZnJhbWU+CjwvTHVtZXRyaT4K";

} // namespace

TEST_CASE("FCP7 XML: a real Premiere export's Lumetri Color lands on the grade by parameter id (0.6)",
          "[interchange][fcp7][filters][premiere]") {
    const ImportResult r = importFixture("premiere_lumetri_real.xml");
    const Sequence& s = sequenceAt(r, 0);
    const Clip& v = track(s, "V1").clips[0];
    REQUIRE(v.effects().size() == 1);
    const EffectInstance& grade = effectAt(v, 0);
    CHECK(grade.effect == "wabcut.color.grade");
    CHECK(grade.enabled);
    // Basic Correction (Premiere's values and units are Wabcut's: the same sliders).
    CHECK(param(grade, "basic.temperature") == -15.0);
    CHECK(param(grade, "basic.tint") == 1.318359375);
    CHECK(near(param(grade, "basic.exposure"), 2.4, 1e-5));
    CHECK(param(grade, "basic.contrast") == 23.0);
    CHECK(param(grade, "basic.highlights") == -4.3701171875);
    CHECK(param(grade, "basic.shadows") == -3.61328125);
    CHECK(param(grade, "basic.whites") == -8.1787109375);
    CHECK(param(grade, "basic.blacks") == -5.126953125);
    CHECK(param(grade, "basic.saturation") == 120.0); // id 20, the Basic one of the two "Saturation"s
    // Creative: Vibrance (id 30) is Creative's; its Saturation (id 31) stays apart from Basic's.
    CHECK(param(grade, "creative.vibrance") == 42.0);
    CHECK(grade.find("basic.vibrance") == nullptr);
    CHECK(param(grade, "creative.saturation") == 100.0);
    CHECK(param(grade, "creative.lookIntensity") == 100.0);
    CHECK(param(grade, "creative.fadedFilm") == 0.0);
    CHECK(param(grade, "creative.tintBalance") == 0.0);
    CHECK(param(grade, "vignette.amount") == 0.0);
    CHECK(param(grade, "vignette.midpoint") == 50.0);
    CHECK(param(grade, "vignette.roundness") == 0.0);
    CHECK(param(grade, "vignette.feather") == 50.0);
    // Every section switch is on: nothing switched off.
    for (const char* key : {"basic.enabled", "creative.enabled", "curves.enabled", "wheels.enabled", "vignette.enabled"}) {
        CHECK(grade.find(key) == nullptr);
    }
    CHECK(hasEntry(r.report, C, ReportCategory::Effect, "'Lumetri Color' on 'Graded Clip.mp4' -> Color Grade (static values)"));
    // No look, no LUT, default curves and wheels in the state block, Sharpen 0: nothing to report as lost.
    CHECK(reasons(r.report, D, ReportCategory::Effect).empty());
    // Premiere wrote no audio filters (the clip had Parametric Equalizer and Dynamics in Premiere).
    CHECK(track(s, "A1").clips[0].effects().empty());
}

TEST_CASE("FCP7 XML: Premiere's key-list values keyframes bypass section switches looks and state block (0.6)",
          "[interchange][fcp7][filters][premiere]") {
    using namespace wabcut::interchange::detail;
    // The value form itself.
    const auto statik = premiereKeyList("-91445760000000000,-15.,0,0,0,0,0,0");
    REQUIRE(statik.has_value());
    CHECK(statik->isConstant());
    CHECK(statik->constant() == -15.0);
    CHECK(premiereKeyList("-91445760000000000,true,0,0,0,0,0,0")->constant() == 1.0);
    CHECK(premiereKeyList("-91445760000000000,false,0,0,0,0,0,0")->constant() == 0.0);
    CHECK_FALSE(premiereKeyList("12.5").has_value());
    CHECK_FALSE(premiereKeyList("PEx1bWV0cmk+").has_value());
    CHECK_FALSE(premiereKeyList("-91445760000000000,abc,0").has_value());
    CHECK_FALSE(premiereKeyList("x,1,0").has_value());
    const auto keyed = premiereKeyList("-91445760000000000,0.5,0,0,0,0,0,0;254016000000,2.,0,0,0,0,0,0;0,1.,0,0,0,0,0,0;");
    REQUIRE(keyed.has_value());
    const Curve* curve = keyed->curve();
    REQUIRE(curve != nullptr);
    REQUIRE(curve->keys.size() == 2); // the static key at -100 hours is not a keyframe
    CHECK(curve->keys[0].time == Ticks{0});
    CHECK(curve->keys[0].value == 1.0);
    CHECK(curve->keys[1].time == Ticks{kTicksPerSecond});
    CHECK(curve->keys[1].value == 2.0);
    CHECK(premiereKeyListText(ParamValue(23.0)) == "-91445760000000000,23,0,0,0,0,0,0");
    CHECK(premiereKeyListText(ParamValue(0.0), 1.0, true) == "-91445760000000000,false,0,0,0,0,0,0");
    CHECK(premiereKeyList(premiereKeyListText(*keyed)) == keyed);

    // The real file, edited the way Premiere would write these settings.
    std::string xml = readFixture("premiere_lumetri_real.xml");
    xml = withLumetriValue(xml, "11", "0,1.,0,0,0,0,0,0;254016000000,2.,0,0,0,0,0,0"); // Exposure keyframed
    xml = withLumetriValue(xml, "23", "-91445760000000000,false,0,0,0,0,0,0");          // Creative switched off
    xml = withLumetriValue(xml, "25", "-91445760000000000,3,0,0,0,0,0,0");              // a built-in Look
    xml = withLumetriValue(xml, "29", "-91445760000000000,25.,0,0,0,0,0,0");            // Sharpen
    xml = std::regex_replace(xml, std::regex("<pproBypass>false</pproBypass>"), "<pproBypass>true</pproBypass>");
    {
        const ImportResult r = importText(xml, Format::Fcp7Xml);
        const EffectInstance& grade = effectAt(track(sequenceAt(r, 0), "V1").clips[0], 0);
        CHECK_FALSE(grade.enabled); // pproBypass
        REQUIRE(grade.find("basic.exposure") != nullptr);
        const Curve* exposure = grade.find("basic.exposure")->curve();
        REQUIRE(exposure != nullptr);
        REQUIRE(exposure->keys.size() == 2);
        CHECK(exposure->keys[1].time == Ticks{kTicksPerSecond});
        CHECK(exposure->keys[1].value == 2.0);
        CHECK(param(grade, "creative.enabled") == 0.0);
        CHECK(param(grade, "basic.contrast") == 23.0);
        CHECK(hasEntry(r.report, D, ReportCategory::Effect, "Look no. 3 of Premiere's list (FCP7 XML has no LUT file"));
        CHECK(hasEntry(r.report, D, ReportCategory::Effect, "Sharpen 25"));
    }
    // A state block with a LUT file, curves and wheels: named, not silently lost; and one that cannot be read.
    {
        const ImportResult r = importText(withLumetriValue(xml, "1", kEditedBlob), Format::Fcp7Xml);
        CHECK(hasEntry(r.report, D, ReportCategory::Effect, "LUT 'Teal Orange.cube' (FCP7 XML has no LUT path"));
        CHECK_FALSE(hasEntry(r.report, D, ReportCategory::Effect, "Look no. 3")); // the file name says more
        CHECK(hasEntry(r.report, D, ReportCategory::Effect,
                       "Lumetri RGB curves, colour wheels (kept only in Premiere's own state block"));
    }
    {
        const ImportResult r = importText(withLumetriValue(xml, "1", "not base64!"), Format::Fcp7Xml);
        CHECK(hasEntry(r.report, D, ReportCategory::Effect, "Premiere's Lumetri state block could not be read"));
        CHECK(param(effectAt(track(sequenceAt(r, 0), "V1").clips[0], 0), "basic.temperature") == -15.0);
    }
}

TEST_CASE("FCP7 XML: the grade exports as Lumetri the way Premiere writes it (0.6)", "[interchange][fcp7][filters][premiere]") {
    wabcut::test::Timeline tl(R"(
        V1 |Aaaaaaaaaa|
    )");
    ProjectState s = tl.state;
    const ClipId v = tl.clip("V1@0");
    Curve exposure;
    exposure.keys = {Keyframe{Ticks{0}, 0.5}, Keyframe{Ticks{kTicksPerSecond / 5}, 1.5}};
    s = ok(edit::addEffect(s, tl.seq, std::span(&v, 1), "wabcut.color.grade",
                           {EffectParam{"basic.exposure", ParamValue(exposure)}, EffectParam{"basic.contrast", ParamValue(23.0)},
                            EffectParam{"basic.saturation", ParamValue(120.0)}, EffectParam{"creative.saturation", ParamValue(80.0)},
                            EffectParam{"creative.vibrance", ParamValue(42.0)}, EffectParam{"vignette.enabled", ParamValue(0.0)}}))
            .state;
    {
        const Clip& clip = track(*s.findSequence(tl.seq), "V1").clips[0];
        edit::EffectChange change;
        change.enabled = false;
        s = ok(edit::setEffect(s, tl.seq, v, clip.effects().front().id, change)).state;
    }
    const auto exported = exportSequence(s, tl.seq, Format::Fcp7Xml);
    REQUIRE(exported.has_value());
    const std::string& xml = exported->data;
    const auto has = [&](const std::string& pattern) { return std::regex_search(xml, std::regex(pattern)); };
    // Premiere's shape: effect id "Lumetri", an empty name, no category, pproBypass, numeric ids, key-list values.
    CHECK(has("<name\\s*/>\\s*<effectid>Lumetri</effectid>\\s*<effecttype>filter</effecttype>\\s*<mediatype>video</mediatype>"
              "\\s*<pproBypass>true</pproBypass>"));
    CHECK(xml.find("AE.ADBE Lumetri") == std::string::npos);
    const auto value = [&](const std::string& id, const std::string& name, const std::string& text) {
        return has("<parameter authoringApp=\"PremierePro\">\\s*<parameterid>" + id + "</parameterid>\\s*<name>" + name +
                   "</name>\\s*<value>" + text + "</value>");
    };
    CHECK(value("7", "Temperature", "-91445760000000000,0,0,0,0,0,0,0"));
    CHECK(value("11", "Exposure", "0,0.5,0,0,0,0,0,0;50803200000,1.5,0,0,0,0,0,0"));
    CHECK(value("12", "Contrast", "-91445760000000000,23,0,0,0,0,0,0"));
    CHECK(value("20", "Saturation", "-91445760000000000,120,0,0,0,0,0,0"));
    CHECK(value("30", "Vibrance", "-91445760000000000,42,0,0,0,0,0,0"));
    CHECK(value("31", "Saturation", "-91445760000000000,80,0,0,0,0,0,0"));
    CHECK(has("<parameterid>3</parameterid>\\s*<name>\\s*</name>\\s*<value>-91445760000000000,true,0,0,0,0,0,0</value>"));
    CHECK(has("<parameterid>50</parameterid>\\s*<name>\\s*</name>\\s*<value>-91445760000000000,false,0,0,0,0,0,0</value>"));
    CHECK(has("<parameter authoringApp=\"Wabcut\">\\s*<parameterid>wabcut:effect</parameterid>")); // Wabcut's own round trip

    // Read back as another editor would (Premiere's parameters only): every value, the keys and the switches.
    const std::string native = std::regex_replace(xml, std::regex("<parameter authoringApp=\"Wabcut\">[\\s\\S]*?</parameter>"), "");
    const ImportResult other = importText(native, Format::Fcp7Xml);
    const EffectInstance& grade = effectAt(track(sequenceAt(other, 0), "V1").clips[0], 0);
    CHECK(grade.effect == "wabcut.color.grade");
    CHECK_FALSE(grade.enabled);
    REQUIRE(grade.find("basic.exposure") != nullptr);
    REQUIRE(grade.find("basic.exposure")->curve() != nullptr);
    CHECK(grade.find("basic.exposure")->curve()->keys == exposure.keys);
    CHECK(param(grade, "basic.saturation") == 120.0);
    CHECK(param(grade, "creative.saturation") == 80.0);
    CHECK(param(grade, "creative.vibrance") == 42.0);
    CHECK(param(grade, "vignette.enabled") == 0.0);
    CHECK(grade.find("creative.enabled") == nullptr);
}
