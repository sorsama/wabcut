// 0.6 (cards #69 and #67): generators and effects through interchange. One sequence with every synthetic kind (and an
// adjustment layer) is exported to FCP7 XML, FCPXML and OTIO and imported again with its settings; an EDL reports each
// generator as dropped. FCPXML filters and OTIO Effect objects from other editors become Wabcut effects (well-known
// ones mapped by name, unknown ones kept as placeholders that an export writes back), and Wabcut's own effects -
// keyframes, data and switched-off ones included - survive FCPXML and OTIO exactly.

#include "InterchangeTestSupport.h"
#include "TimelineFixture.h"

#include <wabcut/interchange/Export.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Synthetic.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::interchange;
using wabcut::test::f;
using wabcut::test::ok;
namespace it = wabcut::interchange::test;

namespace {

constexpr auto O = ReportOutcome::Carried;
constexpr auto A = ReportOutcome::Approximated;
constexpr auto D = ReportOutcome::Dropped;

std::string valueText(const ParamValue& v) {
    std::ostringstream out;
    if (const Curve* c = v.curve()) {
        out << "keys[";
        for (const Keyframe& k : c->keys) {
            out << k.time.value << ":" << k.value << ":" << static_cast<int>(k.interpolation) << " ";
        }
        out << "]";
    } else {
        out << v.constant();
    }
    return out.str();
}

/// An effect stack as text: id, on/off, parameters (keyframes too) and data.
std::string fxText(const EffectStack& stack) {
    std::ostringstream out;
    for (const EffectInstance& e : stack) {
        out << "[" << e.effect << (e.enabled ? "" : " off");
        if (e.version != 1) {
            out << " v" << e.version;
        }
        for (const EffectParam& p : e.params) {
            out << " " << p.key << "=" << valueText(p.value);
        }
        for (const EffectData& d : e.data) {
            out << " " << d.key << ":";
            if (const auto* points = d.points()) {
                for (const EffectPoint& p : *points) {
                    out << p.x << "," << p.y << ";";
                }
            } else {
                out << "'" << *d.text() << "'";
            }
        }
        out << "]";
    }
    return out.str();
}

/// A track's clips: position, source In, the generator and its settings (or media name), effects.
std::vector<std::string> trackText(const ProjectState& s, const Sequence& seq, TrackKind kind, std::size_t index) {
    std::vector<std::string> out;
    const auto& tracks = seq.tracks(kind);
    if (index >= tracks.size()) {
        return out;
    }
    for (const Clip& c : tracks[index]->clips) {
        std::ostringstream line;
        line << c.start().value << "+" << c.duration().value << " in " << c.sourceIn.value << (c.enabled ? "" : " off");
        if (const MediaItem* m = s.findMedia(c.media)) {
            line << " '" << m->name << "'";
            if (m->generator) {
                line << " " << m->generator->kind;
                for (const EffectParam& p : m->generator->params) {
                    line << " " << p.key << "=" << valueText(p.value);
                }
            }
        }
        line << " " << fxText(c.effects());
        out.push_back(line.str());
    }
    return out;
}

const Clip& first(const Track& t) {
    REQUIRE_FALSE(t.clips.empty());
    return t.clips[0];
}

std::string joined(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& l : lines) {
        out += l + "\n";
    }
    return out;
}

std::string emptyTracks(const std::vector<std::string>& labels, std::size_t frames) {
    std::string picture;
    for (const std::string& l : labels) {
        picture += l + " |" + std::string(frames, '.') + "|\n";
    }
    return picture;
}

MediaId addSynthetic(ProjectState& s, SequenceId seq, SyntheticKind kind, std::vector<EffectParam> params,
                     const std::string& name = {}) {
    auto added = edit::addSyntheticMedia(s, kind, std::move(params), seq, {}, name);
    REQUIRE(added.has_value());
    s = added->state;
    return added->id;
}

/// V3: transparent video (with a blur) and an adjustment layer (with a blur); V2: a colour matte and black video;
/// V1 + A1: bars and tone, then the counting leader, each picture linked with its sound.
struct GeneratorProject {
    ProjectState state;
    SequenceId seq;
};

