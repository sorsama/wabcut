// Redaction audio (I.6, AudioRedactions.h): the marked media time of a redaction is bleeped (1 kHz, -12 dBFS) or muted
// sample-accurately in every clip that plays it - trimmed, reversed, reused, through word ranges - except the clips it
// excepts, with 5 ms ramps outside the range; preview == export bit for bit; edits are heard within the lead window.
// (The audio callback stays allocation- and lock-free with a redaction active: test_rt_audit.cpp.)

#include "PlaybackFixture.h"

#include <wabcut/model/ProjectOps.h>
#include <wabcut/playback/audio/AudioRedactions.h>
#include <wabcut/playback/audio/OfflineMixdown.h>

#include <catch2/catch_approx.hpp>

#include <cmath>
#include <vector>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::test;
using Catch::Approx;

namespace {

SyntheticSignal level(float value) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Constant;
    s.amplitude = value;
    return s;
}

RedactionId redact(Project& p, char media, Ticks start, Ticks end, RedactionAudio mode, std::vector<TimeRange> words = {},
                   std::vector<ClipId> exceptions = {}) {
    Redaction r;
    r.media = p.mediaId(media);
    r.source = TimeRange::fromStartEnd(start, end);
    r.audio = mode;
    r.audioRanges = std::move(words);
    r.exceptions = std::move(exceptions);
    auto added = edit::addRedaction(p.state, r);
    REQUIRE(added);
    p.state = std::move(added->state);
    return added->id;
}

void addTrack(Project& p, std::vector<Clip> clips) {
    Track track;
    track.id = p.state.ids.allocate<TrackId>();
    track.kind = TrackKind::Audio;
    for (Clip& c : clips) {
        track.clips.push_back(std::move(c));
    }
    Sequence seq = *p.state.sequences.front();
    seq.audioTracks.push_back(std::make_shared<const Track>(std::move(track)));
    p.state.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
}

/// The export path (with the project's redactions, as export resolves them) from `start`.
std::vector<float> exported(const Project& p, Ticks start, std::size_t count) {
    OfflineMixdownOptions options;
    options.renderer.edgeFade = Ticks{};
    options.redactions = AudioRedactions::of(p.state);
    OfflineMixdown mix(p.state.sequences.front(), 2, 48000, start, *p.feed, options);
    std::vector<float> out(count * 2);
    for (std::size_t done = 0; done < count;) {
        const std::size_t n = std::min<std::size_t>(count - done, 300 + done % 777);
        mix.render(std::span<float>(out.data() + done * 2, n * 2));
        done += n;
    }
    return out;
}

/// Frame of timeline time `t` (48 kHz).
std::size_t at(Ticks t) { return static_cast<std::size_t>(t.value / kTicksPerFrame); }

} // namespace

TEST_CASE("Redaction audio: the marked media time is bleeped exactly, ramps sit outside it", "[playback][redaction]") {
    Project p;
    p.media('A', level(0.5f));
    const Clip clip = p.makeClip({'A', ms(0), ms(1000), ms(200)}); // a trim: media 200 .. 1200 ms
    addTrack(p, {clip});
    redact(p, 'A', ms(500), ms(700), RedactionAudio::Bleep); // timeline 300 .. 500 ms
    const auto out = exported(p, Ticks{}, 48000);

    std::size_t wrongInside = 0;
    for (std::size_t k = at(ms(300)); k < at(ms(500)); ++k) {
        const float expected = AudioRedactions::bleepAt(ms(200) + frames(static_cast<std::int64_t>(k)));
        wrongInside += out[k * 2] != expected || out[k * 2 + 1] != expected ? 1 : 0;
    }
    CHECK(wrongInside == 0); // nothing of the source survives inside, to the sample
    // Untouched beyond the 5 ms ramps; ramping in between.
    CHECK(out[at(ms(294)) * 2] == 0.5f);
    CHECK(out[at(ms(506)) * 2] == 0.5f);
    CHECK(std::fabs(out[at(ms(297.5)) * 2] - 0.5f) > 0.1f);
    CHECK(std::fabs(out[at(ms(297.5)) * 2] - 0.5f) < 0.45f);
    CHECK(maxStep(out) < 0.05); // no click: 0.5 / 240 per sample of ramp plus the sine's own slope
    // The bleep: 1 kHz at -12 dBFS.
    double peak = 0.0;
    int crossings = 0;
    for (std::size_t k = at(ms(310)); k < at(ms(490)); ++k) {
        peak = std::max(peak, static_cast<double>(std::fabs(out[k * 2])));
        crossings += out[(k - 1) * 2] < 0.0f && out[k * 2] >= 0.0f ? 1 : 0;
    }
    CHECK(20.0 * std::log10(peak) == Approx(-12.0).margin(0.05));
    CHECK(crossings == 180);
}

