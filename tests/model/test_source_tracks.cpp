// A.10 multi-track ingest: the names OBS, ShadowPlay and Game Bar recordings give their audio streams, routing each
// stream to the track named for its source (free tracks are named, missing ones added), and the edits that do it.

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

using Origin = edit::SourceTrackName::Origin;

/// A fresh sequence: an empty video track and three empty audio tracks, none of them named.
constexpr const char* kFresh = R"(
    V1 tgt |..........|
    A1 tgt |..........|
    A2     |..........|
    A3     |..........|
)";

/// A recording named `name` (it renders with its first letter): `frames` frames at the fixture rate, a video stream
/// unless `video` is false, and one AAC stereo stream per title ("" = untitled).
MediaItem recording(const std::string& name, std::initializer_list<const char*> titles, bool video = true,
                    std::int64_t frames = 100) {
    MediaItem item;
    item.name = name;
    item.locator.path = "D:/recordings/" + name + ".mkv";
    item.info.path = item.locator.path;
    item.info.container = "matroska,webm";
    item.info.duration = f(frames);
    if (video) {
        VideoStreamInfo v;
        v.index = 0;
        v.codec = "h264";
        v.width = 1920;
        v.height = 1080;
        v.frameRate = kRate;
        v.duration = f(frames);
        v.frameCount = frames;
        item.info.video.push_back(v);
    }
    int index = video ? 1 : 0;
    for (const char* title : titles) {
        AudioStreamInfo a;
        a.index = index++;
        a.codec = "aac";
        a.channels = 2;
        a.channelLayout = "stereo";
        a.duration = f(frames);
        a.title = title;
        item.info.audio.push_back(a);
    }
    return item;
}

MediaId addRecording(ProjectState& state, MediaItem item) {
    auto added = edit::addMedia(state, std::move(item));
    REQUIRE(added);
    state = std::move(added->state);
    return added->id;
}

/// Names `track` (any track, locked or not), as the track header's rename does.
ProjectState named(const ProjectState& state, SequenceId seq, TrackId track, const std::string& name) {
    edit::TrackStateChange change;
    change.name = name;
    return ok(edit::setTrackState(state, seq, track, change)).state;
}

/// `frames` frames of `media` from its start, with the default patching for the sequence as it is in `state`.
edit::SourceClip defaultSource(const ProjectState& state, SequenceId seq, MediaId media, std::int64_t frames) {
    edit::SourceClip source;
    source.media = media;
    source.sourceRange = {f(0), f(frames)};
    source.patches = edit::defaultSourcePatches(*state.findSequence(seq), *state.findMedia(media));
    return source;
}

std::vector<std::string> audioNames(const ProjectState& state, SequenceId seq) {
    std::vector<std::string> out;
    for (const TrackPtr& t : state.findSequence(seq)->audioTracks) {
        out.push_back(t->name);
    }
    return out;
}

std::vector<TrackRole> audioRoles(const ProjectState& state, SequenceId seq) {
    std::vector<TrackRole> out;
    for (const TrackPtr& t : state.findSequence(seq)->audioTracks) {
        out.push_back(t->role);
    }
    return out;
}

const Track& audioTrack(const ProjectState& state, SequenceId seq, std::size_t index) {
    const Sequence& s = *state.findSequence(seq);
    REQUIRE(index < s.audioTracks.size());
    return *s.audioTracks[index];
}

} // namespace