GeneratorProject generatorProject() {
    wabcut::test::Timeline tl(emptyTracks({"V3", "V2", "V1 tgt", "A1 tgt"}, 300));
    ProjectState s = tl.state;
    const SequenceId seq = tl.seq;
    const MediaId bars = addSynthetic(s, seq, SyntheticKind::BarsAndTone,
                                      {EffectParam{"pattern", ParamValue(1.0)}, EffectParam{"tone.frequency", ParamValue(440.0)},
                                       EffectParam{"tone.level", ParamValue(-18.0)}});
    const MediaId leader = addSynthetic(s, seq, SyntheticKind::CountingLeader,
                                        {EffectParam{"cueBlipOnOut", ParamValue(1.0)}, EffectParam{"wipe.r", ParamValue(0.1)}});
    const MediaId matte = addSynthetic(s, seq, SyntheticKind::ColorMatte,
                                       {EffectParam{"color.b", ParamValue(0.6)}, EffectParam{"color.g", ParamValue(0.4)},
                                        EffectParam{"color.r", ParamValue(0.2)}},
                                       "Sky");
    const MediaId black = addSynthetic(s, seq, SyntheticKind::BlackVideo, {});
    const MediaId clear = addSynthetic(s, seq, SyntheticKind::TransparentVideo, {});
    const TrackId v1 = tl.track("V1");
    const TrackId v2 = tl.track("V2");
    const TrackId v3 = tl.track("V3");
    const TrackId a1 = tl.track("A1");
    const StreamRef video{StreamKind::Video, 0};
    const StreamRef audio{StreamKind::Audio, 0};
    const std::vector<edit::ClipPlacement> placements{
        {v1, bars, video, f(0), f(50), Ticks{}},     {a1, bars, audio, f(0), f(50), Ticks{}},
        {v1, leader, video, f(50), f(200), Ticks{}}, {a1, leader, audio, f(50), f(200), Ticks{}},
        {v2, matte, video, f(0), f(25), Ticks{}},    {v2, black, video, f(30), f(25), Ticks{}},
        {v3, clear, video, f(0), f(40), Ticks{}},
    };
    const auto placed = ok(edit::placeClips(s, seq, placements));
    s = placed.state;
    const std::vector<std::vector<ClipId>> groups{{placed.placed[0], placed.placed[1]}, {placed.placed[2], placed.placed[3]}};
    s = ok(edit::linkClipGroups(s, seq, groups)).state;
    const ClipId transparent = placed.placed[6];
    s = ok(edit::addEffect(s, seq, std::span(&transparent, 1), "wabcut.blur.gaussian", {EffectParam{"radius", ParamValue(5.0)}}))
            .state;
    const auto adjustment = ok(edit::addAdjustmentLayer(s, seq, v3, f(60), f(40)));
    s = adjustment.state;
    const ClipId layer = wabcut::test::clipAt(s, seq, "V3@60");
    s = ok(edit::addEffect(s, seq, std::span(&layer, 1), "wabcut.blur.gaussian", {EffectParam{"radius", ParamValue(8.0)}})).state;
    return {s, seq};
}

/// FCPXML around a spine, with asset "a" (picture and sound) and extra resources.
std::string fcpxml(const std::string& spine, const std::string& extraResources) {
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<fcpxml version="1.10"><resources>
<format id="r1" frameDuration="100/2500s" width="1280" height="720"/>
<asset id="a" name="A" start="0s" duration="100s" hasVideo="1" hasAudio="1" audioChannels="2" format="r1"><media-rep kind="original-media" src="file:///m/A.mov"/></asset>
)" + extraResources +
           R"(</resources><library><event name="E"><project name="P"><sequence format="r1" tcStart="0s"><spine>)" + spine +
           "</spine></sequence></project></event></library></fcpxml>";
}

} // namespace