TEST_CASE("Redaction audio: a subclip or a separated stem hears its source's bleep, and the reverse (review wave 5 "
          "E5-P1-1, A5-P1-3)",
          "[playback][redaction][subclip][stems]") {
    // S stands for a subclip or a stem of A: the same media times (a synthetic signal of its own, so what is heard is S).
    const auto derivedFrom = [](Project& p, char item, char source, bool stem) {
        auto copy = std::make_shared<MediaItem>(*p.state.findMedia(p.mediaId(item)));
        if (stem) {
            copy->derived = DerivedProvenance{p.mediaId(source), "separateStems", "htdemucs", "vocals", "2026-10-12T00:00:00Z"};
        } else {
            copy->subclip = SubclipSpec{p.mediaId(source), TimeRange::fromStartEnd(ms(0), ms(1500)), false};
        }
        for (MediaPtr& m : p.state.media) {
            if (m->id == copy->id) {
                m = copy;
            }
        }
    };
    const auto bleepedAt = [](const std::vector<float>& out, Ticks from, Ticks to, Ticks sourceAtFrom) {
        std::size_t wrong = 0;
        for (std::size_t k = at(from); k < at(to); ++k) {
            const float expected = AudioRedactions::bleepAt(sourceAtFrom + frames(static_cast<std::int64_t>(k - at(from))));
            wrong += out[k * 2] != expected || out[k * 2 + 1] != expected ? 1 : 0;
        }
        return wrong;
    };
    for (const bool stem : {false, true}) {
        INFO((stem ? "stem" : "subclip"));
        Project p;
        p.media('A', level(0.5f));
        p.media('S', level(0.5f));
        derivedFrom(p, 'S', 'A', stem);
        addTrack(p, {p.makeClip({'S', ms(0), ms(1000), ms(200)})}); // media 200 .. 1200 ms
        redact(p, 'A', ms(500), ms(700), RedactionAudio::Bleep);    // drawn on the source: timeline 300 .. 500 ms
        const auto out = exported(p, Ticks{}, 48000);
        CHECK(bleepedAt(out, ms(300), ms(500), ms(500)) == 0); // it played the name in clear before
        CHECK(out[at(ms(250)) * 2] == 0.5f);
        // The table names S among the media whose sound changed, so the player refills its tracks.
        const auto changed = AudioRedactions::changedMedia(nullptr, AudioRedactions::of(p.state).get());
        CHECK(std::ranges::find(changed, p.mediaId('S')) != changed.end());

        // The reverse: drawn on S (the Source Monitor showing the subclip), it covers A's clips.
        Project q;
        q.media('A', level(0.5f));
        q.media('S', level(0.5f));
        derivedFrom(q, 'S', 'A', stem);
        addTrack(q, {q.makeClip({'A', ms(0), ms(1000), ms(200)})});
        redact(q, 'S', ms(500), ms(700), RedactionAudio::Bleep);
        CHECK(bleepedAt(exported(q, Ticks{}, 48000), ms(300), ms(500), ms(500)) == 0);
    }
}