TEST_CASE("Stream titles map to canonical track names and roles", "[model][edit][patching][multitrack]") {
    struct Case {
        const char* title;
        const char* name;
        TrackRole role;
        Origin origin;
    };
    const std::vector<Case> cases{
        {"Game", "Game", TrackRole::Effects, Origin::Recognised},
        {"game audio", "Game", TrackRole::Effects, Origin::Recognised},
        {"Gameplay", "Game", TrackRole::Effects, Origin::Recognised},
        {"Desktop Audio", "Game", TrackRole::Effects, Origin::Recognised},
        {"DESKTOP", "Game", TrackRole::Effects, Origin::Recognised},
        {"System Audio", "Game", TrackRole::Effects, Origin::Recognised},
        {"App Audio", "Game", TrackRole::Effects, Origin::Recognised},
        {"Application Audio", "Game", TrackRole::Effects, Origin::Recognised},
        {"Mic", "Mic", TrackRole::Dialogue, Origin::Recognised},
        {"Mic/Aux", "Mic", TrackRole::Dialogue, Origin::Recognised},
        {"mic aux", "Mic", TrackRole::Dialogue, Origin::Recognised},
        {"Microphone", "Mic", TrackRole::Dialogue, Origin::Recognised},
        {"Mic Audio", "Mic", TrackRole::Dialogue, Origin::Recognised},
        {"Voice", "Mic", TrackRole::Dialogue, Origin::Recognised},
        {"Commentary", "Mic", TrackRole::Dialogue, Origin::Recognised},
        {"Discord", "Discord", TrackRole::Dialogue, Origin::Recognised},
        {"Voice Chat", "Discord", TrackRole::Dialogue, Origin::Recognised},
        {"chat", "Discord", TrackRole::Dialogue, Origin::Recognised},
        {"Party Chat", "Discord", TrackRole::Dialogue, Origin::Recognised},
        {"TeamSpeak", "Discord", TrackRole::Dialogue, Origin::Recognised},
        {"TeamSpeak 3", "Discord", TrackRole::Dialogue, Origin::Recognised},
        {"TS3", "Discord", TrackRole::Dialogue, Origin::Recognised},
        {"Music", "Music", TrackRole::Music, Origin::Recognised},
        {"Spotify", "Music", TrackRole::Music, Origin::Recognised},
        {"BGM", "Music", TrackRole::Music, Origin::Recognised},
        {"Alerts", "Alerts", TrackRole::Effects, Origin::Recognised},
        {"Stream Alerts", "Alerts", TrackRole::Effects, Origin::Recognised},
        // OBS's own track names, and a number after a recognised source: two mics stay apart.
        {"Track1", "Track 1", TrackRole::None, Origin::Recognised},
        {"Track 3", "Track 3", TrackRole::None, Origin::Recognised},
        {"track_6", "Track 6", TrackRole::None, Origin::Recognised},
        {"Mic/Aux 2", "Mic 2", TrackRole::Dialogue, Origin::Recognised},
        {"Desktop Audio 2", "Game 2", TrackRole::Effects, Origin::Recognised},
        // Anything else as written, trimmed.
        {"  Valorant  ", "Valorant", TrackRole::None, Origin::Title},
        {"Track", "Track", TrackRole::None, Origin::Title},
        {"Mic 123456", "Mic 123456", TrackRole::None, Origin::Title}, // a long number is not a source number
        {"", "", TrackRole::None, Origin::None},
        {"   ", "", TrackRole::None, Origin::None},
    };
    for (const Case& c : cases) {
        INFO("title \"" << c.title << "\"");
        const edit::SourceTrackName n = edit::sourceTrackName(c.title);
        CHECK(n.name == c.name);
        CHECK(n.role == c.role);
        CHECK(n.origin == c.origin);
    }

    // Long titles are cut to kMaxSourceTrackNameBytes, never inside a UTF-8 character.
    const std::string longTitle = std::string(31, 'a') + "\xC3\xA9" + "bcd"; // 31 + 2 + 3 bytes
    const edit::SourceTrackName cut = edit::sourceTrackName(longTitle);
    CHECK(cut.name == std::string(31, 'a'));
    CHECK(cut.origin == Origin::Title);
    const std::string fits = std::string(30, 'a') + "\xC3\xA9"; // exactly 32 bytes
    CHECK(edit::sourceTrackName(fits).name == fits);
}

