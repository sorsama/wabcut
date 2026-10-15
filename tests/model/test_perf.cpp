// Performance: edits, undo and redo on a 10,000-clip sequence.
//
// Two projects: a realistic one (clips cut from 200 media files keep their media's name and default
// parameters, so they share content blocks) must stay under 1 ms per edit; a worst case where every clip
// has its own name must stay under the 2 ms contract bound. The timed tests run in every ctest run; run
// the hidden benchmarks with:
//     wabcut_model_tests "[!benchmark]"

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

constexpr int kClipFrames = 10;
constexpr int kMediaFiles = 200;

enum class Content {
    SharedPerMedia, ///< clips keep their media's name and defaults (the usual case)
    UniquePerClip,  ///< every clip renamed (worst case for copying clips)
};

struct BigProject {
    ProjectState state;
    SequenceId seq;
    std::vector<ClipId> firstClips; // first clip of each track
    MediaId media;                  // a media item to edit in
};

/// `tracks` video and `tracks` audio tracks of `clipsPerTrack` adjacent 10-frame clips; Vn/An clip pairs
/// are linked like imported A/V clips. V1 and A1 are targeted.
BigProject makeBigProject(int tracks, int clipsPerTrack, bool linkPairs, Content content) {
    BigProject big;
    for (int m = 0; m < kMediaFiles; ++m) {
        char name[32];
        std::snprintf(name, sizeof name, "A%03d_C%03d_0101XY.mov", m / 10, m);
        MediaItem item;
        item.name = name;
        item.locator.path = std::string("D:/footage/") + name;
        item.info.duration = f(1'000'000);
        item.info.video.push_back({});
        item.info.video[0].duration = item.info.duration;
        item.info.audio.push_back({});
        item.info.audio[0].duration = item.info.duration;
        auto added = edit::addMedia(big.state, item);
        REQUIRE(added);
        big.state = std::move(added->state);
        big.media = added->id;
    }
    std::vector<const MediaItem*> media;
    for (const MediaPtr& m : big.state.media) {
        media.push_back(m.get());
    }
    edit::SequenceSettings settings;
    settings.frameRate = kRate;
    settings.videoTracks = tracks;
    settings.audioTracks = tracks;
    auto created = edit::addSequence(big.state, settings);
    REQUIRE(created);
    big.state = std::move(created->state);
    big.seq = created->id;

    auto seq = std::make_shared<Sequence>(*big.state.findSequence(big.seq));
    std::vector<std::shared_ptr<Track>> video;
    std::vector<std::shared_ptr<Track>> audio;
    for (int t = 0; t < tracks; ++t) {
        video.push_back(std::make_shared<Track>(*seq->videoTracks[static_cast<std::size_t>(t)]));
        audio.push_back(std::make_shared<Track>(*seq->audioTracks[static_cast<std::size_t>(t)]));
    }
    for (int t = 0; t < tracks; ++t) {
        for (int i = 0; i < clipsPerTrack; ++i) {
            const LinkGroupId group = linkPairs ? big.state.ids.allocate<LinkGroupId>() : LinkGroupId{};
            const MediaItem& m = *media[static_cast<std::size_t>(i % kMediaFiles)];
            for (auto* list : {&video, &audio}) {
                Clip c;
                c.id = big.state.ids.allocate<ClipId>();
                c.media = m.id;
                c.stream = {list == &video ? StreamKind::Video : StreamKind::Audio, 0};
                c.timelineRange = {f(static_cast<std::int64_t>(i) * kClipFrames), f(kClipFrames)};
                c.sourceIn = f(static_cast<std::int64_t>(i % 50) * kClipFrames);
                if (content == Content::SharedPerMedia) {
                    c.setContent(m.defaultClipContent());
                } else {
                    c.setName(m.name + " take " + std::to_string(i));
                }
                c.linkGroup = group;
                (*list)[static_cast<std::size_t>(t)]->clips.push_back(std::move(c));
            }
        }
        big.firstClips.push_back(video[static_cast<std::size_t>(t)]->clips.front().id);
    }
    for (int t = 0; t < tracks; ++t) {
        seq->videoTracks[static_cast<std::size_t>(t)] = video[static_cast<std::size_t>(t)];
        seq->audioTracks[static_cast<std::size_t>(t)] = audio[static_cast<std::size_t>(t)];
    }
    seq->duration = seq->contentEnd();
    const IdGenerator ids = big.state.ids;
    big.state = big.state.withSequence(seq);
    big.state.ids = ids;
    return big;
}