TEST_CASE("interchange 0.6: every synthetic kind survives FCP7 XML FCPXML and OTIO with its settings",
          "[interchange][generators][export]") {
    const GeneratorProject project = generatorProject();
    const Sequence& original = *project.state.findSequence(project.seq);
    for (const Format format : {Format::Fcp7Xml, Format::FcpXml, Format::Otio}) {
        INFO(toString(format));
        const auto exported = exportSequence(project.state, project.seq, format);
        REQUIRE(exported.has_value());
        INFO(exported->data);
        const ImportResult back = it::importText(exported->data, format, {}, "generators" + exported->extension);
        REQUIRE_FALSE(back.sequences.empty());
        const Sequence& seq = *back.project.findSequence(back.sequences.front());
        for (const auto& [kind, index] : {std::pair{TrackKind::Video, std::size_t{0}}, std::pair{TrackKind::Video, std::size_t{1}},
                                          std::pair{TrackKind::Video, std::size_t{2}}, std::pair{TrackKind::Audio, std::size_t{0}}}) {
            INFO((kind == TrackKind::Video ? "V" : "A") << index + 1);
            const auto want = trackText(project.state, original, kind, index);
            const auto got = trackText(back.project, seq, kind, index);
            INFO("want:\n" << joined(want) << "got:\n" << joined(got));
            CHECK(want == got);
        }
        CHECK(wabcut::test::links(back.project, back.sequences.front()) == "V1@0+A1@0 V1@50+A1@50");
        // Generated items need no file: nothing offline, nothing to relink.
        CHECK(back.unresolvedMedia.empty());
        CHECK(it::hasEntry(back.report, O, ReportCategory::Media, "generated 'Sky' (Color Matte, no file)"));
        CHECK(it::hasEntry(back.report, O, ReportCategory::Media, "generated 'Universal Counting Leader' (Universal Counting Leader, no file)"));
        CHECK_FALSE(it::hasEntry(exported->report, D, ReportCategory::Graphic, "cannot describe"));
    }
    // FCP7 XML: Premiere's and FCP7's own generators where they have one.
    const auto fcp7 = exportSequence(project.state, project.seq, Format::Fcp7Xml);
    REQUIRE(fcp7.has_value());
    CHECK(fcp7->data.find("<effectid>Color</effectid>") != std::string::npos);
    CHECK(fcp7->data.find("<effectid>Slug</effectid>") != std::string::npos);
    CHECK(fcp7->data.find("<effectid>Bars and Tone</effectid>") != std::string::npos);
    CHECK(fcp7->data.find("<effectid>wabcut.synthetic.countingLeader</effectid>") != std::string::npos);
    // FCPXML: Final Cut's Custom solid; the leader as a Wabcut generator.
    const auto fcpx = exportSequence(project.state, project.seq, Format::FcpXml);
    REQUIRE(fcpx.has_value());
    CHECK(fcpx->data.find("Solids.localized/Custom.localized/Custom.motn") != std::string::npos);
    CHECK(fcpx->data.find("uid=\"com.wabcut.generator/wabcut.synthetic.countingLeader\"") != std::string::npos);
    // OTIO: GeneratorReferences other tools know.
    const auto otio = exportSequence(project.state, project.seq, Format::Otio);
    REQUIRE(otio.has_value());
    CHECK(otio->data.find("\"generator_kind\": \"SolidColor\"") != std::string::npos);
    CHECK(otio->data.find("\"generator_kind\": \"SMPTEBars\"") != std::string::npos);
}

TEST_CASE("interchange 0.6: an EDL reports each generator as dropped with its name and length",
          "[interchange][generators][export]") {
    const GeneratorProject project = generatorProject();
    const auto edl = exportSequence(project.state, project.seq, Format::Edl);
    REQUIRE(edl.has_value());
    for (const char* name : {"'Bars and Tone' (Bars and Tone, ", "'Universal Counting Leader' (Universal Counting Leader, ",
                             "'Sky' (Color Matte, ", "'Black Video' (Black Video, ", "'Transparent Video' (Transparent Video, ",
                             "'Adjustment Layer' (Adjustment Layer, "}) {
        INFO(name);
        CHECK(it::hasEntry(edl->report, D, ReportCategory::Clip, std::string("generated ") + name));
    }
    CHECK(it::hasEntry(edl->report, D, ReportCategory::Clip, "an EDL has no generators"));
}