TEST_CASE("Untitled streams of multi-stream files are named by position", "[model][edit][patching][multitrack]") {
    const auto namesOf = [](const MediaItem& item) {
        std::vector<std::string> out;
        for (const edit::SourceTrackName& n : edit::sourceTrackNames(item.info)) {
            out.push_back(n.name);
        }
        return out;
    };
    // ShadowPlay ("separate both tracks") and Xbox Game Bar with the mic: game first, then the mic.
    const MediaItem shadowPlay = recording("SP", {"", ""});
    const auto sp = edit::sourceTrackNames(shadowPlay.info);
    REQUIRE(sp.size() == 2);
    CHECK(sp[0] == edit::SourceTrackName{"Game", TrackRole::Effects, Origin::Inferred});
    CHECK(sp[1] == edit::SourceTrackName{"Mic", TrackRole::Dialogue, Origin::Inferred});
    CHECK(edit::routesAudioByName(shadowPlay.info));

    // Without a picture, or with more streams, untitled streams are Track 1, Track 2, ...
    CHECK(namesOf(recording("AU", {"", ""}, false)) == std::vector<std::string>{"Track 1", "Track 2"});
    CHECK(namesOf(recording("U3", {"", "", ""})) == std::vector<std::string>{"Track 1", "Track 2", "Track 3"});
    // A titled stream keeps its name; the untitled one is named by its position.
    const auto mixed = edit::sourceTrackNames(recording("MX", {"Game", ""}).info);
    REQUIRE(mixed.size() == 2);
    CHECK(mixed[0].origin == Origin::Recognised);
    CHECK(mixed[1] == edit::SourceTrackName{"Track 2", TrackRole::None, Origin::Inferred});
    CHECK(namesOf(recording("OBS", {"Game", "Mic", "Discord", "Music"})) ==
          std::vector<std::string>{"Game", "Mic", "Discord", "Music"});

    // A file's single stream: untitled or with an unknown title it is patched as before; a recognised one is routed.
    const auto single = edit::sourceTrackNames(recording("CAM", {""}).info);
    REQUIRE(single.size() == 1);
    CHECK(single[0].origin == Origin::None);
    CHECK(single[0].name.empty());
    CHECK_FALSE(edit::routesAudioByName(recording("CAM", {""}).info));
    CHECK_FALSE(edit::routesAudioByName(recording("CAM", {"Interview"}).info));
    CHECK(edit::routesAudioByName(recording("MIC", {"Mic"}, false).info));
    CHECK_FALSE(edit::routesAudioByName(recording("V", {}).info));
}

TEST_CASE("Default patching of a titled recording names the free tracks and asks for a new one",
          "[model][edit][patching][multitrack]") {
    Timeline tl(kFresh);
    const MediaId obs = addRecording(tl.state, recording("OBS", {"Game", "Mic", "Discord", "Music"}));
    const auto patches = edit::defaultSourcePatches(tl.sequence(), *tl.state.findMedia(obs));
    REQUIRE(patches.size() == 5);
    CHECK(patches[0].source == StreamRef{StreamKind::Video, 0});
    CHECK(patches[0].destination == tl.track("V1"));
    CHECK(patches[0].newTrack.empty());
    const std::vector<std::string> names{"Game", "Mic", "Discord", "Music"};
    for (std::size_t k = 0; k < 4; ++k) {
        INFO("audio stream " << k);
        CHECK(patches[k + 1].source == StreamRef{StreamKind::Audio, static_cast<std::int32_t>(k)});
        CHECK(patches[k + 1].newTrack == names[k]);
    }
    CHECK(patches[1].destination == tl.track("A1"));
    CHECK(patches[2].destination == tl.track("A2"));
    CHECK(patches[3].destination == tl.track("A3"));
    CHECK_FALSE(patches[4].destination); // no free track left: a new one
}

