// 0.6 (card #74): auditions through interchange. FCPXML writes Final Cut's <audition> (the active take first, as its
// active pick) and OTIO keeps the takes in Wabcut's metadata; both come back as an audition with every take, its
// effects and the picture and sound together. FCP7 XML and EDL have no auditions: they write the active take and
// report the others. An FCPXML audition from Final Cut becomes an audition with its picks as takes.

#include "InterchangeTestSupport.h"
#include "TimelineFixture.h"

#include <wabcut/interchange/Export.h>
#include <wabcut/model/Auditions.h>
#include <wabcut/model/FinishingOps.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::interchange;
using wabcut::test::f;
using wabcut::test::ok;
namespace it = wabcut::interchange::test;

namespace {

constexpr auto O = ReportOutcome::Carried;
constexpr auto D = ReportOutcome::Dropped;

/// V1 + A1: an audition of "Calm" (A from 0) and "Angry" (B from 5, with a blur on its picture), "Angry" active.
struct Auditioned {
    wabcut::test::Timeline tl{R"(
        V2 |B5bbbb....|
        V1 |Aaaaaa....|
        A1 |Aaaaaa....|
        A2 |B5bbbb....|
    )"};
    ProjectState state;

    Auditioned() {
        tl.link("V1@0 A1@0").link("V2@0 A2@0");
        const ClipId a = tl.clip("V1@0");
        const ClipId b = tl.clip("V2@0");
        ProjectState s = ok(edit::addEffect(tl.state, tl.seq, std::span(&b, 1), "wabcut.blur.gaussian",
                                            {EffectParam{"radius", ParamValue(6.0)}}))
                             .state;
        const ClipId ids[] = {a, b};
        s = ok(edit::createAudition(s, tl.seq, ids)).state;
        s = ok(edit::renameAuditionTake(s, tl.seq, a, 0, "Calm")).state;
        s = ok(edit::renameAuditionTake(s, tl.seq, a, 1, "Angry")).state;
        state = ok(edit::setActiveTake(s, tl.seq, a, 1)).state;
    }
};

std::string mediaName(const ProjectState& s, const Clip& c) {
    const MediaItem* m = s.findMedia(c.media);
    return m ? m->name : std::string("?");
}

std::vector<std::string> takeNames(const Clip& c) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < takeCount(c); ++i) {
        out.push_back(takeName(c, i));
    }
    return out;
}

} // namespace

TEST_CASE("Interchange 0.6: an audition survives FCPXML and OTIO with its takes and its active take",
          "[interchange][auditions]") {
    const Auditioned project;
    for (const Format format : {Format::FcpXml, Format::Otio}) {
        CAPTURE(displayName(format));
        const auto exported = exportSequence(project.state, project.tl.seq, format);
        REQUIRE(exported.has_value());
        CHECK(it::hasEntry(exported->report, O, ReportCategory::Clip, "audition 'Angry' with 2 takes"));
        const ImportResult back = it::importText(exported->data, format, {}, "auditions" + exported->extension);
        const Sequence& seq = it::sequenceAt(back, 0);
        const Clip& picture = it::track(seq, "V1").clips[0];
        const Clip& sound = it::track(seq, "A1").clips[0];
        REQUIRE(takeCount(picture) == 2);
        REQUIRE(takeCount(sound) == 2);
        // The active take plays as it did: B from frame 5 with its blur, picture and sound linked.
        CHECK(mediaName(back.project, picture) == "B");
        CHECK(picture.sourceIn == f(5));
        CHECK(picture.timelineRange == TimeRange{f(0), f(6)});
        REQUIRE(picture.effects().size() == 1);
        CHECK(picture.effects().front().effect == "wabcut.blur.gaussian");
        CHECK(it::links(seq) == it::Strings{"V1@0+A1@0"});
        // The takes' scratch tracks are gone: nothing but the audition is left on the timeline.
        CHECK(seq.videoTracks.size() <= 2);
        CHECK(seq.audioTracks.size() <= 2);
        seq.forEachTrack([&](const Track& t) { CHECK(t.clips.size() == (t.id == it::track(seq, "V1").id ||
                                                                       t.id == it::track(seq, "A1").id ? 1u : 0u)); });
        // FCPXML lists Final Cut's active pick first; OTIO keeps Wabcut's take order.
        const std::size_t calm = format == Format::FcpXml ? 1 : 0;
        CHECK(takeNames(picture) == (format == Format::FcpXml ? std::vector<std::string>{"Angry", "Calm"}
                                                               : std::vector<std::string>{"Calm", "Angry"}));
        CHECK(picture.audition()->active == 1 - calm);
        // The other take: A from frame 0, no effects; using it switches the sound too.
        const ProjectState other = ok(edit::setActiveTake(back.project, seq.id, picture.id, calm)).state;
        const Sequence& switched = *other.findSequence(seq.id);
        const Clip& calmPicture = it::track(switched, "V1").clips[0];
        CHECK(mediaName(other, calmPicture) == "A");
        CHECK(calmPicture.sourceIn == f(0));
        CHECK(calmPicture.effects().empty());
        CHECK(mediaName(other, it::track(switched, "A1").clips[0]) == "A");
    }
}

