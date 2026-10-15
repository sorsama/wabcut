#include "PerfProjects.h"

#include <wabcut/model/ProjectOps.h>

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

namespace wabcut::perf {

namespace {

constexpr int kMedia = 200;
constexpr Rate kFps = kRate25;

Ticks frames(std::int64_t n) { return framesToTicks(n, kFps); }

template <class T>
T orThrow(std::expected<T, Error> value, const char* what) {
    if (!value) {
        throw std::runtime_error(std::string(what) + ": " + value.error().message);
    }
    return std::move(*value);
}

} // namespace

OneHourProject oneHourProject(int clipPairsPerTrack, int framesPerClip) {
    OneHourProject p;
    const Ticks mediaLength = frames(25 * 60 * 10);
    for (int m = 0; m < kMedia; ++m) {
        char name[48];
        std::snprintf(name, sizeof name, "A%03d_C%03d_0101XY.mov", m / 10, m);
        MediaItem item;
        item.name = name;
        item.locator.path = std::string("D:/footage/day") + std::to_string(m / 50 + 1) + "/" + name;
        item.info.path = item.locator.path;
        item.info.container = "mov,mp4,m4a,3gp,3g2,mj2";
        item.info.duration = mediaLength;
        VideoStreamInfo v;
        v.index = 0;
        v.codec = "h264";
        v.width = 1920;
        v.height = 1080;
        v.frameRate = kFps;
        v.duration = mediaLength;
        item.info.video.push_back(v);
        AudioStreamInfo a;
        a.index = 1;
        a.codec = "aac";
        a.channels = 2;
        a.duration = mediaLength;
        item.info.audio.push_back(a);
        auto added = orThrow(edit::addMedia(p.state, item), "addMedia");
        p.state = std::move(added.state);
    }
    std::vector<const MediaItem*> media;
    for (const MediaPtr& m : p.state.media) {
        media.push_back(m.get());
    }

    edit::SequenceSettings settings;
    settings.name = "One hour";
    settings.frameRate = kFps;
    settings.videoTracks = 2;
    settings.audioTracks = 2;
    auto created = orThrow(edit::addSequence(p.state, settings), "addSequence");
    p.state = std::move(created.state);
    p.sequence = created.id;

    auto seq = std::make_shared<Sequence>(*p.state.findSequence(p.sequence));
    for (int t = 0; t < 2; ++t) {
        auto video = std::make_shared<Track>(*seq->videoTracks[static_cast<std::size_t>(t)]);
        auto audio = std::make_shared<Track>(*seq->audioTracks[static_cast<std::size_t>(t)]);
        for (int i = 0; i < clipPairsPerTrack; ++i) {
            const LinkGroupId group = p.state.ids.allocate<LinkGroupId>();
            // V2 / A2 (B-roll) cycle through the media in another order than V1 / A1.
            const MediaItem& m = *media[static_cast<std::size_t>((t == 0 ? i : i * 7 + 3) % kMedia)];
            for (auto* list : {&video, &audio}) {
                Clip c;
                c.id = p.state.ids.allocate<ClipId>();
                c.media = m.id;
                c.stream = {list == &video ? StreamKind::Video : StreamKind::Audio, 0};
                c.timelineRange = {frames(static_cast<std::int64_t>(i) * framesPerClip), frames(framesPerClip)};
                c.sourceIn = frames(static_cast<std::int64_t>((i * 13) % 400) * framesPerClip);
                c.setContent(m.defaultClipContent());
                c.linkGroup = group;
                if (t == 0) {
                    (list == &video ? p.v1 : p.a1).push_back(c.id);
                }
                (*list)->clips.push_back(std::move(c));
                ++p.clips;
            }
        }
        seq->videoTracks[static_cast<std::size_t>(t)] = video;
        seq->audioTracks[static_cast<std::size_t>(t)] = audio;
    }
    const Ticks length = frames(static_cast<std::int64_t>(clipPairsPerTrack) * framesPerClip);
    for (int k = 0; k < 100; ++k) {
        Marker marker;
        marker.start = Ticks{length.value / 100 * k};
        marker.name = "Marker " + std::to_string(k + 1);
        marker.colorIndex = k % 8;
        seq->markers.push_back(marker);
    }
    seq->duration = seq->contentEnd();
    const IdGenerator ids = p.state.ids;
    p.state = p.state.withSequence(seq);
    p.state.ids = ids;
    if (const auto problem = validate(p.state, Validation::Structure); !problem) {
        throw std::runtime_error("the one-hour project does not validate: " + problem.error().message);
    }
    return p;
}

} // namespace wabcut::perf