TEST_CASE("Inserting a multi-track recording adds and names its tracks with linked clips",
          "[model][edit][patching][multitrack]") {
    Timeline tl(kFresh);
    const MediaId obs = addRecording(tl.state, recording("OBS", {"Game", "Mic", "Discord", "Music"}));
    const auto r = ok(edit::insertEdit(tl.state, tl.seq, defaultSource(tl.state, tl.seq, obs, 4), f(0)));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |Oooo|
        A1 |Oooo|
        A2 |Oooo|
        A3 |Oooo|
        A4 |Oooo|
    )");
    CHECK(audioNames(r.state, tl.seq) == std::vector<std::string>{"Game", "Mic", "Discord", "Music"});
    CHECK(audioRoles(r.state, tl.seq) ==
          std::vector<TrackRole>{TrackRole::Effects, TrackRole::Dialogue, TrackRole::Dialogue, TrackRole::Music});
    const TrackId music = audioTrack(r.state, tl.seq, 3).id;
    CHECK(r.createdTracks == std::vector<TrackId>{music});
    // A new track gets addTrack's defaults: sync-locked, unlocked, untargeted.
    CHECK(audioTrack(r.state, tl.seq, 3).syncLocked);
    CHECK_FALSE(audioTrack(r.state, tl.seq, 3).locked);
    CHECK_FALSE(audioTrack(r.state, tl.seq, 3).targeted);

    // All five clips are one link group.
    REQUIRE(r.placed.size() == 5);
    const Sequence& s = *r.state.findSequence(tl.seq);
    const LinkGroupId group = s.findClip(r.placed[0])->clip().linkGroup;
    CHECK(group.isValid());
    for (const ClipId id : r.placed) {
        CHECK(s.findClip(id)->clip().linkGroup == group);
    }
    CHECK(links(r.state, tl.seq) == "V1@0+A1@0+A2@0+A3@0+A4@0");

    // The ChangeSet has the new track and the renamed ones.
    REQUIRE(r.changes.sequences.size() == 1);
    const SequenceChange& change = r.changes.sequences.front();
    CHECK(change.trackListChanged);
    const auto changeOf = [&](TrackId id) {
        const auto it = std::find_if(change.tracks.begin(), change.tracks.end(),
                                     [&](const TrackChange& t) { return t.track == id; });
        REQUIRE(it != change.tracks.end());
        return *it;
    };
    CHECK(changeOf(music).added);
    CHECK(changeOf(tl.track("A1")).headerChanged);
    CHECK_FALSE(changeOf(tl.track("A1")).added);

    // Overwrite does the same.
    const auto over = ok(edit::overwriteEdit(tl.state, tl.seq, defaultSource(tl.state, tl.seq, obs, 4), f(2)));
    CHECK(audioNames(over.state, tl.seq) == std::vector<std::string>{"Game", "Mic", "Discord", "Music"});
    CHECK(over.createdTracks.size() == 1);
}

TEST_CASE("The next recording routes by name to the same tracks", "[model][edit][patching][multitrack]") {
    Timeline tl(kFresh);
    const MediaId obs = addRecording(tl.state, recording("OBS", {"Game", "Mic", "Discord", "Music"}));
    // OBS's hybrid MP4 names: Desktop Audio, Mic/Aux, and voice chat; in another order than the first file.
    const MediaId hybrid = addRecording(tl.state, recording("Hybrid", {"Mic/Aux", "Desktop Audio", "Voice Chat"}));
    const auto first = ok(edit::insertEdit(tl.state, tl.seq, defaultSource(tl.state, tl.seq, obs, 4), f(0)));
    const auto second =
        ok(edit::overwriteEdit(first.state, tl.seq, defaultSource(first.state, tl.seq, hybrid, 3), f(4)));
    CHECK(second.createdTracks.empty()); // every source has its track already
    CHECK(audioNames(second.state, tl.seq) == std::vector<std::string>{"Game", "Mic", "Discord", "Music"});
    CHECK_TIMELINE(second.state, tl.seq, R"(
        V1 |OoooHhh|
        A1 |OoooHhh|
        A2 |OoooHhh|
        A3 |OoooHhh|
        A4 |Oooo...|
    )");
    // Both recordings' mics are on the Mic track: its mute and solo buttons act on every mic at once.
    const Track& mic = audioTrack(second.state, tl.seq, 1);
    REQUIRE(mic.clips.size() == 2);
    CHECK(mic.clips[0].media == obs);
    CHECK(mic.clips[0].stream == StreamRef{StreamKind::Audio, 1});
    CHECK(mic.clips[1].media == hybrid);
    CHECK(mic.clips[1].stream == StreamRef{StreamKind::Audio, 0});

    // Names match ignoring case and surrounding spaces.
    const ProjectState renamed = named(tl.state, tl.seq, tl.track("A3"), " MIC ");
    const auto patches = edit::defaultSourcePatches(*renamed.findSequence(tl.seq), *renamed.findMedia(hybrid));
    REQUIRE(patches.size() == 4);
    CHECK(patches[1].destination == tl.track("A3"));
    CHECK(patches[1].newTrack.empty());
}