TEST_CASE("Redaction audio: remapped spans bleep at source time at any speed and holds stay silent",
          "[playback][redaction][remap][export]") {
    Project p;
    p.media('A', level(0.5f));
    Clip clip = p.makeClip({'A', ms(0), ms(1500), ms(0), -12.0});
    ClipContent content = clip.content();
    Curve curve;
    curve.keys = {{ms(0), double(ms(1000).value)}, {ms(300), double(ms(1300).value)},
                  {ms(600), double(ms(1300).value)}, {ms(900), double(ms(1000).value)},
                  {ms(1200), double(ms(1600).value)}, {ms(1500), double(ms(1900).value)}};
    content.timeRemap = curve;
    clip.setContent(std::move(content));
    addTrack(p, {clip});
    redact(p, 'A', ms(1000), ms(2000), RedactionAudio::Bleep);
    const auto out = exported(p, Ticks{}, at(ms(1500)));
    // The 100 % spans: the bleep at source time, to the sample (the first 32 frames after 1200 ms crossfade with the 2x
    // span before them). The hold [300, 600) ms is silent.
    std::size_t wrong = 0;
    for (std::size_t k = 0; k < at(ms(1500)); ++k) {
        const Ticks t = frames(static_cast<std::int64_t>(k));
        const bool unity = t < ms(300) || t >= ms(1200) + frames(32);
        const bool hold = t >= ms(300) && t < ms(600);
        if (unity || hold) {
            const float expected = unity ? AudioRedactions::bleepAt(clip.sourceTimeAt(t)) : 0.0f;
            wrong += out[k * 2] != expected || out[k * 2 + 1] != expected ? 1 : 0;
        }
    }
    CHECK(wrong == 0);
    // A.10: the reverse [600, 900) ms and the 2x span [900, 1200) ms play their media time redacted: the bleep (at the
    // clip's -12 dB there, as the speed spans read redacted media), never the 0.5 level under it.
    double sum = 0.0;
    double peak = 0.0;
    for (std::size_t k = at(ms(600)); k < at(ms(1200)); ++k) {
        sum += out[k * 2];
        peak = std::max(peak, static_cast<double>(std::fabs(out[k * 2])));
    }
    CHECK(std::fabs(sum / static_cast<double>(at(ms(1200)) - at(ms(600)))) < 0.005);
    CHECK(peak > 0.03);
    CHECK(peak < 0.07);
    Rig rig(p);
    rig.player->play();
    rig.pullFor(1500);
    REQUIRE(rig.stream.size() >= out.size());
    CHECK(std::equal(out.begin(), out.end(), rig.stream.begin()));
    CHECK(rig.player->stats().underrunFrames == 0);
}

TEST_CASE("Redaction audio: mutes word ranges in every use of the media, reversed too, except where excepted",
          "[playback][redaction]") {
    Project p;
    p.media('A', level(0.5f));
    p.media('B', level(0.25f));
    const Clip forward = p.makeClip({'A', ms(0), ms(1000), ms(0)});
    Clip reversed = p.makeClip({'A', ms(1000), ms(1000), ms(0)});
    reversed.reversed = true; // plays media 999.98 .. 0 ms
    const Clip excepted = p.makeClip({'A', ms(2000), ms(1000), ms(0)});
    const Clip other = p.makeClip({'B', ms(0), ms(3000), ms(0)});
    addTrack(p, {forward, reversed, excepted});
    addTrack(p, {other});
    // Two words of the marked range are muted, not the whole range.
    redact(p, 'A', ms(100), ms(900), RedactionAudio::Mute, {TimeRange{ms(200), ms(50)}, TimeRange{ms(600), ms(100)}},
           {excepted.id});
    const auto out = exported(p, Ticks{}, 3 * 48000);
    const auto valueAt = [&](double timelineMs) { return out[at(ms(timelineMs)) * 2]; };
    // Forward: the words are silent (only B's 0.25 sounds), between them A sounds again.
    CHECK(valueAt(220) == 0.25f);
    CHECK(valueAt(650) == 0.25f);
    CHECK(valueAt(400) == 0.75f);
    CHECK(valueAt(150) == 0.75f); // inside the marked range but not a word
    // Reversed: media time t plays at timeline 2000 - t.
    CHECK(valueAt(2000 - 220) == 0.25f);
    CHECK(valueAt(2000 - 650) == 0.25f);
    CHECK(valueAt(2000 - 400) == 0.75f);
    // Excepted: untouched.
    CHECK(valueAt(2220) == 0.75f);
    CHECK(valueAt(2650) == 0.75f);
    // Every sample inside a word, in both uses, is free of A.
    std::size_t leaks = 0;
    for (const auto& [from, to] : {std::pair{200.0, 250.0}, std::pair{600.0, 700.0}}) {
        for (std::size_t k = at(ms(from)); k < at(ms(to)); ++k) {
            leaks += out[k * 2] != 0.25f ? 1 : 0;
        }
        for (std::size_t k = at(ms(2000 - to)); k < at(ms(2000 - from)); ++k) {
            leaks += out[k * 2] != 0.25f ? 1 : 0;
        }
    }
    CHECK(leaks == 0);
}