TEST_CASE("interchange 0.6: generators from other editors become synthetic items", "[interchange][generators]") {
    SECTION("FCPXML: a Final Cut solid becomes a colour matte; an unknown generator is reported") {
        const auto r = it::importText(
            fcpxml(R"(<video ref="solid" offset="0s" start="3600s" duration="2s" name="Red"><param name="Color" key="9999/1" value="1 0 0 1"/></video>
                      <video ref="noise" offset="2s" start="3600s" duration="1s" name="Static"/>)",
                   R"(<effect id="solid" name="Custom" uid=".../Generators.localized/Solids.localized/Custom.localized/Custom.motn"/>
                      <effect id="noise" name="Noise" uid=".../Generators.localized/Textures.localized/Noise.motn"/>)"),
            Format::FcpXml);
        const Sequence& seq = it::sequenceAt(r, 0);
        REQUIRE(it::track(seq, "V1").clips.size() == 1);
        const Clip& red = it::track(seq, "V1").clips[0];
        CHECK(red.sourceIn == Ticks{});
        const MediaItem* item = r.project.findMedia(red.media);
        REQUIRE(item);
        REQUIRE(syntheticOf(*item));
        CHECK(syntheticOf(*item)->kind == SyntheticKind::ColorMatte);
        CHECK(syntheticParam(*item, "color.r") == 1.0);
        CHECK(syntheticParam(*item, "color.g") == 0.0);
        CHECK(it::hasEntry(r.report, A, ReportCategory::Media, "Final Cut's 'Custom' became a Color Matte"));
        CHECK(it::hasEntry(r.report, D, ReportCategory::Clip, "generator 'Static' (no media file) is not supported yet"));
    }
    SECTION("OTIO: SolidColor and SMPTEBars references from another tool") {
        const std::string doc = R"({"OTIO_SCHEMA": "Timeline.1", "name": "t", "tracks": {"OTIO_SCHEMA": "Stack.1", "children": [
            {"OTIO_SCHEMA": "Track.1", "kind": "Video", "name": "", "children": [
              {"OTIO_SCHEMA": "Clip.2", "name": "bars",
               "source_range": {"OTIO_SCHEMA": "TimeRange.1", "start_time": {"OTIO_SCHEMA": "RationalTime.1", "value": 0, "rate": 25},
                                "duration": {"OTIO_SCHEMA": "RationalTime.1", "value": 25, "rate": 25}},
               "media_references": {"DEFAULT_MEDIA": {"OTIO_SCHEMA": "GeneratorReference.1", "generator_kind": "SMPTEBars", "parameters": {}}},
               "active_media_reference_key": "DEFAULT_MEDIA"},
              {"OTIO_SCHEMA": "Clip.2", "name": "teal",
               "source_range": {"OTIO_SCHEMA": "TimeRange.1", "start_time": {"OTIO_SCHEMA": "RationalTime.1", "value": 0, "rate": 25},
                                "duration": {"OTIO_SCHEMA": "RationalTime.1", "value": 10, "rate": 25}},
               "media_references": {"DEFAULT_MEDIA": {"OTIO_SCHEMA": "GeneratorReference.1", "generator_kind": "SolidColor",
                                                      "parameters": {"color": "#008080"}}},
               "active_media_reference_key": "DEFAULT_MEDIA"}]}]}})";
        const auto r = it::importText(doc, Format::Otio);
        const Sequence& seq = it::sequenceAt(r, 0);
        REQUIRE(it::track(seq, "V1").clips.size() == 2);
        const MediaItem* bars = r.project.findMedia(it::track(seq, "V1").clips[0].media);
        const MediaItem* teal = r.project.findMedia(it::track(seq, "V1").clips[1].media);
        REQUIRE((bars && teal && syntheticOf(*bars) && syntheticOf(*teal)));
        CHECK(syntheticOf(*bars)->kind == SyntheticKind::BarsAndTone);
        CHECK(syntheticOf(*teal)->kind == SyntheticKind::ColorMatte);
        CHECK(syntheticParam(*teal, "color.g") == 128.0 / 255.0);
        CHECK(it::hasEntry(r.report, A, ReportCategory::Media, "SMPTE bars (HD) and a 1 kHz tone"));
    }
    SECTION("FCP7 XML: Color and Bars and Tone generators") {
        const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?><xmeml version="5"><sequence id="s"><name>S</name>
            <rate><timebase>25</timebase></rate><media><video><track>
            <generatoritem id="g1"><name>Blue</name><rate><timebase>25</timebase></rate><start>0</start><end>25</end><in>0</in><out>25</out>
              <effect><name>Color</name><effectid>Color</effectid><effecttype>generator</effecttype><mediatype>video</mediatype>
              <parameter><parameterid>fillcolor</parameterid><name>Color</name><value><alpha>255</alpha><red>0</red><green>0</green><blue>255</blue></value></parameter>
              </effect></generatoritem>
            <generatoritem id="g2"><name>HD Bars and Tone</name><rate><timebase>25</timebase></rate><start>25</start><end>50</end><in>0</in><out>25</out>
              <effect><name>HD Bars and Tone</name><effectid>HD Bars and Tone</effectid><effecttype>generator</effecttype><mediatype>video</mediatype></effect>
            </generatoritem></track></video></media></sequence></xmeml>)";
        const auto r = it::importText(xml, Format::Fcp7Xml);
        const Sequence& seq = it::sequenceAt(r, 0);
        REQUIRE(it::track(seq, "V1").clips.size() == 2);
        const MediaItem* blue = r.project.findMedia(it::track(seq, "V1").clips[0].media);
        const MediaItem* bars = r.project.findMedia(it::track(seq, "V1").clips[1].media);
        REQUIRE((blue && bars && syntheticOf(*blue) && syntheticOf(*bars)));
        CHECK(blue->name == "Blue");
        CHECK(syntheticParam(*blue, "color.b") == 1.0);
        CHECK(syntheticParam(*blue, "color.r") == 0.0);
        CHECK(syntheticOf(*bars)->kind == SyntheticKind::BarsAndTone);
        CHECK(syntheticParam(*bars, "pattern") == 0.0); // HD: SMPTE RP 219
    }
}

