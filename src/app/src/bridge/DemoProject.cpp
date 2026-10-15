#include "DemoProject.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>

#include <QDebug>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace wabcut::app {

namespace {

constexpr Rate kDemoRate = kRate23_976;

Ticks frames(std::int64_t n) {
    return framesToTicks(n, kDemoRate);
}

// Demo construction cannot fail unless the model contract changes; fail loudly in that case.
template <class T>
T take(std::expected<T, Error> result, const char* what) {
    if (!result) {
        qFatal("buildDemoProject: %s failed: %s", what, result.error().message.c_str());
    }
    return std::move(*result);
}

struct MediaSpec {
    const char* name;
    std::int64_t frames;
    bool video;
    bool audio;
    bool still;
};

MediaItem makeMedia(const MediaSpec& spec) {
    MediaItem item;
    item.name = spec.name;
    // Demo media has no files behind it: the locator names a path that never resolves, so the timeline keeps
    // placeholder waveforms and the library its mock gradients for these items.
    item.locator.path = std::string("demo/") + spec.name;
    item.info.path = item.locator.path;
    item.info.isStillImage = spec.still;
    item.info.duration = spec.still ? Ticks{} : frames(spec.frames);
    if (spec.video) {
        VideoStreamInfo v;
        v.index = 0;
        v.codec = spec.still ? "png" : "h264";
        v.width = 1920;
        v.height = 1080;
        v.frameRate = kDemoRate;
        v.duration = item.info.duration;
        item.info.video.push_back(v);
    }
    if (spec.audio) {
        AudioStreamInfo a;
        a.index = spec.video ? 1 : 0;
        a.codec = "pcm_s24le";
        a.channels = 2;
        a.channelLayout = "stereo";
        a.duration = item.info.duration;
        item.info.audio.push_back(a);
    }
    return item;
}

class Builder {
public:
    explicit Builder(ProjectState state) : m_state(std::move(state)) {}

    MediaId addMedia(const MediaSpec& spec) {
        auto created = take(edit::addMedia(m_state, makeMedia(spec)), "addMedia");
        m_state = std::move(created.state);
        return created.id;
    }

    SequenceId addSequence(const char* name, std::int32_t width, std::int32_t height) {
        edit::SequenceSettings settings;
        settings.name = name;
        settings.frameRate = kDemoRate;
        settings.width = width;
        settings.height = height;
        settings.videoTracks = 3;
        settings.audioTracks = 3;
        auto created = take(edit::addSequence(m_state, settings), "addSequence");
        m_state = std::move(created.state);
        return created.id;
    }

    TrackId track(SequenceId seq, TrackKind kind, std::size_t index) const {
        return m_state.findSequence(seq)->tracks(kind)[index]->id;
    }

    // Overwrites `media` (from source frame 0) at `startFrame` onto the given tracks; one clip per patch,
    // linked when there are several. Renames the placed clips and sets their label colour.
    std::vector<ClipId> place(SequenceId seq, MediaId media, std::int64_t startFrame, std::int64_t lengthFrames,
                              const char* clipName, std::int32_t color, std::vector<edit::SourcePatch> patches) {
        edit::SourceClip source{media, {Ticks{}, frames(lengthFrames)}, std::move(patches)};
        auto placed = take(edit::overwriteEdit(m_state, seq, source, frames(startFrame)), "overwriteEdit");
        m_state = std::move(placed.state);
        edit::ClipAttributeChange change;
        change.name = clipName;
        change.colorIndex = color;
        m_state = take(edit::setClipAttributes(m_state, seq, placed.placed, change), "setClipAttributes").state;
        return placed.placed;
    }

    void addMarker(SequenceId seq, std::int64_t frame, const char* name, std::int32_t color) {
        Marker marker;
        marker.start = frames(frame);
        marker.name = name;
        marker.colorIndex = color;
        m_state = take(edit::addMarker(m_state, seq, std::move(marker)), "addMarker").state;
    }