TEST_CASE("A locked track of the source's name is skipped: the stream gets a track of that name",
          "[model][edit][patching][multitrack]") {
    Timeline tl(R"(
        V1 tgt  |Aaaa|
        A1 lock |Aaaa|
        A2      |Aaaa|
    )");
    ProjectState state = named(tl.state, tl.seq, tl.track("A1"), "Mic");
    const MediaId rec = addRecording(state, recording("Rec", {"Game", "Mic"}));
    const auto patches = edit::defaultSourcePatches(*state.findSequence(tl.seq), *state.findMedia(rec));
    REQUIRE(patches.size() == 3);
    // A1 (Mic) is locked and A2 holds clips: both streams get new tracks.
    CHECK_FALSE(patches[1].destination);
    CHECK(patches[1].newTrack == "Game");
    CHECK_FALSE(patches[2].destination);
    CHECK(patches[2].newTrack == "Mic");

    const auto r = ok(edit::overwriteEdit(state, tl.seq, defaultSource(state, tl.seq, rec, 2), f(4)));
    CHECK(audioNames(r.state, tl.seq) == std::vector<std::string>{"Mic", "", "Game", "Mic"});
    CHECK(r.createdTracks.size() == 2);
    CHECK(r.state.findSequence(tl.seq)->audioTracks[0] == state.findSequence(tl.seq)->audioTracks[0]); // untouched
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |AaaaRr|
        A1 |Aaaa..|
        A2 |Aaaa..|
        A3 |....Rr|
        A4 |....Rr|
    )");
}

TEST_CASE("ShadowPlay and Game Bar two-track files get Game and Mic tracks", "[model][edit][patching][multitrack]") {
    Timeline tl(kFresh);
    const MediaId sp = addRecording(tl.state, recording("SP", {"", ""}));
    const auto first = ok(edit::insertEdit(tl.state, tl.seq, defaultSource(tl.state, tl.seq, sp, 3), f(0)));
    CHECK(audioNames(first.state, tl.seq) == std::vector<std::string>{"Game", "Mic", ""});
    CHECK(audioRoles(first.state, tl.seq) ==
          std::vector<TrackRole>{TrackRole::Effects, TrackRole::Dialogue, TrackRole::None});
    CHECK(first.createdTracks.empty());
    // The next one lands on the same two tracks.
    const auto second = ok(edit::insertEdit(first.state, tl.seq, defaultSource(first.state, tl.seq, sp, 2), f(3)));
    CHECK_TIMELINE(second.state, tl.seq, R"(
        V1 |SssSs|
        A1 |SssSs|
        A2 |SssSs|
        A3 |.....|
    )");
}

TEST_CASE("Three untitled streams take Track 1 to Track 3", "[model][edit][patching][multitrack]") {
    Timeline tl(kFresh);
    const MediaId rec = addRecording(tl.state, recording("Rec", {"", "", ""}));
    const auto r = ok(edit::insertEdit(tl.state, tl.seq, defaultSource(tl.state, tl.seq, rec, 2), f(0)));
    CHECK(audioNames(r.state, tl.seq) == std::vector<std::string>{"Track 1", "Track 2", "Track 3"});
    CHECK(audioRoles(r.state, tl.seq) == std::vector<TrackRole>{TrackRole::None, TrackRole::None, TrackRole::None});
    CHECK(r.createdTracks.empty());
}

TEST_CASE("A single untitled stream keeps Premiere's default patching", "[model][edit][patching][multitrack]") {
    Timeline tl(kFresh);
    const MediaId cam = addRecording(tl.state, recording("Cam", {""}));
    const auto patches = edit::defaultSourcePatches(tl.sequence(), *tl.state.findMedia(cam));
    REQUIRE(patches.size() == 2);
    CHECK(patches[1].destination == tl.track("A1"));
    CHECK(patches[1].newTrack.empty());
    const auto r = ok(edit::insertEdit(tl.state, tl.seq, defaultSource(tl.state, tl.seq, cam, 2), f(0)));
    CHECK(audioNames(r.state, tl.seq) == std::vector<std::string>{"", "", ""});
    CHECK(r.createdTracks.empty());
    // An unknown title on a single stream (a camera's handler name) does not rename A1 either.
    const MediaId gopro = addRecording(tl.state, recording("Gopro", {"Hero Audio"}));
    const auto titled = edit::defaultSourcePatches(tl.sequence(), *tl.state.findMedia(gopro));
    REQUIRE(titled.size() == 2);
    CHECK(titled[1].destination == tl.track("A1"));
    CHECK(titled[1].newTrack.empty());
}