TEST_CASE("Interchange 0.6: FCP7 XML and EDL write the active take of an audition and report the others",
          "[interchange][auditions]") {
    const Auditioned project;
    for (const Format format : {Format::Fcp7Xml, Format::Edl}) {
        CAPTURE(displayName(format));
        const auto exported = exportSequence(project.state, project.tl.seq, format);
        REQUIRE(exported.has_value());
        CHECK(it::hasEntry(exported->report, D, ReportCategory::Clip,
                           "audition 'Angry': its active take is written; the other 1 take ('Calm') is not: " +
                               std::string(displayName(format)) + " has no auditions"));
    }
    const auto fcp7 = exportSequence(project.state, project.tl.seq, Format::Fcp7Xml);
    const ImportResult back = it::importText(fcp7->data, Format::Fcp7Xml, {}, "auditions.xml");
    const Sequence& seq = it::sequenceAt(back, 0);
    const Clip& picture = it::track(seq, "V1").clips[0];
    CHECK_FALSE(picture.isAudition());
    CHECK(mediaName(back.project, picture) == "B");
    CHECK(picture.sourceIn == f(5));
}

TEST_CASE("Interchange 0.6: an FCPXML audition from Final Cut becomes an audition of its picks",
          "[interchange][auditions]") {
    const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<fcpxml version="1.9"><resources>
<format id="r1" frameDuration="100/2500s" width="1280" height="720"/>
<asset id="a" name="A" start="0s" duration="100s" hasVideo="1" hasAudio="1" audioChannels="2" format="r1"><media-rep kind="original-media" src="file:///m/A.mov"/></asset>
<asset id="b" name="B" start="0s" duration="100s" hasVideo="1" format="r1" src="file:///m/B.mov"/>
</resources><project name="P"><sequence format="r1" tcStart="0s"><spine>
<audition>
  <asset-clip ref="a" offset="0s" name="Take 1" duration="4s" start="0s"/>
  <asset-clip ref="a" offset="0s" name="Take 2" duration="4s" start="8s"><asset-clip ref="b" lane="1" offset="8s" duration="1s" start="0s"/></asset-clip>
  <asset-clip ref="b" offset="0s" name="Take 3" duration="4s" start="10s"/>
</audition>
<asset-clip ref="a" offset="4s" name="After" duration="2s" start="0s"/>
</spine></sequence></project></fcpxml>)";
    const ImportResult r = it::importText(xml, Format::FcpXml);
    const Sequence& seq = it::sequenceAt(r, 0);
    CHECK(it::clips(seq, "V1") == it::Strings{"0+100 Take 1 @0", "100+50 After @0"});
    CHECK(it::clips(seq, "A1") == it::Strings{"0+100 Take 1 @0", "100+50 After @0"});
    for (const char* label : {"V1", "A1"}) {
        const Clip& audition = it::track(seq, label).clips[0];
        REQUIRE(takeCount(audition) == 2);
        CHECK(takeNames(audition) == std::vector<std::string>{"Take 1", "Take 2"});
        CHECK(audition.audition()->active == 0);
        CHECK(auditionTakeClip(audition, 1).sourceIn == it::frames(200, kRate25));
    }
    seq.forEachTrack([&](const Track& t) { // the second pick's connected clip is not imported
        for (const Clip& c : t.clips) {
            CHECK(mediaName(r.project, c) == "A");
        }
    });
    CHECK(it::hasEntry(r.report, O, ReportCategory::Clip, "audition 'Take 1' with 2 takes (the first pick is the active take)"));
    CHECK(it::hasEntry(r.report, D, ReportCategory::Clip,
                       "audition pick 'Take 3' does not have the picture and sound of the active pick 'Take 1'"));
    CHECK(it::hasEntry(r.report, D, ReportCategory::Clip, "the connected clips of audition pick 'Take 2' were not imported"));
}