TEST_CASE("interchange 0.6: FCPXML filters become effects and unknown ones round-trip as placeholders",
          "[interchange][effects][fcpxml]") {
    const std::string doc = fcpxml(
        R"(<asset-clip ref="a" offset="0s" start="0s" duration="4s" name="Shot">
             <filter-video ref="blur" name="Gaussian Blur"><param name="Amount" key="9999/gaussianBlur/1" value="12"/></filter-video>
             <filter-video ref="prism" name="Prism" enabled="0"><param name="Amount" key="1" value="30"/><param name="Angle" value="45"/></filter-video>
             <filter-audio ref="eq" name="Channel EQ"><param name="Gain" key="7" value="3"/></filter-audio>
           </asset-clip>)",
        R"(<effect id="blur" name="Gaussian Blur" uid="FFGaussianBlur"/>
           <effect id="prism" name="Prism" uid=".../Effects.localized/Blur.localized/Prism.localized/Prism.moef"/>
           <effect id="eq" name="Channel EQ" uid="FFAudioUnitEffect aufx chEQ appl"/>)");
    const auto r = it::importText(doc, Format::FcpXml);
    const Sequence& seq = it::sequenceAt(r, 0);
    const Clip& picture = first(it::track(seq, "V1"));
    const Clip& sound = first(it::track(seq, "A1"));
    CHECK(fxText(picture.effects()) ==
          "[wabcut.blur.gaussian radius=12 source.name:'Gaussian Blur' source.uid:'FFGaussianBlur']"
          "[fcpxml:Prism off key:Amount:'1' param:Amount:'30' param:Angle:'45' source.enabled:'0' source.name:'Prism' "
          "source.uid:'.../Effects.localized/Blur.localized/Prism.localized/Prism.moef']");
    CHECK(fxText(sound.effects()) ==
          "[wabcut.audio.eq3 key:Gain:'7' param:Gain:'3' source.name:'Channel EQ' source.uid:'FFAudioUnitEffect aufx chEQ appl']");
    CHECK(it::hasEntry(r.report, A, ReportCategory::Effect,
                       "effect 'Gaussian Blur' on 'Shot' as Wabcut's 'Gaussian Blur'; settings matched by name: Amount"));
    CHECK(it::hasEntry(r.report, A, ReportCategory::Effect,
                       "effect 'Prism' on 'Shot' kept as a placeholder: Wabcut does not render it"));
    CHECK(it::hasEntry(r.report, A, ReportCategory::Effect, "kept for export but not applied: Gain"));

    // Written back: the same effects, names, ids and settings; and read again unchanged.
    const auto exported = exportSequence(r.project, r.sequences.front(), Format::FcpXml);
    REQUIRE(exported.has_value());
    INFO(exported->data);
    CHECK(exported->data.find(R"(uid=".../Effects.localized/Blur.localized/Prism.localized/Prism.moef")") != std::string::npos);
    CHECK(exported->data.find(R"(<param name="Amount" key="1" value="30")") != std::string::npos);
    CHECK(exported->data.find(R"(name="Prism" enabled="0")") != std::string::npos);
    CHECK(exported->data.find(R"(uid="FFGaussianBlur")") != std::string::npos);
    CHECK(it::hasEntry(exported->report, O, ReportCategory::Effect, "(written back as it came)"));
    const auto again = it::importText(exported->data, Format::FcpXml);
    const Sequence& seq2 = it::sequenceAt(again, 0);
    CHECK(fxText(first(it::track(seq2, "V1")).effects()) == fxText(picture.effects()));
    CHECK(fxText(first(it::track(seq2, "A1")).effects()) == fxText(sound.effects()));
}