    const ProjectState& state() const { return m_state; }
    ProjectState release() { return std::move(m_state); }

private:
    ProjectState m_state;
};

edit::SourcePatch videoTo(TrackId track) {
    return {{StreamKind::Video, 0}, track};
}
edit::SourcePatch audioTo(TrackId track) {
    return {{StreamKind::Audio, 0}, track};
}

} // namespace

DemoProject buildDemoProject() {
    Builder b{ProjectState{}};

    // Media (names as in the library panel). Stills stand in for graphics until titles exist.
    const MediaId interviewA = b.addMedia({"Interview A.mov", 1728, true, true, false});
    const MediaId interviewB = b.addMedia({"Interview B.mov", 1560, true, true, false});
    const MediaId cityBroll = b.addMedia({"City B-roll.mp4", 576, true, false, false});
    const MediaId droneCoast = b.addMedia({"Drone Coast.mp4", 912, true, false, false});
    const MediaId streetWalk = b.addMedia({"Street Walk.mov", 744, true, true, false});
    const MediaId productCloseUp = b.addMedia({"Product Close-up.mp4", 288, true, false, false});
    const MediaId musicBed = b.addMedia({"Music Bed - Lofi.wav", 3120, false, true, false});
    const MediaId whoosh = b.addMedia({"Whoosh SFX.wav", 48, false, true, false});
    const MediaId voTake = b.addMedia({"VO Take 3.wav", 672, false, true, false});
    const MediaId lowerThird = b.addMedia({"Lower Third.png", 0, true, false, true});
    const MediaId titleCard = b.addMedia({"Title Card.png", 0, true, false, true});
    const MediaId logoBug = b.addMedia({"Logo Bug.png", 0, true, false, true});
    const MediaId outro = b.addMedia({"Outro.mov", 480, true, false, false});

    // Sequence 01: the M1 timeline.
    const SequenceId seq = b.addSequence("Sequence 01", 1920, 1080);
    const TrackId v1 = b.track(seq, TrackKind::Video, 0);
    const TrackId v2 = b.track(seq, TrackKind::Video, 1);
    const TrackId v3 = b.track(seq, TrackKind::Video, 2);
    const TrackId a1 = b.track(seq, TrackKind::Audio, 0);
    const TrackId a2 = b.track(seq, TrackKind::Audio, 1);
    const TrackId a3 = b.track(seq, TrackKind::Audio, 2);

    b.place(seq, interviewA, 0, 576, "Interview A", 0, {videoTo(v1), audioTo(a1)});
    b.place(seq, interviewB, 576, 576, "Interview B", 0, {videoTo(v1), audioTo(a1)});
    b.place(seq, streetWalk, 1152, 432, "Street Walk", 2, {videoTo(v1), audioTo(a1)});
    b.place(seq, titleCard, 1584, 96, "Title Card", 5, {videoTo(v1)});
    b.place(seq, outro, 1680, 240, "Outro", 6, {videoTo(v1)});
    const ClipId city = b.place(seq, cityBroll, 96, 336, "City B-roll", 1, {videoTo(v2)}).front();
    b.place(seq, droneCoast, 720, 336, "Drone Coast", 4, {videoTo(v2)});
    b.place(seq, productCloseUp, 1680, 192, "Product Close-up", 7, {videoTo(v2)});
    b.place(seq, lowerThird, 192, 144, "Lower Third", 3, {videoTo(v3)});
    b.place(seq, logoBug, 1440, 240, "Logo Bug", 5, {videoTo(v3)});
    b.place(seq, whoosh, 552, 48, "Whoosh SFX", 3, {audioTo(a2)});
    b.place(seq, voTake, 1200, 384, "VO Take 3", 7, {audioTo(a2)});
    b.place(seq, musicBed, 0, 1920, "Music Bed - Lofi", 6, {audioTo(a3)});

    b.addMarker(seq, 192, "Lower third in", 3);
    b.addMarker(seq, 576, "Cut to Interview B", 0);
    b.addMarker(seq, 720, "Drone reveal", 4);
    b.addMarker(seq, 1200, "VO starts", 7);
    b.addMarker(seq, 1680, "Outro", 6);

    // Two more sequences for the sequence switcher.
    const SequenceId social = b.addSequence("Social Cut 9x16", 1080, 1920);
    b.place(social, interviewA, 0, 240, "Interview A", 0,
            {videoTo(b.track(social, TrackKind::Video, 0)), audioTo(b.track(social, TrackKind::Audio, 0))});
    b.place(social, cityBroll, 48, 120, "City B-roll", 1, {videoTo(b.track(social, TrackKind::Video, 1))});
    const SequenceId trailer = b.addSequence("Trailer 30s", 1920, 1080);
    b.place(trailer, droneCoast, 0, 240, "Drone Coast", 4, {videoTo(b.track(trailer, TrackKind::Video, 0))});
    b.place(trailer, musicBed, 0, 720, "Music Bed - Lofi", 6, {audioTo(b.track(trailer, TrackKind::Audio, 0))});

    return {b.release(), seq, city, frames(12 * 24 + 8)};
}

