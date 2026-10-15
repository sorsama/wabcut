// Auto reframe (AI-9) on fixture timelines and subject analyses: the reframed copy (ids renewed, links and
// transitions kept, the source untouched, the derivation recorded), the Transform keyframes it writes (the subject in
// frame, a jump exactly at each cut, nothing for a still subject), the modes and fit options, and the commands
// (dryRun preview, one undo step, rerun after an edit, a missing source, permissions).

#include "CommandHarness.h"
#include "TimelineFixture.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/reframe/ReframeCommands.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>
#include <set>

using namespace wabcut;
using namespace wabcut::test;
using namespace wabcut::ai::reframe;
using wabcut::ai::vision::Box;
using wabcut::ai::vision::SubjectAnalysis;
using wabcut::ai::vision::SubjectBox;
using wabcut::ai::vision::SubjectSample;
using Catch::Approx;

namespace {

Ticks sec(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

/// An analysis of `seconds` of 1920 x 1080 video sampled at 5 per second, with cuts, and the faces `at` returns.
SubjectAnalysis analysis(double seconds, std::vector<double> cuts, const std::function<std::vector<SubjectBox>(double)>& at) {
    SubjectAnalysis a;
    a.frameWidth = 1920;
    a.frameHeight = 1080;
    a.start = Ticks{0};
    a.end = sec(seconds);
    a.models = "fixture/1";
    for (const double c : cuts) {
        a.cuts.push_back(sec(c));
    }
    for (double t = 0.1; t < seconds; t += 0.2) {
        SubjectSample s;
        s.time = sec(t);
        s.faces = at(t);
        a.samples.push_back(std::move(s));
    }
    return a;
}

SubjectBox faceAt(float cx, float cy, int track, float activity = 0.0f) {
    return SubjectBox{Box{cx - 0.05f, cy - 0.09f, 0.1f, 0.18f}, 0.9f, track, activity};
}

/// "V1 |A....|" of `frames` frames at 25 fps (media A: 1920 x 1080, 1000 frames).
std::string clipPicture(int frames) { return "V1 |A" + std::string(static_cast<std::size_t>(frames - 1), 'a') + "|"; }

SubjectSource fixed(SubjectAnalysis a) {
    return [a = std::move(a)](const MediaItem&, int) -> std::expected<SubjectAnalysis, Error> { return a; };
}

const Clip& onlyVideoClip(const ProjectState& state, SequenceId seq, std::size_t track = 0) {
    const Sequence& s = *state.findSequence(seq);
    REQUIRE(s.videoTracks.size() > track);
    REQUIRE_FALSE(s.videoTracks[track]->clips.empty());
    return s.videoTracks[track]->clips.front();
}

/// The window centre (0..1 of the picture) a 1080 x 1920 frame shows at source time `t` of a fill-scaled clip.
double centreXAt(const Clip& clip, Ticks t) {
    const double scale = clip.properties().value(ParamId::Scale, t);
    return 0.5 - clip.properties().value(ParamId::PositionX, t) / (scale * 1920.0);
}

} // namespace

TEST_CASE("Reframing to 9:16 follows the subject with sparse keyframes and cuts with the shots", "[ai][reframe]") {
    const Timeline tl(clipPicture(100)); // 4 s
    // Shot 1 (0-2 s): a face walking from 0.25 to 0.4; shot 2 (2-4 s): another face, still, at 0.75.
    const auto subjects = analysis(40.0, {2.0}, [](double t) {
        return t < 2.0 ? std::vector<SubjectBox>{faceAt(static_cast<float>(0.25 + 0.075 * t), 0.35f, 1)}
                       : std::vector<SubjectBox>{faceAt(0.75f, 0.4f, 2)};
    });
    ReframeOptions options;
    auto result = reframeSequence(tl.state, tl.seq, options, fixed(subjects));
    REQUIRE(result);
    REQUIRE(validate(result->state));
    CHECK(result->warnings.empty());
    CHECK(result->clipsReframed == 1);
    CHECK(result->state.sequences.size() == 2);
    CHECK(result->state.sequencePtr(tl.seq) == tl.state.sequencePtr(tl.seq)); // the source is untouched (shared)

    const Sequence& made = *result->state.findSequence(result->sequence);
    CHECK(made.width == 1080);
    CHECK(made.height == 1920);
    CHECK(made.name == tl.sequence().name + " (9:16)");
    REQUIRE(made.derivedFrom);
    CHECK(made.derivedFrom->source == tl.seq);
    CHECK(made.derivedFrom->generator == "reframe/1");
    CHECK(optionsFromJson(made.derivedFrom->params)->aspect == Aspect{9, 16});

    const Clip& clip = onlyVideoClip(result->state, result->sequence);
    CHECK(clip.properties().value(ParamId::Scale) == Approx(1920.0 / 1080.0)); // fills the frame's height
    CHECK(clip.properties()[ParamId::Scale].isConstant());
    REQUIRE(clip.properties()[ParamId::PositionX].isCurve());
    const Curve& x = *clip.properties()[ParamId::PositionX].curve();
    CHECK(x.keys.size() <= 12); // sparse
    CHECK(x.isWellFormed());
    CHECK(clip.properties().firstInvalid() == std::nullopt);

    // The face is inside the 9:16 window at every sample, in both shots.
    const double half = (1080.0 * 1080.0 / 1920.0) / 1920.0 / 2.0; // the window's half width, 0..1 of the picture
    for (const SubjectSample& s : subjects.samples) {
        if (s.time >= sec(4.0)) {
            break;
        }
        const double cx = centreXAt(clip, s.time);
        const Box& face = s.faces[0].box;
        INFO("t = " << s.time.toSeconds());
        CHECK(face.x >= cx - half - 1e-3);
        CHECK(face.x + face.w <= cx + half + 1e-3);
    }
    // The framing jumps exactly at the cut: the old shot's framing up to it, the new one from its first frame.
    CHECK(centreXAt(clip, sec(1.96)) < 0.5);
    CHECK(centreXAt(clip, sec(2.0)) == Approx(0.75).margin(0.06));
    CHECK(result->plans.begin()->second.cuts == std::vector<Ticks>{sec(2.0)});
    CHECK(result->plans.begin()->second.guide == "faces");
}

TEST_CASE("A still subject gets no motion; no analysis keeps clips centred", "[ai][reframe]") {
    const Timeline tl(clipPicture(75));
    const auto still = analysis(40.0, {}, [](double) { return std::vector<SubjectBox>{faceAt(0.62f, 0.4f, 1)}; });
    auto result = reframeSequence(tl.state, tl.seq, ReframeOptions{}, fixed(still));
    REQUIRE(result);
    const Clip& clip = onlyVideoClip(result->state, result->sequence);
    CHECK(clip.properties()[ParamId::PositionX].isConstant());
    CHECK(clip.properties()[ParamId::PositionY].isConstant());
    CHECK(centreXAt(clip, sec(1.0)) == Approx(0.62).margin(0.03));
    CHECK(result->keyframes == 0);

    const SubjectSource failing = [](const MediaItem&, int) -> std::expected<SubjectAnalysis, Error> {
        return std::unexpected(Error(ErrorCode::NotFound, "offline"));
    };
    auto centred = reframeSequence(tl.state, tl.seq, ReframeOptions{}, failing);
    REQUIRE(centred);
    REQUIRE(centred->warnings.size() == 1);
    CHECK(centred->warnings[0].find("offline") != std::string::npos);
    const Clip& middle = onlyVideoClip(centred->state, centred->sequence);
    CHECK(middle.properties().value(ParamId::PositionX) == Approx(0.0).margin(0.5));
    CHECK(middle.properties()[ParamId::PositionX].isConstant());

    ReframeOptions anchored;
    anchored.mode = ReframeMode::Anchor;
    anchored.anchorX = 0.2;
    auto anchor = reframeSequence(tl.state, tl.seq, anchored, {});
    REQUIRE(anchor);
    CHECK(centreXAt(onlyVideoClip(anchor->state, anchor->sequence), sec(1.0)) == Approx(0.2).margin(0.03));
}

TEST_CASE("Faces that cannot fit: crop, letterbox or blur fill", "[ai][reframe]") {
    const Timeline tl(clipPicture(50));
    const auto pair = analysis(40.0, {}, [](double) {
        return std::vector<SubjectBox>{faceAt(0.15f, 0.4f, 1), faceAt(0.85f, 0.4f, 2)};
    });
    const double fill = 1920.0 / 1080.0;
    ReframeOptions faces;
    faces.mode = ReframeMode::Faces;
    auto cropped = reframeSequence(tl.state, tl.seq, faces, fixed(pair));
    REQUIRE(cropped);
    CHECK(onlyVideoClip(cropped->state, cropped->sequence).properties().value(ParamId::Scale) == Approx(fill));

    faces.fit = FitMode::Letterbox;
    auto boxed = reframeSequence(tl.state, tl.seq, faces, fixed(pair));
    REQUIRE(boxed);
    const Clip& zoomed = onlyVideoClip(boxed->state, boxed->sequence);
    const double scale = zoomed.properties().value(ParamId::Scale);
    CHECK(scale < fill);
    CHECK(1920.0 * 0.8 * scale <= 1080.0 + 1.0); // both faces (0.1 .. 0.9 of the picture) fit the 1080-pixel width
    CHECK(boxed->plans.begin()->second.zoomedOut);
    CHECK(boxed->state.findSequence(boxed->sequence)->videoTracks.size() == 1);

    faces.fit = FitMode::BlurFill;
    auto blurred = reframeSequence(tl.state, tl.seq, faces, fixed(pair));
    REQUIRE(blurred);
    REQUIRE(validate(blurred->state));
    const Sequence& made = *blurred->state.findSequence(blurred->sequence);
    REQUIRE(made.videoTracks.size() == 2);
    CHECK(made.videoTracks[0]->name == "Blur fill");
    const Clip& background = made.videoTracks[0]->clips.front();
    CHECK(background.timelineRange == zoomed.timelineRange);
    REQUIRE_FALSE(background.effects().empty());
    CHECK(background.effects().back().effect == "wabcut.blur.gaussian");
    CHECK(background.properties().value(ParamId::Scale) == Approx(fill)); // covers the frame
    CHECK(made.videoTracks[1]->clips.front().properties().value(ParamId::Scale) == Approx(scale));
}

TEST_CASE("Speaker mode follows whoever is talking", "[ai][reframe]") {
    const Timeline tl(clipPicture(150)); // 6 s
    const auto talk = analysis(40.0, {}, [](double t) {
        const bool leftTalks = t < 3.0;
        return std::vector<SubjectBox>{faceAt(0.2f, 0.4f, 1, leftTalks ? 0.08f : 0.0f), faceAt(0.8f, 0.4f, 2, leftTalks ? 0.0f : 0.08f)};
    });
    ReframeOptions speaker;
    speaker.mode = ReframeMode::Speaker;
    auto result = reframeSequence(tl.state, tl.seq, speaker, fixed(talk));
    REQUIRE(result);
    const Clip& clip = onlyVideoClip(result->state, result->sequence);
    CHECK(centreXAt(clip, sec(1.0)) == Approx(0.2).margin(0.08));
    CHECK(centreXAt(clip, sec(5.5)) == Approx(0.8).margin(0.08));
}

TEST_CASE("The reframed copy renews every id and keeps links, transitions, captions and effects", "[ai][reframe]") {
    Timeline tl(R"(
        V1 tgt |AaaaB5bbbbb|
        A1 tgt |AaaaB5bbbbb|
    )");
    tl.linkMatching();
    edit::TransitionSpec spec;
    spec.outgoing = tl.clip("V1@0");
    spec.incoming = tl.clip("V1@4");
    spec.before = f(2);
    spec.after = f(2);
    auto withTransition = edit::addTransition(tl.state, tl.seq, tl.track("V1"), spec);
    REQUIRE(withTransition);
    ProjectState state = withTransition->state;
    {
        auto seq = std::make_shared<Sequence>(*state.findSequence(tl.seq));
        seq->captionTracks.push_back(CaptionTrack{state.ids.allocate<CaptionTrackId>(), "Captions", "en", true, {CaptionCue{f(0), f(3), "hi"}}});
        seq->markers.push_back(Marker{f(2), {}, "beat", "", 1});
        state = state.withSequence(seq);
    }
    REQUIRE(validate(state));

    auto copy = duplicateSequence(state, tl.seq, "Copy", 1080, 1080);
    REQUIRE(copy);
    REQUIRE(validate(copy->state));
    const Sequence& a = *copy->state.findSequence(tl.seq);
    const Sequence& b = *copy->state.findSequence(copy->sequence);
    CHECK(b.width == 1080);
    CHECK(b.name == "Copy");
    CHECK(b.markers == a.markers);
    REQUIRE(b.captionTracks.size() == 1);
    CHECK(b.captionTracks[0].id != a.captionTracks[0].id);
    CHECK(b.captionTracks[0].cues == a.captionTracks[0].cues);
    std::set<std::uint64_t> oldIds;
    a.forEachTrack([&](const Track& t) {
        oldIds.insert(t.id.value);
        for (const Clip& c : t.clips) {
            oldIds.insert(c.id.value);
        }
    });
    std::set<std::uint64_t> links;
    b.forEachTrack([&](const Track& t) {
        CHECK_FALSE(oldIds.contains(t.id.value));
        for (const Clip& c : t.clips) {
            CHECK_FALSE(oldIds.contains(c.id.value));
            links.insert(c.linkGroup.value);
        }
    });
    CHECK(links.size() == 2); // the two A/V pairs are linked in the copy, by new groups
    const Track& v1 = *b.videoTracks[0];
    REQUIRE(v1.transitions.size() == 1);
    CHECK(v1.transitions[0].id != a.videoTracks[0]->transitions[0].id);
    CHECK(v1.transitions[0].outgoing == copy->clips.at(spec.outgoing.value));
    CHECK(v1.transitions[0].incoming == copy->clips.at(spec.incoming.value));
    CHECK(duplicateSequence(state, SequenceId{999999}, "x", 2, 2).error().code == ErrorCode::NotFound);
}

TEST_CASE("Options, aspects and output sizes", "[ai][reframe]") {
    CHECK(aspectFromString("9:16") == Aspect{9, 16});
    CHECK(aspectFromString("4:5") == Aspect{4, 5});
    CHECK_FALSE(aspectFromString("9-16"));
    CHECK_FALSE(aspectFromString("0:1"));
    CHECK_FALSE(aspectFromString("9:16x"));
    CHECK_FALSE(aspectFromString("101:1"));
    ReframeOptions o;
    CHECK(outputSize(1920, 1080, o) == std::pair{1080, 1920});
    o.aspect = {1, 1};
    CHECK(outputSize(1920, 1080, o) == std::pair{1080, 1080});
    o.aspect = {4, 5};
    CHECK(outputSize(1920, 1080, o) == std::pair{1080, 1350});
    o.aspect = {16, 9};
    CHECK(outputSize(1080, 1920, o) == std::pair{1920, 1080});
    o.width = 720;
    o.height = 1281;
    CHECK(outputSize(1920, 1080, o) == std::pair{720, 1282});

    ReframeOptions full;
    full.aspect = {4, 5};
    full.mode = ReframeMode::Anchor;
    full.fit = FitMode::BlurFill;
    full.smoothness = 0.8;
    full.anchorX = 0.25;
    full.anchorY = 0.3;
    const auto back = optionsFromJson(optionsToJson(full));
    REQUIRE(back);
    CHECK(back->aspect == full.aspect);
    CHECK(back->mode == ReframeMode::Anchor);
    CHECK(back->fit == FitMode::BlurFill);
    CHECK(back->smoothness == Approx(0.8));
    CHECK(back->anchorX == Approx(0.25));
    CHECK_FALSE(optionsFromJson("[1]"));
    CHECK_FALSE(optionsFromJson(R"({"mode":"dance"})"));

    VideoStreamInfo portrait;
    portrait.width = 1920;
    portrait.height = 1080;
    portrait.rotationDegrees = 90;
    CHECK(displayGeometry(portrait).width == 1080.0);
    VideoStreamInfo anamorphic;
    anamorphic.width = 1440;
    anamorphic.height = 1080;
    anamorphic.sampleAspect = Rate{4, 3};
    CHECK(displayGeometry(anamorphic).width == Approx(1920.0));
}

// --- The commands ------------------------------------------------------------------------------------------------------

namespace {

std::shared_ptr<command::CommandServices> servicesWith(MediaId media, SubjectAnalysis a) {
    auto provider = std::make_shared<ai::vision::MapSubjectProvider>();
    provider->add(media, std::move(a));
    return std::make_shared<ReframeServices>(provider);
}

} // namespace

TEST_CASE("reframe.createSequence previews, applies as one undo step, and reruns", "[ai][reframe][commands]") {
    Timeline tl(clipPicture(100));
    const MediaId media = tl.media('A');
    const auto subjects = analysis(40.0, {}, [](double t) { return std::vector<SubjectBox>{faceAt(static_cast<float>(0.3 + 0.05 * t), 0.4f, 1)}; });
    CommandHarness h(std::move(tl), command::EnginePolicy{command::CapabilitySet::all(), 1024, servicesWith(media, subjects)},
                     ai::edit::editingCommands());

    // Dry run: the change and a preview, nothing committed.
    const auto preview = h.run(Json{{"command", "reframe.createSequence"}, {"params", Json{{"aspect", "9:16"}}}, {"dryRun", true}});
    REQUIRE(preview.ok());
    CHECK_FALSE(preview.committed);
    REQUIRE(preview.preview);
    CHECK(preview.preview->sequences.size() == 2);
    CHECK(preview.changes.addedSequences.size() == 1);
    CHECK(h.state()->sequences.size() == 1);
    const Json& clips = preview.data["clips"];
    REQUIRE(clips.size() == 1);
    CHECK(clips[0]["guide"] == "faces");
    CHECK(clips[0]["path"].size() >= 10);
    CHECK(clips[0]["path"][0].size() == 5);
    CHECK_FALSE(clips[0]["subjects"].empty());

    // Applied: one undo step that adds the sequence and changes nothing else.
    const auto made = h.ok("reframe.createSequence", Json{{"aspect", "1:1"}, {"mode", "follow"}, {"smoothness", 0.7}});
    CHECK(made.committed);
    REQUIRE(made.createdIds.size() == 1);
    const SequenceId reframed{made.createdIds[0]};
    CHECK(made.changes.addedSequences == std::vector<SequenceId>{reframed});
    CHECK(made.changes.sequences.empty());
    CHECK(made.data["width"] == 1080);
    CHECK(made.data["height"] == 1080);
    CHECK(h.session.undoLabel() == "Auto Reframe");
    REQUIRE(h.session.undo());
    CHECK(h.state()->sequences.size() == 1);
    REQUIRE(h.session.redo());
    CHECK(h.state()->findSequence(reframed) != nullptr);

    // Edit the source (a ripple trim), then make the reframed sequence again: same id, name, new contents.
    const std::string name = h.state()->findSequence(reframed)->name;
    h.ok("clip.trim", Json{{"sequence", h.tl.seq.value}, {"clip", h.clip("V1@0").value}, {"edge", "tail"}, {"delta", "-00:00:02:00"}});
    const auto again = h.ok("reframe.rerun", Json{{"sequence", reframed.value}});
    const Sequence& rebuilt = *h.state()->findSequence(reframed);
    CHECK(rebuilt.name == name);
    CHECK(rebuilt.width == 1080);
    CHECK(rebuilt.videoTracks[0]->clips.front().duration() == h.sequence(h.state()).videoTracks[0]->clips.front().duration());
    CHECK(again.data["smoothness"] == 0.7); // the recorded parameters
    const auto other = h.ok("reframe.rerun", Json{{"sequence", reframed.value}, {"aspect", "4:5"}});
    CHECK(h.state()->findSequence(reframed)->height == 1350);
    CHECK(optionsFromJson(h.state()->findSequence(reframed)->derivedFrom->params)->aspect == Aspect{4, 5});

    // A plain sequence cannot be rerun; a deleted source is reported.
    CHECK(h.errorOf("reframe.rerun", Json{{"sequence", h.tl.seq.value}}) == "InvalidParams");
    {
        auto removed = edit::removeSequence(*h.state(), h.tl.seq);
        REQUIRE(removed);
        REQUIRE(h.session.apply("remove source", [&](const ProjectState&) { return std::expected<ProjectState, Error>(*removed); }));
    }
    const auto missing = h.run(Json{{"command", "reframe.rerun"}, {"params", Json{{"sequence", reframed.value}}}});
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error->code == command::ErrorCode::EditRejected);
    CHECK(missing.error->message.find("source missing") != std::string::npos);
}