TEST_CASE("interchange 0.6: OTIO Effect objects on clips and tracks become effects and unknown ones round-trip",
          "[interchange][effects][otio]") {
    const std::string doc = R"({"OTIO_SCHEMA": "Timeline.1", "name": "t", "tracks": {"OTIO_SCHEMA": "Stack.1", "children": [
        {"OTIO_SCHEMA": "Track.1", "kind": "Video", "name": "",
         "effects": [{"OTIO_SCHEMA": "Effect.1", "name": "look", "effect_name": "FilmLook", "metadata": {"vendor": {"grain": 0.3}}}],
         "children": [
          {"OTIO_SCHEMA": "Clip.2", "name": "c",
           "source_range": {"OTIO_SCHEMA": "TimeRange.1", "start_time": {"OTIO_SCHEMA": "RationalTime.1", "value": 0, "rate": 25},
                            "duration": {"OTIO_SCHEMA": "RationalTime.1", "value": 25, "rate": 25}},
           "effects": [{"OTIO_SCHEMA": "Effect.1", "name": "soft", "effect_name": "Gaussian Blur", "metadata": {}},
                       {"OTIO_SCHEMA": "Effect.1", "name": "glow", "effect_name": "FancyGlow", "enabled": true,
                        "metadata": {"vendor": {"strength": 2}}}],
           "media_references": {"DEFAULT_MEDIA": {"OTIO_SCHEMA": "ExternalReference.1", "target_url": "file:///m/c.mov"}},
           "active_media_reference_key": "DEFAULT_MEDIA"}]}]}})";
    const auto r = it::importText(doc, Format::Otio);
    const Sequence& seq = it::sequenceAt(r, 0);
    const Clip& c = first(it::track(seq, "V1"));
    CHECK(fxText(c.effects()) == "[wabcut.blur.gaussian source.name:'Gaussian Blur']"
                                 "[otio:FancyGlow off source.enabled:'1' source.metadata:'{\"vendor\":{\"strength\":2}}' "
                                 "source.name:'glow']");
    CHECK(fxText(it::track(seq, "V1").effects) ==
          "[otio:FilmLook off source.enabled:'1' source.metadata:'{\"vendor\":{\"grain\":0.3}}' source.name:'look']");
    CHECK(it::hasEntry(r.report, A, ReportCategory::Effect, "effect 'FancyGlow' on 'c' kept as a placeholder"));
    CHECK(it::hasEntry(r.report, A, ReportCategory::Effect, "track effect 'look' of V1 (kept, not rendered"));

    const auto exported = exportSequence(r.project, r.sequences.front(), Format::Otio);
    REQUIRE(exported.has_value());
    INFO(exported->data);
    CHECK(exported->data.find("\"effect_name\": \"FancyGlow\"") != std::string::npos);
    CHECK(exported->data.find("\"strength\": 2") != std::string::npos);
    CHECK(exported->data.find("\"effect_name\": \"FilmLook\"") != std::string::npos);
    const auto again = it::importText(exported->data, Format::Otio);
    const Sequence& seq2 = it::sequenceAt(again, 0);
    CHECK(fxText(first(it::track(seq2, "V1")).effects()) == fxText(c.effects()));
    CHECK(fxText(it::track(seq2, "V1").effects) == fxText(it::track(seq, "V1").effects));
}