/// Same layout with a single video track holding every clip: the worst case for ripples.
BigProject makeSingleTrackProject(int clips, Content content) {
    BigProject big = makeBigProject(1, clips, false, content);
    auto seq = std::make_shared<Sequence>(*big.state.findSequence(big.seq));
    seq->audioTracks.clear();
    const IdGenerator ids = big.state.ids;
    big.state = big.state.withSequence(seq);
    big.state.ids = ids;
    return big;
}

struct Timing {
    std::string name;
    double bestMs = 0.0;
    double medianMs = 0.0;
};

Timing timeEdit(const std::string& name, const std::function<edit::EditOutcome()>& op) {
    using Clock = std::chrono::steady_clock;
    std::vector<double> samples;
    // Best of 25 timed runs: the minimum is the cost of the edit itself; a busy machine only adds to it.
    for (int run = 0; run < 27; ++run) {
        const auto start = Clock::now();
        auto outcome = op();
        const auto stop = Clock::now();
        REQUIRE(outcome);
        if (run >= 2) { // the first runs warm caches and the allocator
            samples.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
        }
    }
    std::sort(samples.begin(), samples.end());
    return {name, samples.front(), samples[samples.size() / 2]};
}

#ifdef NDEBUG
constexpr double kRealisticMs = 1.0; // best run, usual project
constexpr double kContractMs = 2.0;  // best run, worst case (the architecture's bound)
constexpr double kGenerousMs = 10.0; // median, with headroom for a busy machine
#else
constexpr double kRealisticMs = 40.0; // unoptimized builds only check that nothing is pathological
constexpr double kContractMs = 80.0;
constexpr double kGenerousMs = 200.0;
#endif