TEST_CASE("analysis.subjects reports what was found; permissions and services are enforced", "[ai][reframe][commands]") {
    Timeline tl(clipPicture(25));
    const MediaId media = tl.media('A');
    const auto subjects = analysis(4.0, {2.0}, [](double) { return std::vector<SubjectBox>{faceAt(0.5f, 0.4f, 1)}; });
    CommandHarness h(tl, command::EnginePolicy{command::CapabilitySet::all(), 1024, servicesWith(media, subjects)},
                     ai::edit::editingCommands());
    const auto found = h.ok("analysis.subjects", Json{{"media", "A"}});
    CHECK(found.data["samples"] == subjects.samples.size());
    CHECK(found.data["shots"] == 2);
    CHECK(found.data["faces"] == 1);
    CHECK(found.data["models"] == "fixture/1");
    CHECK_FALSE(found.committed);
    CHECK(h.errorOf("analysis.subjects", Json{{"media", "A"}, {"stream", 3}}) == "InvalidParams");
    CHECK(h.errorOf("reframe.createSequence", Json{{"aspect", "9x16"}}) == "InvalidParams");
    CHECK(h.errorOf("reframe.createSequence", Json{{"width", 720}}) == "InvalidParams");

    CommandHarness none(tl, command::EnginePolicy{}, ai::edit::editingCommands());
    CHECK(none.errorOf("analysis.subjects", Json{{"media", "A"}}) == "InvalidRequest");
    const auto centred = none.ok("reframe.createSequence");
    REQUIRE_FALSE(centred.warnings.empty()); // no subjects here: centred, and it says so

    CommandHarness readOnly(tl, command::EnginePolicy{command::CapabilitySet::of({command::Capability::Read}), 1024, nullptr},
                            ai::edit::editingCommands());
    CHECK(readOnly.errorOf("reframe.createSequence") == "PermissionDenied");
    CHECK(readOnly.errorOf("analysis.subjects", Json{{"media", "A"}}) == "PermissionDenied");
}
