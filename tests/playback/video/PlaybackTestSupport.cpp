#include "PlaybackTestSupport.h"

#include <wabcut/media/Probe.h>
#include <wabcut/playback/video/TransportClock.h>

#include <QGuiApplication>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <thread>

namespace wabcut::playback::test {

namespace {

int g_argc = 1;
char g_argv0[] = "wabcut_playback_video_tests";
char* g_argv[] = {g_argv0, nullptr};
QGuiApplication* g_app = nullptr;

class QtApplicationListener final : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;
    void testRunStarting(const Catch::TestRunInfo&) override {
        if (!g_app) {
            g_app =
                new QGuiApplication(g_argc, g_argv); // shader resources for offscreen rendering; no window
        }
    }
    void testRunEnded(const Catch::TestRunStats&) override {
        delete g_app;
        g_app = nullptr;
    }
};

int luma8(const media::VideoFrame& f, int x, int y) {
    const media::Plane& p = f.cpu()->planes[0];
    const std::uint8_t* row = p.data + static_cast<std::ptrdiff_t>(y) * p.stride;
    switch (f.format) {
    case media::PixelFormat::P010: {
        std::uint16_t v = 0;
        std::memcpy(&v, row + 2 * x, 2);
        return v >> 8;
    }
    case media::PixelFormat::YUV420P10:
    case media::PixelFormat::YUV422P10:
    case media::PixelFormat::YUV444P10: {
        std::uint16_t v = 0;
        std::memcpy(&v, row + 2 * x, 2);
        return v >> 2;
    }
    case media::PixelFormat::RGBA8:
        return row[4 * x + 1];
    default:
        return row[x];
    }
}

} // namespace

std::string mediaPath(std::string_view name) {
    const std::u8string p = (std::filesystem::path(WABCUT_TEST_MEDIA_DIR) / name).generic_u8string();
    return std::string(reinterpret_cast<const char*>(p.data()), p.size());
}

bool haveMedia(std::string_view name) {
    return std::filesystem::is_regular_file(std::filesystem::path(WABCUT_TEST_MEDIA_DIR) / name);
}

TestProject makeProject(const std::vector<ClipSpec>& clips, int width, int height, Rate rate) {
    TestProject out;
    ProjectState& state = out.state;
    std::map<std::string, MediaId> mediaIds;
    for (const ClipSpec& c : clips) {
        if (mediaIds.count(c.media) != 0) {
            continue;
        }
        auto info = media::probe(mediaPath(c.media));
        REQUIRE(info.has_value());
        auto item = std::make_shared<MediaItem>();
        item->id = state.ids.allocate<MediaId>();
        item->name = c.media;
        item->locator.path = mediaPath(c.media);
        item->info = std::move(*info);
        mediaIds[c.media] = item->id;
        state.media.push_back(std::move(item));
    }
    auto seq = std::make_shared<Sequence>();
    seq->id = state.ids.allocate<SequenceId>();
    seq->name = "Test";
    seq->frameRate = rate;
    seq->width = width;
    seq->height = height;
    int trackCount = 0;
    for (const ClipSpec& c : clips) {
        trackCount = std::max(trackCount, c.track + 1);
    }
    std::vector<Track> tracks(static_cast<std::size_t>(trackCount));
    std::vector<std::vector<Clip>> trackClips(static_cast<std::size_t>(trackCount));
    for (Track& t : tracks) {
        t.id = state.ids.allocate<TrackId>();
        t.kind = TrackKind::Video;
    }
    for (const ClipSpec& c : clips) {
        Clip clip;
        clip.id = state.ids.allocate<ClipId>();
        clip.media = mediaIds[c.media];
        clip.stream = {StreamKind::Video, 0};
        clip.timelineRange = {framesToTicks(c.startFrame, rate), framesToTicks(c.frames, rate)};
        clip.sourceIn = framesToTicks(c.sourceInFrame, kRate60);
        clip.enabled = c.enabled;
        clip.setProperties(c.properties);
        out.clips.push_back(clip.id);
        trackClips[static_cast<std::size_t>(c.track)].push_back(std::move(clip));
    }
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        std::sort(trackClips[i].begin(), trackClips[i].end(),
                  [](const Clip& a, const Clip& b) { return a.start() < b.start(); });
        tracks[i].clips = ClipList(std::move(trackClips[i]));
        seq->videoTracks.push_back(std::make_shared<const Track>(std::move(tracks[i])));
    }
    seq->duration = seq->contentEnd();
    out.sequence = seq->id;
    state.sequences.push_back(std::move(seq));
    return out;
}

std::int64_t barcodeOf(const media::VideoFrame& frame) {
    if (!frame.isValid() || !frame.cpu() || frame.width < 512 || frame.height < 32) {
        return -1;
    }
    std::int64_t index = 0;
    for (int bit = 0; bit < 16; ++bit) {
        if (luma8(frame, bit * 32 + 16, 16) > 128) {
            index |= std::int64_t{1} << bit;
        }
    }
    return index;
}

std::vector<Presented> consume(render::CompositeMailbox& mailbox, std::chrono::milliseconds duration,
                               const std::function<bool(const std::vector<Presented>&)>& stop) {
    std::vector<Presented> out;
    const auto start = std::chrono::steady_clock::now();
    const auto refresh = std::chrono::microseconds(16'667);
    for (int k = 0;; ++k) {
        const auto due = start + refresh * k;
        if (due - start > duration) {
            break;
        }
        std::this_thread::sleep_until(due);
        const auto sel = mailbox.acquire(steadyNowNs() + 16'666'667);
        if (sel.changed && sel.composite) {
            Presented p;
            p.target = sel.composite->targetTime;
            p.epoch = sel.composite->seekEpoch;
            p.layers = static_cast<int>(sel.composite->layers.size());
            // The topmost decoded picture (a burned-in caption on top carries no frame).
            for (auto it = sel.composite->layers.rbegin(); it != sel.composite->layers.rend(); ++it) {
                if (it->frame.isValid()) {
                    p.barcode = barcodeOf(it->frame);
                    break;
                }
            }
            out.push_back(p);
            if (stop && stop(out)) {
                break;
            }
        }
    }
    return out;
}

} // namespace wabcut::playback::test

CATCH_REGISTER_LISTENER(wabcut::playback::test::QtApplicationListener)