std::vector<Timing> timeAllEdits(const BigProject& big) {
    const ProjectState& s = big.state;
    const SequenceId seq = big.seq;
    const Sequence& sequence = *s.findSequence(seq);
    const ClipId first = big.firstClips[0];
    const ClipId middle = sequence.videoTracks[0]->clips[1250].id;
    const std::vector<ClipId> firstSel{first};
    const std::vector<ClipId> middleSel{middle};
    const edit::SourceClip source{big.media, {f(0), f(25)}, edit::defaultSourcePatches(sequence, *s.findMedia(big.media))};
    auto marked = ok(edit::markIn(s, seq, f(5))).state;
    marked = ok(edit::markOut(marked, seq, f(24))).state;

    std::vector<Timing> timings;
    timings.push_back(timeEdit("insert near the start", [&] { return edit::insertEdit(s, seq, source, f(5)); }));
    timings.push_back(timeEdit("overwrite in the middle", [&] { return edit::overwriteEdit(s, seq, source, f(12'503)); }));
    timings.push_back(timeEdit("ripple delete first clip", [&] { return edit::rippleDeleteClips(s, seq, firstSel); }));
    timings.push_back(timeEdit("delete middle clip", [&] { return edit::deleteClips(s, seq, middleSel); }));
    timings.push_back(timeEdit("add edit to all tracks", [&] { return edit::addEditToAllTracks(s, seq, f(12'505)); }));
    timings.push_back(timeEdit("lift", [&] { return edit::lift(marked, seq); }));
    timings.push_back(timeEdit("extract", [&] { return edit::extract(marked, seq); }));
    timings.push_back(timeEdit("Q at the start", [&] { return edit::rippleTrimPreviousEditToPlayhead(s, seq, f(5)); }));
    timings.push_back(timeEdit("W at the start", [&] { return edit::rippleTrimNextEditToPlayhead(s, seq, f(5)); }));
    timings.push_back(timeEdit("ripple trim first tail", [&] { return edit::rippleTrim(s, seq, first, edit::Edge::Tail, f(-3)); }));
    timings.push_back(timeEdit("trim edge", [&] { return edit::trimEdge(s, seq, middle, edit::Edge::Head, f(2)); }));
    timings.push_back(timeEdit("roll", [&] { return edit::rollEdit(s, seq, middle, edit::Edge::Tail, f(2)); }));
    timings.push_back(timeEdit("slip", [&] { return edit::slip(s, seq, middleSel, f(2)); }));
    timings.push_back(timeEdit("slide", [&] { return edit::slide(s, seq, middle, f(2)); }));
    timings.push_back(timeEdit("move (overwrite)", [&] { return edit::moveClips(s, seq, middleSel, f(3)); }));
    timings.push_back(timeEdit("move (insert)", [&] {
        return edit::moveClips(s, seq, middleSel, f(-12'000), {}, edit::MoveMode::Insert);
    }));
    timings.push_back(timeEdit("mark in", [&] { return edit::markIn(s, seq, f(100)); }));
    // Finishing ops: a rippled speed change and a hold segment move every later clip; a transition or a key
    // touches one track or one clip.
    timings.push_back(timeEdit("speed/duration with ripple", [&] {
        edit::SpeedChange change;
        change.speed = Speed{1, 2};
        change.ripple = true;
        return edit::setSpeed(s, seq, firstSel, change);
    }));
    const Clip& middleClip = sequence.findClip(middle)->clip();
    timings.push_back(timeEdit("frame hold segment", [&] {
        return edit::frameHold(s, seq, middle, middleClip.start() + f(1), f(10));
    }));
    const auto edge = edit::transitionAtEdge(s, seq, middle, edit::Edge::Tail, f(4), edit::TransitionAlignment::Center,
                                             "crossDissolve");
    REQUIRE(edge);
    timings.push_back(timeEdit("add transition", [&] {
        return edit::addTransition(s, seq, edge->track, edge->spec, edit::HandleFit::UseAvailable);
    }));
    timings.push_back(timeEdit("add keyframe", [&] {
        return edit::addKeyframe(s, seq, middle, edit::ParamRef::of(ParamId::Opacity),
                                 Keyframe{middleClip.curveTimeAt(middleClip.start()), 0.5});
    }));
    return timings;
}

std::vector<Timing> timeSingleTrack(const BigProject& single) {
    const SequenceId seq = single.seq;
    const edit::SourceClip source{single.media, {f(0), f(25)},
                                  {{{StreamKind::Video, 0}, single.state.findSequence(seq)->videoTracks[0]->id}}};
    const std::vector<ClipId> first{single.firstClips[0]};
    std::vector<Timing> timings;
    timings.push_back(timeEdit("single track: insert at start", [&] { return edit::insertEdit(single.state, seq, source, f(5)); }));
    timings.push_back(timeEdit("single track: ripple delete first", [&] { return edit::rippleDeleteClips(single.state, seq, first); }));
    return timings;
}

} // namespace

TEST_CASE("Every edit on a realistic 10,000-clip sequence stays under 1 ms", "[model][perf]") {
    const BigProject big = makeBigProject(2, 2500, true, Content::SharedPerMedia); // 2 V + 2 A tracks x 2500
    requireValid(big.state);
    std::vector<Timing> timings = timeAllEdits(big);
    const std::vector<Timing> single = timeSingleTrack(makeSingleTrackProject(10'000, Content::SharedPerMedia));
    timings.insert(timings.end(), single.begin(), single.end());
    for (const Timing& t : timings) {
        INFO(t.name << ": best " << t.bestMs << " ms, median " << t.medianMs << " ms");
        CHECK(t.bestMs < kRealisticMs);
        CHECK(t.medianMs < kGenerousMs);
    }
}

TEST_CASE("Edits stay under 2 ms when every one of 10,000 clips has its own name", "[model][perf]") {
    const BigProject big = makeBigProject(2, 2500, true, Content::UniquePerClip);
    std::vector<Timing> timings = timeAllEdits(big);
    const std::vector<Timing> single = timeSingleTrack(makeSingleTrackProject(10'000, Content::UniquePerClip));
    timings.insert(timings.end(), single.begin(), single.end());
    for (const Timing& t : timings) {
        INFO("worst case " << t.name << ": best " << t.bestMs << " ms, median " << t.medianMs << " ms");
        CHECK(t.bestMs < kContractMs);
        CHECK(t.medianMs < kGenerousMs);
    }
}

TEST_CASE("Undo and redo of a 10,000-clip ripple stay under 1 ms", "[model][perf]") {
    const BigProject big = makeBigProject(2, 2500, true, Content::SharedPerMedia);
    EditSessionOptions options;
    options.validateEdits = false;
    options.deferRelease = false;
    EditSession session(big.state, options);
    const std::vector<ClipId> sel{big.firstClips[0]};
    REQUIRE(session.apply("Ripple Delete", [&](const ProjectState& s) { return edit::rippleDeleteClips(s, big.seq, sel); }));
    using Clock = std::chrono::steady_clock;
    std::vector<double> undos;
    std::vector<double> redos;
    for (int i = 0; i < 15; ++i) {
        auto start = Clock::now();
        const auto undone = session.undo();
        undos.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        REQUIRE(undone);
        // The deleted pair and the cut-through pair on V2/A2 come back; everything after them moves.
        CHECK(undone->sequence(big.seq)->addedClips.size() == 4);
        CHECK(undone->sequence(big.seq)->modifiedClips.size() == 9'996);
        start = Clock::now();
        REQUIRE(session.redo());
        redos.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    std::sort(undos.begin(), undos.end());
    std::sort(redos.begin(), redos.end());
    INFO("undo best " << undos.front() << " ms, median " << undos[7] << " ms; redo best " << redos.front()
                      << " ms, median " << redos[7] << " ms");
    CHECK(undos.front() < kRealisticMs);
    CHECK(redos.front() < kRealisticMs);
    CHECK(undos[7] < kGenerousMs);
}

TEST_CASE("Edit benchmarks on a 10,000-clip sequence", "[!benchmark][model][perf]") {
    const BigProject big = makeBigProject(2, 2500, true, Content::SharedPerMedia);
    const ProjectState& s = big.state;
    const Sequence& sequence = *s.findSequence(big.seq);
    const edit::SourceClip source{big.media, {f(0), f(25)}, edit::defaultSourcePatches(sequence, *s.findMedia(big.media))};
    const std::vector<ClipId> firstSel{big.firstClips[0]};
    const ClipId middle = sequence.videoTracks[0]->clips[1250].id;
    const ProjectState rippled = edit::rippleDeleteClips(s, big.seq, firstSel)->state;

    BENCHMARK("insert near the start") { return edit::insertEdit(s, big.seq, source, f(5)); };
    BENCHMARK("overwrite in the middle") { return edit::overwriteEdit(s, big.seq, source, f(12'503)); };
    BENCHMARK("ripple delete first clip") { return edit::rippleDeleteClips(s, big.seq, firstSel); };
    BENCHMARK("add edit to all tracks") { return edit::addEditToAllTracks(s, big.seq, f(12'505)); };
    BENCHMARK("ripple trim") { return edit::rippleTrim(s, big.seq, big.firstClips[0], edit::Edge::Tail, f(-3)); };
    BENCHMARK("slide") { return edit::slide(s, big.seq, middle, f(2)); };
    BENCHMARK("diff after a ripple") { return diff(s, rippled).sequences.size(); };
    BENCHMARK("validate (not on the edit path)") { return validate(s).has_value(); };
}