TEST_CASE("interchange 0.6: Wabcut effects with keyframes and data survive FCPXML and OTIO exactly",
          "[interchange][effects][export]") {
    wabcut::test::Timeline tl(R"(
        V1 tgt |A10aaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |A10aaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    tl.linkMatching();
    ProjectState s = tl.state;
    const ClipId v = tl.clip("V1@0");
    const ClipId a = tl.clip("A1@0");
    Curve radius;
    radius.keys = {Keyframe{f(10), 0.0}, Keyframe{f(20), 25.0}};
    radius.keys[1].interpolation = Interpolation::Hold;
    s = ok(edit::addEffect(s, tl.seq, std::span(&v, 1), "wabcut.blur.gaussian", {EffectParam{"radius", ParamValue(radius)}})).state;
    s = ok(edit::addEffect(s, tl.seq, std::span(&v, 1), "wabcut.color.basic",
                           {EffectParam{"exposure", ParamValue(1.5)}, EffectParam{"saturation", ParamValue(120.0)}}))
            .state;
    const auto grade = ok(edit::addEffect(s, tl.seq, std::span(&v, 1), "wabcut.color.grade"));
    s = grade.state;
    edit::EffectChange curves;
    curves.data = {EffectData{"curves.master", std::vector<EffectPoint>{{0.0, 0.0}, {0.5, 0.625}, {1.0, 1.0}}}};
    s = ok(edit::setEffect(s, tl.seq, v, grade.createdEffects.front(), curves)).state;
    s = ok(edit::addEffect(s, tl.seq, std::span(&a, 1), "wabcut.audio.compressor",
                           {EffectParam{"ratio", ParamValue(6.0)}, EffectParam{"threshold", ParamValue(-30.0)}}))
            .state;
    const auto eq = ok(edit::addEffect(s, tl.seq, std::span(&a, 1), "wabcut.audio.eq3", {EffectParam{"lowGain", ParamValue(-3.0)}}));
    s = eq.state;
    edit::EffectChange off;
    off.enabled = false;
    s = ok(edit::setEffect(s, tl.seq, a, eq.createdEffects.front(), off)).state;
    s = ok(edit::addEffect(s, tl.seq, edit::EffectTarget::ofTrack(tl.track("V1")), "wabcut.color.basic",
                           {EffectParam{"exposure", ParamValue(0.5)}}))
            .state;
    const Sequence& original = *s.findSequence(tl.seq);
    const std::string wantPicture = fxText(original.findClip(v)->clip().effects());
    const std::string wantSound = fxText(original.findClip(a)->clip().effects());
    REQUIRE(wantPicture.find("curves.master:0,0;0.5,0.625;1,1;") != std::string::npos);

    for (const Format format : {Format::FcpXml, Format::Otio}) {
        INFO(toString(format));
        const auto exported = exportSequence(s, tl.seq, format);
        REQUIRE(exported.has_value());
        INFO(exported->data);
        const auto back = it::importText(exported->data, format, {}, "effects" + exported->extension);
        const Sequence& seq = it::sequenceAt(back, 0);
        CHECK(fxText(first(it::track(seq, "V1")).effects()) == wantPicture);
        CHECK(fxText(first(it::track(seq, "A1")).effects()) == wantSound);
        if (format == Format::Otio) {
            CHECK(fxText(it::track(seq, "V1").effects) == fxText(original.videoTracks[0]->effects));
            CHECK(it::hasEntry(exported->report, O, ReportCategory::Effect, "V1 track effects Basic Correction"));
            CHECK(exported->data.find("\"OTIO_SCHEMA\": \"Effect.1\"") != std::string::npos);
        } else {
            CHECK(it::hasEntry(exported->report, D, ReportCategory::Effect, "1 track effect not written"));
            CHECK(it::hasEntry(exported->report, O, ReportCategory::Effect, "as Final Cut's 'Gaussian Blur'"));
            CHECK(it::hasEntry(exported->report, O, ReportCategory::Effect, "(other editors skip it; Wabcut reads it)"));
            CHECK(exported->data.find(R"(<filter-audio ref=)") != std::string::npos);
            CHECK(exported->data.find(R"(interp="hold")") != std::string::npos);
        }
    }
}