DemoProject buildStressProject(const StressProjectSpec& spec) {
    Builder b{ProjectState{}};
    const int tracks = std::max(1, spec.videoTracks + spec.audioTracks);
    const int clipsPerTrack = (spec.clips + tracks - 1) / tracks;
    const std::int64_t mediaFrames = std::int64_t{clipsPerTrack} * spec.clipFrames + spec.clipFrames;
    const MediaId media = b.addMedia({"A001_C001.mov", mediaFrames, true, true, false});

    edit::SequenceSettings settings;
    settings.name = "Stress";
    settings.frameRate = kDemoRate;
    settings.videoTracks = spec.videoTracks;
    settings.audioTracks = spec.audioTracks;
    ProjectState state = b.release();
    auto created = take(edit::addSequence(state, settings), "addSequence");
    state = std::move(created.state);
    const SequenceId seqId = created.id;

    auto seq = std::make_shared<Sequence>(*state.findSequence(seqId));
    std::vector<std::shared_ptr<Track>> video;
    std::vector<std::shared_ptr<Track>> audio;
    for (const auto& t : seq->videoTracks)
        video.push_back(std::make_shared<Track>(*t));
    for (const auto& t : seq->audioTracks)
        audio.push_back(std::make_shared<Track>(*t));

    int remaining = spec.clips;
    for (int i = 0; i < clipsPerTrack && remaining > 0; ++i) {
        const std::size_t pairs = std::max(video.size(), audio.size());
        for (std::size_t p = 0; p < pairs && remaining > 0; ++p) {
            const bool both = p < video.size() && p < audio.size() && remaining >= 2;
            const LinkGroupId group = both ? state.ids.allocate<LinkGroupId>() : LinkGroupId{};
            const std::string name = "A001_C" + std::to_string(i) + "_" + std::to_string(p) + ".mov";
            for (int kind = 0; kind < 2 && remaining > 0; ++kind) {
                auto& list = kind == 0 ? video : audio;
                if (p >= list.size())
                    continue;
                Clip c;
                c.id = state.ids.allocate<ClipId>();
                c.media = media;
                c.stream = {kind == 0 ? StreamKind::Video : StreamKind::Audio, 0};
                c.timelineRange = {frames(std::int64_t{i} * spec.clipFrames), frames(spec.clipFrames)};
                c.sourceIn = frames(std::int64_t{i % 50} * spec.clipFrames);
                c.linkGroup = group;
                c.colorIndex = (i + static_cast<int>(p)) % 8;
                c.setName(name);
                list[p]->clips.push_back(std::move(c));
                --remaining;
            }
        }
    }
    seq->videoTracks.assign(video.begin(), video.end());
    seq->audioTracks.assign(audio.begin(), audio.end());
    seq->duration = seq->contentEnd(); // the explicit length edits would have grown to
    state = state.withSequence(std::move(seq));
    return {std::move(state), seqId, {}};
}

} // namespace wabcut::app