TEST_CASE("Untitled streams keep their default place on tracks that hold clips",
          "[model][edit][patching][multitrack]") {
    // The fixture's media have two untitled audio streams, like a ShadowPlay file: stream 0 goes to A1 as it always
    // did (A1 holds clips, so it is not renamed) and stream 1, which had no track, gets a new Mic track.
    Timeline tl(R"(
        V1 tgt |Aaaa|
        A1 tgt |Aaaa|
    )");
    const auto patches = edit::defaultSourcePatches(tl.sequence(), *tl.state.findMedia(tl.media('S')));
    REQUIRE(patches.size() == 3);
    CHECK(patches[1].destination == tl.track("A1"));
    CHECK(patches[1].newTrack.empty());
    CHECK_FALSE(patches[2].destination);
    CHECK(patches[2].newTrack == "Mic");
    const auto r = ok(edit::overwriteEdit(tl.state, tl.seq, defaultSource(tl.state, tl.seq, tl.media('S'), 2), f(4)));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |AaaaSs|
        A1 |AaaaSs|
        A2 |....Ss|
    )");
    CHECK(audioNames(r.state, tl.seq) == std::vector<std::string>{"", "Mic"});
    CHECK(audioTrack(r.state, tl.seq, 1).role == TrackRole::Dialogue);
}

TEST_CASE("Undo and redo of a multi-track insert restore the track list", "[model][edit][patching][multitrack]") {
    Timeline tl(kFresh);
    const MediaId obs = addRecording(tl.state, recording("OBS", {"Game", "Mic", "Discord", "Music"}));
    EditSession session(tl.state);
    const edit::SourceClip source = defaultSource(tl.state, tl.seq, obs, 4);
    const auto applied =
        session.apply("Insert", [&](const ProjectState& s) { return edit::insertEdit(s, tl.seq, source, f(0)); });
    REQUIRE(applied);
    CHECK(audioNames(*session.snapshot(), tl.seq) == std::vector<std::string>{"Game", "Mic", "Discord", "Music"});
    REQUIRE(session.undo());
    CHECK(audioNames(*session.snapshot(), tl.seq) == std::vector<std::string>{"", "", ""});
    CHECK(audioRoles(*session.snapshot(), tl.seq) ==
          std::vector<TrackRole>{TrackRole::None, TrackRole::None, TrackRole::None});
    REQUIRE(session.redo());
    CHECK(audioNames(*session.snapshot(), tl.seq) == std::vector<std::string>{"Game", "Mic", "Discord", "Music"});
}

TEST_CASE("Explicit track names on patches add and name tracks", "[model][edit][patching][multitrack]") {
    Timeline tl(R"(
        V1 tgt |....|
        A1 tgt |....|
        A2     |....|
    )");
    ProjectState state = named(tl.state, tl.seq, tl.track("A2"), "Keep");
    edit::SourceClip source;
    source.media = tl.media('S');
    source.sourceRange = {f(0), f(2)};
    // Stream 0 to a new "voice chat" track (its role follows the name), stream 1 to A2, whose name stays.
    source.patches = {{{StreamKind::Audio, 0}, {}, "voice chat"}, {{StreamKind::Audio, 1}, tl.track("A2"), "Mic"}};
    const auto r = ok(edit::insertEdit(state, tl.seq, source, f(0)));
    CHECK(audioNames(r.state, tl.seq) == std::vector<std::string>{"", "Keep", "voice chat"});
    CHECK(audioTrack(r.state, tl.seq, 2).role == TrackRole::Dialogue);
    REQUIRE(r.createdTracks.size() == 1);
    CHECK(r.createdTracks.front() == audioTrack(r.state, tl.seq, 2).id);
    CHECK(links(r.state, tl.seq) == "A2@0+A3@0");

    SECTION("a video stream cannot name or add a track") {
        source.patches = {{{StreamKind::Video, 0}, {}, "Picture"}};
        const auto refused = edit::insertEdit(state, tl.seq, source, f(0));
        REQUIRE_FALSE(refused);
        CHECK(refused.error().code == ErrorCode::InvalidArgument);
    }
    SECTION("a patch with neither a track nor a name is refused") {
        source.patches = {{{StreamKind::Audio, 0}, {}}};
        const auto refused = edit::insertEdit(state, tl.seq, source, f(0));
        REQUIRE_FALSE(refused);
        CHECK(refused.error().code == ErrorCode::NotFound);
    }
}