TEST_CASE("Redaction audio: preview == export bit for bit; adding one while playing is heard within the lead",
          "[playback][redaction][export]") {
    Project p;
    SyntheticSignal speech;
    speech.kind = SyntheticSignal::Kind::Noise;
    speech.amplitude = 0.3f;
    speech.seed = 11;
    p.media('A', speech);
    p.addTrack({{'A', ms(0), ms(3000), ms(500), -2.0, 0.2}});
    redact(p, 'A', ms(800), ms(1400), RedactionAudio::Bleep);
    redact(p, 'A', ms(1500), ms(1600), RedactionAudio::Mute);
    {
        Rig rig(p);
        rig.player->play();
        rig.pullFor(2000);
        const auto offline = exported(p, Ticks{}, 2000 * 48);
        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < offline.size(); ++i) {
            mismatches += rig.stream[i] != offline[i] ? 1 : 0;
        }
        CHECK(mismatches == 0);
        CHECK(rig.player->stats().underrunFrames == 0);
    }
    // A redaction added during playback (an agent or the Source Monitor marking a word) takes over ahead of the playhead.
    Project live;
    live.media('A', level(0.5f));
    live.addTrack({{'A', ms(0), ms(5000), ms(0)}});
    Rig rig(live);
    rig.player->play();
    rig.pullFor(500);
    Redaction r;
    r.media = live.mediaId('A');
    r.source = TimeRange{ms(700), ms(300)};
    r.audio = RedactionAudio::Mute;
    auto added = edit::addRedaction(*rig.session.snapshot(), r);
    REQUIRE(added);
    REQUIRE(rig.session.commit("redact", std::move(added->state)));
    rig.player->feedOnce();
    rig.pullFor(700);
    CHECK(rig.stream[at(ms(600)) * 2] == 0.5f);
    std::size_t leaks = 0;
    for (std::size_t k = at(ms(700)); k < at(ms(1000)); ++k) {
        leaks += rig.stream[k * 2] != 0.0f ? 1 : 0;
    }
    CHECK(leaks == 0);
    CHECK(rig.stream[at(ms(1100)) * 2] == 0.5f);
    CHECK(rig.player->stats().underrunFrames == 0);
}

TEST_CASE("Redaction audio: an edit refills only the tracks whose media sound different", "[playback][redaction]") {
    // Review P2-12: before the fix any change to the project's redactions (a rename, a video-only redaction, a region
    // move) refilled every audio track, restarting their effects and decoding again.
    Project p;
    p.media('A', level(0.5f));
    p.media('B', level(0.25f));
    p.addTrack({{'A', ms(0), ms(5000), ms(0)}});
    p.addTrack({{'B', ms(0), ms(5000), ms(0)}});
    const RedactionId onA = redact(p, 'A', ms(3000), ms(3200), RedactionAudio::Mute);
    Rig rig(p);
    rig.player->play();
    rig.pullFor(300);
    const auto flushes = [&] { return rig.player->stats().editFlushes; };
    const auto commit = [&](ProjectState next) {
        REQUIRE(rig.session.commit("redaction", std::move(next)));
        rig.player->feedOnce();
        rig.pullFor(50);
    };
    const std::uint64_t start = flushes();

    // Picture-only changes: the name, a video-only redaction on A.
    Redaction renamed = *std::find_if(rig.session.snapshot()->redactions.begin(), rig.session.snapshot()->redactions.end(),
                                      [&](const Redaction& r) { return r.id == onA; });
    renamed.name = "Licence plate";
    commit(*edit::updateRedaction(*rig.session.snapshot(), renamed));
    Redaction picture;
    picture.media = p.mediaId('A');
    picture.source = TimeRange{ms(1000), ms(500)};
    picture.audio = RedactionAudio::None;
    commit(edit::addRedaction(*rig.session.snapshot(), picture)->state);
    CHECK(flushes() == start);

    // A bleep on B: only A2 (which plays B) is refilled, and it is heard.
    Redaction onB;
    onB.media = p.mediaId('B');
    onB.source = TimeRange{ms(800), ms(200)};
    onB.audio = RedactionAudio::Mute;
    commit(edit::addRedaction(*rig.session.snapshot(), onB)->state);
    CHECK(flushes() == start + 1);
    rig.pullFor(800);
    CHECK(rig.stream[at(ms(900)) * 2] == Approx(0.5f)); // A alone where B is muted
    CHECK(rig.stream[at(ms(600)) * 2] == Approx(0.75f));
}
