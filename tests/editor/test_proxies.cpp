// Progressive media readiness: the automatic proxy policy, the resource governor, the proxy queue (priority,
// preemption, pausing for the foreground), the proxy registry (auto proxies, provenance in the saved project,
// rediscovery from the proxy folder's index, stale proxies regenerated after a relink, Delete proxies) and the
// cache inspector.

#include <wabcut/editor/CacheInspector.h>
#include <wabcut/editor/MediaImport.h>
#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/editor/ProxyManager.h>

#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::editor;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

fs::path testMedia(const char* name) {
    return fs::path(WABCUT_TEST_MEDIA_DIR) / name;
}

fs::path workDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "editor" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

ProjectState projectWithSequence() {
    auto created = edit::addSequence(ProjectState{}, edit::SequenceSettings{});
    REQUIRE(created);
    return std::move(created->state);
}

MediaId importOne(EditSession& session, const fs::path& file, const fs::path& projectFile) {
    const std::vector<fs::path> files{file};
    const MediaImportResult r = importMediaFiles(session, files, {.projectFile = projectFile});
    REQUIRE(r.failed.empty());
    REQUIRE(r.added.size() == 1);
    return r.added.front();
}

/// Polls `pm` until no proxy is queued, generating or being probed for (or `timeout`).
bool settle(ProxyManager& pm, std::chrono::seconds timeout = 120s) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    int quiet = 0;
    while (std::chrono::steady_clock::now() < until) {
        pm.poll();
        const ProxyCounts c = pm.counts();
        quiet = (c.queued == 0 && c.generating == 0 && pm.queue().statuses().empty() && !pm.probing()) ? quiet + 1 : 0;
        if (quiet >= 10) {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }
    return false;
}

VideoStreamInfo stream(const char* codec, int w, int h, int bits, const char* chroma) {
    VideoStreamInfo s;
    s.codec = codec;
    s.width = w;
    s.height = h;
    s.bitDepth = bits;
    s.chromaSubsampling = chroma;
    return s;
}

/// A fake transcode: `frames` frames of `frameMs`, yielding before each one.
ProxyQueue::Transcoder fakeTranscoder(int frames, std::chrono::milliseconds frameMs) {
    return [=](const ProxyJob& job, const exporter::ProxyControl& control) -> std::expected<exporter::ProxyResult, Error> {
        for (int i = 0; i < frames; ++i) {
            if (control.stop.stop_requested() || (control.yield && !control.yield())) {
                return std::unexpected(Error{ErrorCode::Cancelled, "cancelled"});
            }
            std::this_thread::sleep_for(frameMs);
            if (control.onProgress) {
                control.onProgress(i + 1, frames);
            }
        }
        exporter::ProxyResult r;
        r.file = job.output;
        r.frames = frames;
        return r;
    };
}

bool hardwareDecodes(const fs::path& file) {
    auto d = media::VideoDecoder::open(pathToUtf8(file), -1, media::DecoderOptions{});
    return d && (*d)->isHardwareAccelerated();
}

} // namespace

TEST_CASE("Proxy policy: what Auto proxies, and why", "[editor][proxy]") {
    const DecodeCapability none{};
    const DecodeCapability software{true, false, 16};
    const DecodeCapability gpu{true, true, 16};
    const DecodeCapability weak{true, false, 4};

    // H.264 10-bit 4:2:2 (no hardware decoder on an RTX 3060), unless the GPU says it decodes it.
    const VideoStreamInfo h264422 = stream("h264", 1280, 720, 10, "4:2:2");
    CHECK(needsCapabilityProbe(h264422));
    CHECK(proxyReason(h264422, software, 0) == "H.264 4:2:2 10-bit: no hardware decoder here");
    CHECK(proxyReason(h264422, none, 0) == "H.264 4:2:2 10-bit: no hardware decoder here");
    CHECK(proxyReason(h264422, gpu, 0).empty());
    CHECK_FALSE(proxyReason(stream("h264", 1920, 1080, 8, "4:2:2"), software, 0).empty());
    // 8-bit 4:2:0 H.264 plays: no proxy, no probe.
    const VideoStreamInfo h264 = stream("h264", 1920, 1080, 8, "4:2:0");
    CHECK_FALSE(needsCapabilityProbe(h264));
    CHECK(proxyReason(h264, none, 0).empty());
    // Larger than 4K, whatever the codec.
    CHECK(proxyReason(stream("prores", 4096, 2160, 10, "4:2:2"), gpu, 0) == "4096x2160 is larger than 4K");
    CHECK(proxyReason(stream("h264", 3840, 2160, 8, "4:2:0"), gpu, 0).empty());
    // Long-GOP HEVC 10-bit: only on weak machines (no hardware decoding and few cores, or above 1080p).
    const VideoStreamInfo hevc1080 = stream("hevc", 1920, 1080, 10, "4:2:0");
    CHECK(proxyReason(hevc1080, gpu, 0).empty());
    CHECK(proxyReason(hevc1080, software, 0).empty());
    CHECK(proxyReason(hevc1080, weak, 0) == "HEVC 10-bit long-GOP without hardware decoding");
    CHECK_FALSE(proxyReason(stream("hevc", 3840, 2160, 10, "4:2:0"), software, 0).empty());
    // Stats-driven: media that dropped frames in playback.
    CHECK(proxyReason(h264, gpu, kLateFramesForProxy - 1).empty());
    CHECK(proxyReason(h264, gpu, kLateFramesForProxy) == "dropped 3 frames in playback");
    // Alpha is never proxied (the proxy codecs have none).
    VideoStreamInfo alpha = stream("prores", 5000, 3000, 10, "4:4:4");
    alpha.alpha = AlphaMode::Straight;
    CHECK(proxyReason(alpha, none, 99).empty());

    CHECK(parsePolicy(policyName(ProxyPolicy::Always)) == ProxyPolicy::Always);
    CHECK(parsePolicy("auto") == ProxyPolicy::Auto);
    CHECK_FALSE(parsePolicy("sometimes").has_value());
}

TEST_CASE("Proxy policy: high frame rates and UHD 4K long-GOP without hardware decoding; length alone is no reason",
          "[editor][proxy]") {
    const DecodeCapability none{};
    const DecodeCapability software{true, false, 16};
    const DecodeCapability gpu{true, true, 16};
    const auto at = [](VideoStreamInfo s, Rate rate) {
        s.frameRate = rate;
        return s;
    };

    // A 240 fps 1080p capture: proxied without hardware decoding (unknown counts as none), not with it.
    const VideoStreamInfo hfr = at(stream("hevc", 1920, 1080, 8, "4:2:0"), Rate{240, 1});
    CHECK(needsCapabilityProbe(hfr));
    CHECK(proxyReason(hfr, software, 0) == "240 fps 1080p without hardware decoding");
    CHECK(proxyReason(hfr, none, 0) == "240 fps 1080p without hardware decoding");
    CHECK(proxyReason(hfr, gpu, 0).empty());
    CHECK(proxyReason(at(stream("h264", 1920, 1080, 8, "4:2:0"), Rate{144, 1}), software, 0) ==
          "144 fps 1080p without hardware decoding");
    CHECK(proxyReason(at(stream("h264", 1920, 1080, 8, "4:2:0"), Rate{120000, 1001}), software, 0) ==
          "119.88 fps 1080p without hardware decoding");
    // Above 1080p a high frame rate is proxied whatever decodes it, so no trial open is needed.
    const VideoStreamInfo hfr1440 = at(stream("h264", 2560, 1440, 8, "4:2:0"), Rate{240, 1});
    CHECK_FALSE(needsCapabilityProbe(hfr1440));
    CHECK(proxyReason(hfr1440, gpu, 0) == "240 fps 1440p: a high frame rate above 1080p");
    // Up to 60 fps nothing changes.
    const VideoStreamInfo p60 = at(stream("h264", 2560, 1440, 8, "4:2:0"), kRate60);
    CHECK_FALSE(needsCapabilityProbe(p60));
    CHECK(proxyReason(p60, none, 0).empty());
    CHECK(proxyReason(at(stream("h264", 1920, 1080, 8, "4:2:0"), Rate{60000, 1001}), software, 0).empty());

    // UHD 4K long-GOP: proxied without hardware decoding; intra codecs are not long-GOP.
    const VideoStreamInfo uhd = at(stream("h264", 3840, 2160, 8, "4:2:0"), kRate60);
    CHECK(needsCapabilityProbe(uhd));
    CHECK(proxyReason(uhd, gpu, 0).empty());
    CHECK(proxyReason(uhd, software, 0) == "3840x2160 long-GOP H.264 without hardware decoding");
    CHECK(proxyReason(at(stream("av1", 3840, 2160, 10, "4:2:0"), kRate30), none, 0) ==
          "3840x2160 long-GOP AV1 without hardware decoding");
    const VideoStreamInfo prores = stream("prores", 3840, 2160, 10, "4:2:2");
    CHECK_FALSE(needsCapabilityProbe(prores));
    CHECK(proxyReason(prores, software, 0).empty());
    // The earlier rules keep their reasons: HEVC 10-bit 4K is reported as such.
    CHECK(proxyReason(stream("hevc", 3840, 2160, 10, "4:2:0"), software, 0) ==
          "HEVC 10-bit long-GOP without hardware decoding");

    // Length alone is no reason: a 2-hour 1440p60 VOD plays (and seeks) like a short clip of the same GOP.
    VideoStreamInfo vod = p60;
    vod.duration = Ticks::fromSeconds(2 * 3600);
    CHECK_FALSE(needsCapabilityProbe(vod));
    CHECK(proxyReason(vod, none, 0).empty());
    CHECK(proxyReason(vod, software, 0).empty());
    // ...unless it is also one of the above.
    VideoStreamInfo longHfr = hfr;
    longHfr.duration = Ticks::fromSeconds(30 * 60);
    CHECK(proxyReason(longHfr, software, 0) == "240 fps 1080p without hardware decoding");
}

TEST_CASE("Resource governor: background work waits while the foreground is busy", "[editor][proxy]") {
    ResourceGovernor g;
    CHECK_FALSE(g.busy());
    CHECK(g.waitIdle({}));
    CHECK(g.waits() == 0);
    g.setActive(ResourceGovernor::Playback, true);
    g.setActive(ResourceGovernor::Gesture, true);
    CHECK(g.activities() == (ResourceGovernor::Playback | ResourceGovernor::Gesture));
    std::atomic<bool> released{false};
    std::thread t([&] {
        std::this_thread::sleep_for(60ms);
        g.setActive(ResourceGovernor::Playback, false);
        std::this_thread::sleep_for(60ms);
        released = true;
        g.setActive(ResourceGovernor::Gesture, false);
    });
    CHECK(g.waitIdle({}));
    CHECK(released.load());
    CHECK(g.waits() == 1);
    t.join();
    // A stop request or the abort hook ends the wait.
    g.setActive(ResourceGovernor::Export, true);
    std::stop_source stop;
    stop.request_stop();
    CHECK_FALSE(g.waitIdle(stop.get_token()));
    CHECK_FALSE(g.waitIdle({}, [] { return true; }));
}

TEST_CASE("Proxy queue: most urgent first, preemption, pausing for the foreground", "[editor][proxy]") {
    auto governor = std::make_shared<ResourceGovernor>();
    const auto job = [](std::uint64_t id, ProxyPriority p) {
        ProxyJob j;
        j.media = MediaId{id};
        j.priority = p;
        j.output = "out-" + std::to_string(id) + ".mov";
        j.reason = "test";
        return j;
    };

    SECTION("order: under the playhead, on the timeline, selected, background; FIFO within a rank") {
        ProxyQueue q(governor, fakeTranscoder(3, 1ms));
        q.setPaused(true);
        q.enqueue(job(1, ProxyPriority::Background));
        q.enqueue(job(2, ProxyPriority::Selected));
        q.enqueue(job(3, ProxyPriority::Background));
        q.enqueue(job(4, ProxyPriority::OnTimeline));
        q.enqueue(job(5, ProxyPriority::UnderPlayhead));
        q.setPriority(MediaId{3}, ProxyPriority::OnTimeline); // re-ranked; queued before 4, so first at that rank
        const auto queued = q.statuses();
        REQUIRE(queued.size() == 5);
        CHECK(queued.front().media == MediaId{5});
        q.setPaused(false);
        REQUIRE(q.waitIdle(5s));
        const std::vector<MediaId> expected{MediaId{5}, MediaId{3}, MediaId{4}, MediaId{2}, MediaId{1}};
        CHECK(q.startOrder() == expected);
        CHECK(q.takeFinished().size() == 5);
    }
    SECTION("a more urgent job preempts a background one, which runs again afterwards") {
        ProxyQueue q(governor, fakeTranscoder(40, 5ms));
        q.enqueue(job(1, ProxyPriority::Background));
        while (q.statuses().empty() || q.statuses().front().progress < 0.1) {
            std::this_thread::sleep_for(2ms);
        }
        q.enqueue(job(2, ProxyPriority::UnderPlayhead));
        REQUIRE(q.waitIdle(10s));
        const std::vector<MediaId> expected{MediaId{1}, MediaId{2}, MediaId{1}};
        CHECK(q.startOrder() == expected);
        const auto finished = q.takeFinished();
        REQUIRE(finished.size() == 2); // the preempted attempt is not a result
        CHECK(finished[0].job.media == MediaId{2});
        CHECK(finished[1].job.media == MediaId{1});
        CHECK(finished[1].result.has_value());
    }
    SECTION("the governor pauses a running job between frames; cancel ends it") {
        ProxyQueue q(governor, fakeTranscoder(60, 5ms));
        q.enqueue(job(1, ProxyPriority::Background));
        while (q.statuses().empty() || q.statuses().front().progress < 0.05) {
            std::this_thread::sleep_for(2ms);
        }
        governor->setActive(ResourceGovernor::Playback, true);
        std::this_thread::sleep_for(50ms); // the frame in flight finishes
        const double held = q.statuses().front().progress;
        std::this_thread::sleep_for(150ms);
        REQUIRE(q.statuses().size() == 1);
        CHECK(q.statuses().front().state == ProxyState::Paused);
        CHECK(q.statuses().front().progress == held); // no frame while playing
        governor->setActive(ResourceGovernor::Playback, false);
        std::this_thread::sleep_for(50ms);
        CHECK(q.statuses().front().state == ProxyState::Generating);
        CHECK(q.statuses().front().progress > held);
        CHECK(governor->waits() >= 1);
        q.cancel(MediaId{1});
        REQUIRE(q.waitIdle(5s));
        const auto finished = q.takeFinished();
        REQUIRE(finished.size() == 1);
        REQUIRE_FALSE(finished[0].result.has_value());
        CHECK(finished[0].result.error().code == ErrorCode::Cancelled);
    }
}

TEST_CASE("Proxy manager: Auto proxies what cannot play in real time; the save carries the provenance",
          "[editor][proxy]") {
    if (!fs::exists(testMedia("h264_422_10bit.mp4")) || !fs::exists(testMedia("h264_1080p60_aac.mp4"))) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("proxy-auto");
    const fs::path project = dir / "p.wabcut";
    EditSession session;
    session.reset(projectWithSequence());
    ProjectDocument doc(session);
    REQUIRE(doc.saveAs(project));
    const MediaId heavy = importOne(session, testMedia("h264_422_10bit.mp4"), project);
    const MediaId light = importOne(session, testMedia("h264_1080p60_aac.mp4"), project);
    const bool gpu422 = hardwareDecodes(testMedia("h264_422_10bit.mp4"));

    {
        ProxyManager pm(doc, {.untitledProxyRoot = dir, .policy = ProxyPolicy::Auto});
        int changes = 0;
        pm.onChanged = [&] { ++changes; };
        pm.refresh();
        REQUIRE(settle(pm));
        CHECK(changes > 0);
        CHECK(pm.status(light).state == ProxyState::None);
        if (gpu422) {
            SUCCEED("this GPU decodes H.264 4:2:2 10-bit: nothing to proxy");
            CHECK(pm.status(heavy).state == ProxyState::None);
            return;
        }
        const ProxyStatus s = pm.status(heavy);
        REQUIRE(s.state == ProxyState::Ready);
        CHECK(s.detail == "ProRes Proxy 1/2");
        CHECK(s.file.parent_path() == dir / "p Proxies");
        CHECK(fs::is_regular_file(s.file));
        CHECK(fs::is_regular_file(dir / "p Proxies" / "proxies.json"));
        const ProxyCounts c = pm.counts();
        CHECK(c.ready == 1);
        CHECK(c.wanted == 1);
        REQUIRE(pm.table()->entries.size() == 1);
        CHECK(pm.table()->entries.at(heavy.value).path == pathToUtf8(s.file));
        CHECK(pm.table()->entries.at(heavy.value).divisor == 2);
        CHECK(pm.filesInUse().contains(s.file.lexically_normal()));
        // The undo history did not see any of it.
        CHECK(session.undoLabel() == std::optional<std::string>("Import"));
        REQUIRE(doc.save());
    }

    EditSession reopened;
    ProjectDocument doc2(reopened);
    REQUIRE(doc2.open(project));
    const MediaItem* item = reopened.snapshot()->findMedia(heavy);
    REQUIRE(item);
    REQUIRE(item->locator.proxy);
    const ProxyProvenance& p = item->locator.proxy->provenance;
    CHECK(p.generator == "wabcut-proxy 1");
    CHECK(p.settings == "prores_ks proxy 1/2 audio=copy");
    CHECK(p.source.sameContentAs(item->locator.fingerprint));
    CHECK(p.generation == 1);
    CHECK_FALSE(reopened.snapshot()->findMedia(light)->locator.proxy.has_value());
}

TEST_CASE("Proxy manager: proxies are found again after a quit without saving; Delete proxies", "[editor][proxy]") {
    if (!fs::exists(testMedia("h264_422_10bit.mp4"))) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("proxy-index");
    const fs::path project = dir / "p.wabcut";
    EditSession session;
    session.reset(projectWithSequence());
    ProjectDocument doc(session);
    const MediaId id = importOne(session, testMedia("h264_422_10bit.mp4"), project);
    REQUIRE(doc.saveAs(project)); // saved BEFORE the proxy exists: the file has no proxy
    fs::path made;
    {
        ProxyManager pm(doc, {.untitledProxyRoot = dir, .policy = ProxyPolicy::Off});
        pm.refresh();
        CHECK(pm.status(id).state == ProxyState::None); // Off: nothing automatic
        CHECK(pm.generate({id}) == 1);                   // Library > Create proxy
        REQUIRE(settle(pm));
        REQUIRE(pm.status(id).state == ProxyState::Ready);
        made = pm.status(id).file;
    } // quit without saving

    EditSession again;
    ProjectDocument doc2(again);
    REQUIRE(doc2.open(project));
    CHECK_FALSE(again.snapshot()->findMedia(id)->locator.proxy.has_value());
    ProxyManager pm(doc2, {.untitledProxyRoot = dir, .policy = ProxyPolicy::Auto});
    pm.refresh();
    REQUIRE(pm.status(id).state == ProxyState::Ready); // from "<project> Proxies/proxies.json"
    CHECK(pm.status(id).file == made);
    REQUIRE(settle(pm));
    CHECK(pm.queue().startOrder().empty()); // nothing regenerated

    CHECK(pm.deleteProxies({id}) == 1);
    CHECK_FALSE(fs::exists(made));
    CHECK(pm.status(id).state == ProxyState::None);
    CHECK(pm.table()->entries.empty());
    pm.refresh(true);
    REQUIRE(settle(pm, 5s));
    CHECK(pm.status(id).state == ProxyState::None); // a deleted proxy is not made again automatically
}

TEST_CASE("Proxy manager: a relinked original with other content makes its proxy stale; it is regenerated",
          "[editor][proxy]") {
    if (!fs::exists(testMedia("h264_422_10bit.mp4")) || !fs::exists(testMedia("vfr.mp4"))) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("proxy-stale");
    const fs::path project = dir / "p.wabcut";
    const fs::path clip = dir / "clip.mp4";
    fs::copy_file(testMedia("h264_422_10bit.mp4"), clip);
    EditSession session;
    session.reset(projectWithSequence());
    ProjectDocument doc(session);
    REQUIRE(doc.saveAs(project));
    const MediaId id = importOne(session, clip, project);
    ProxyManager pm(doc, {.untitledProxyRoot = dir, .policy = ProxyPolicy::Always});
    pm.refresh();
    REQUIRE(settle(pm));
    REQUIRE(pm.status(id).state == ProxyState::Ready);
    const fs::path first = pm.status(id).file;

    // The original is replaced by a re-render (other content, same name): it opens offline, the user relinks it.
    fs::copy_file(testMedia("vfr.mp4"), clip, fs::copy_options::overwrite_existing);
    REQUIRE(doc.refreshMediaStatus());
    REQUIRE(session.snapshot()->findMedia(id)->locator.offline);
    CHECK(pm.table()->entries.contains(id.value)); // until the registry looks again...
    REQUIRE(doc.relink(id, clip, true));
    pm.refresh();
    const ProxyStatus during = pm.status(id);
    CHECK((during.state == ProxyState::Queued || during.state == ProxyState::Generating ||
           during.state == ProxyState::Stale));
    CHECK_FALSE(pm.table()->entries.contains(id.value)); // ...a stale proxy is never shown
    ProjectState saved = session.stateForSave();
    pm.applyTo(saved);
    CHECK_FALSE(saved.findMedia(id)->locator.proxy.has_value()); // nor saved
    REQUIRE(settle(pm));
    const ProxyStatus after = pm.status(id);
    REQUIRE(after.state == ProxyState::Ready);
    CHECK(after.file != first);
    CHECK_FALSE(fs::exists(first)); // the old generation is gone
    pm.applyTo(saved);
    REQUIRE(saved.findMedia(id)->locator.proxy.has_value());
    CHECK(saved.findMedia(id)->locator.proxy->provenance.generation == 2);
    CHECK(saved.findMedia(id)->locator.proxy->provenance.source.sameContentAs(
        session.snapshot()->findMedia(id)->locator.fingerprint));
}

TEST_CASE("Proxy manager: Auto proxies media that drops frames in playback", "[editor][proxy]") {
    if (!fs::exists(testMedia("h264_1080p60_aac.mp4"))) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("proxy-late");
    EditSession session;
    session.reset(projectWithSequence());
    ProjectDocument doc(session);
    const MediaId id = importOne(session, testMedia("h264_1080p60_aac.mp4"), {});
    ProxyManagerOptions options{.untitledProxyRoot = dir, .policy = ProxyPolicy::Auto};
    options.transcoder = [](const ProxyJob& job, const exporter::ProxyControl&) -> std::expected<exporter::ProxyResult, Error> {
        fs::create_directories(job.output.parent_path());
        std::ofstream(job.output, std::ios::binary) << "proxy";
        exporter::ProxyResult r;
        r.file = job.output;
        return r;
    };
    ProxyManager pm(doc, std::move(options));
    pm.refresh();
    REQUIRE(settle(pm, 10s));
    CHECK(pm.status(id).state == ProxyState::None); // 1080p 8-bit 4:2:0: plays in real time
    pm.setLateFrames({{id.value, kLateFramesForProxy - 1}});
    REQUIRE(settle(pm, 10s));
    CHECK(pm.status(id).state == ProxyState::None);
    pm.setLateFrames({{id.value, 7}}); // the scheduler kept waiting for this clip
    CHECK(pm.queue().contains(id));
    REQUIRE(settle(pm, 10s));
    CHECK(pm.status(id).state == ProxyState::Ready);
    REQUIRE(pm.queue().startOrder().size() == 1);
    CHECK(pm.queue().takeFinished().empty()); // poll() took it
}

TEST_CASE("Proxy queue: long recordings wait behind shorter media of the same urgency and keep their progress",
          "[editor][proxy]") {
    auto governor = std::make_shared<ResourceGovernor>();
    const auto job = [](std::uint64_t id, ProxyPriority p, bool longSource) {
        ProxyJob j;
        j.media = MediaId{id};
        j.priority = p;
        j.output = "out-" + std::to_string(id) + ".mov";
        j.reason = "test";
        j.longSource = longSource;
        return j;
    };

    SECTION("order: urgency first, then shorter media, then first come first served") {
        ProxyQueue q(governor, fakeTranscoder(3, 1ms));
        q.setPaused(true);
        q.enqueue(job(1, ProxyPriority::OnTimeline, true));
        q.enqueue(job(2, ProxyPriority::Background, false));
        q.enqueue(job(3, ProxyPriority::OnTimeline, false));
        q.enqueue(job(4, ProxyPriority::UnderPlayhead, true)); // long, but the most urgent
        q.enqueue(job(5, ProxyPriority::OnTimeline, false));
        const auto queued = q.statuses();
        REQUIRE(queued.size() == 5);
        CHECK(queued.front().media == MediaId{4});
        CHECK(queued[1].media == MediaId{3});
        q.setPaused(false);
        REQUIRE(q.waitIdle(5s));
        const std::vector<MediaId> expected{MediaId{4}, MediaId{3}, MediaId{5}, MediaId{1}, MediaId{2}};
        CHECK(q.startOrder() == expected);
    }
    SECTION("a running long recording is not preempted once it is under way") {
        ProxyQueue q(governor, fakeTranscoder(60, 5ms));
        q.enqueue(job(1, ProxyPriority::Background, true));
        while (q.statuses().empty() || q.statuses().front().progress < 0.1) {
            std::this_thread::sleep_for(2ms);
        }
        // More urgent work, arriving so or re-ranked: neither restarts the long transcode.
        q.enqueue(job(2, ProxyPriority::UnderPlayhead, false));
        q.enqueue(job(3, ProxyPriority::Background, false));
        q.setPriority(MediaId{3}, ProxyPriority::OnTimeline);
        REQUIRE(q.waitIdle(10s));
        const std::vector<MediaId> expected{MediaId{1}, MediaId{2}, MediaId{3}}; // no restart of 1
        CHECK(q.startOrder() == expected);
        const auto finished = q.takeFinished();
        REQUIRE(finished.size() == 3);
        CHECK(finished[0].job.media == MediaId{1});
        CHECK(finished[0].result.has_value());
    }
}

TEST_CASE("Proxy manager: a long 240 fps recording is queued as a long job; a long 1440p60 one gets no proxy",
          "[editor][proxy]") {
    const fs::path dir = workDir("proxy-long");
    // Two 2-hour recordings known only to the project: probing is off (no hardware decoding assumed) and the
    // transcode is a fake, so no file is decoded.
    const auto recording = [&](std::uint64_t id, const char* name, VideoStreamInfo video) {
        MediaItem m;
        m.id = MediaId{id};
        m.name = name;
        m.locator.path = pathToUtf8(dir / name);
        m.info.path = m.locator.path;
        m.info.duration = Ticks::fromSeconds(2 * 3600);
        video.index = 0;
        video.duration = m.info.duration;
        m.info.video.push_back(video);
        return m;
    };
    VideoStreamInfo vod = stream("h264", 2560, 1440, 8, "4:2:0");
    vod.frameRate = kRate60;
    VideoStreamInfo hfr = stream("hevc", 1920, 1080, 8, "4:2:0");
    hfr.frameRate = Rate{240, 1};
    ProjectState state = projectWithSequence();
    state.media.push_back(std::make_shared<const MediaItem>(recording(901, "vod.mkv", vod)));
    state.media.push_back(std::make_shared<const MediaItem>(recording(902, "slowmo.mp4", hfr)));
    EditSession session;
    session.reset(state);
    ProjectDocument doc(session);

    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, bool>> seen; // media, ProxyJob::longSource
    ProxyManagerOptions options{.untitledProxyRoot = dir, .policy = ProxyPolicy::Auto, .probeHardware = false};
    options.transcoder = [&](const ProxyJob& job,
                             const exporter::ProxyControl&) -> std::expected<exporter::ProxyResult, Error> {
        {
            std::lock_guard lock(mutex);
            seen.emplace_back(job.media.value, job.longSource);
        }
        fs::create_directories(job.output.parent_path());
        std::ofstream(job.output, std::ios::binary) << "proxy";
        exporter::ProxyResult r;
        r.file = job.output;
        return r;
    };
    ProxyManager pm(doc, std::move(options));
    pm.refresh();
    REQUIRE(settle(pm, 10s));
    CHECK(pm.status(MediaId{901}).state == ProxyState::None); // length alone is no reason
    CHECK(pm.status(MediaId{902}).state == ProxyState::Ready);
    std::lock_guard lock(mutex);
    const std::vector<std::pair<std::uint64_t, bool>> expected{{902, true}};
    CHECK(seen == expected);
}

TEST_CASE("Cache inspector: sizes, cleaning regenerable data, per-cache limits", "[editor][cache]") {
    const fs::path dir = workDir("cache-inspector");
    const auto write = [](const fs::path& f, std::size_t bytes) {
        fs::create_directories(f.parent_path());
        std::ofstream(f, std::ios::binary) << std::string(bytes, 'x');
    };
    write(dir / "thumbnails" / "a.wthumb", 1000);
    write(dir / "thumbnails" / "b.wthumb", 2000);
    write(dir / "thumbnails" / "notes.txt", 50); // not a cache file
    write(dir / "peaks" / "a.wpeaks", 300);
    write(dir / "proxies" / "used.mov", 5000);
    std::this_thread::sleep_for(20ms);
    write(dir / "proxies" / "orphan-old.mov", 4000);
    std::this_thread::sleep_for(20ms);
    write(dir / "proxies" / "orphan-new.mov", 3000);
    const std::set<fs::path> inUse{(dir / "proxies" / "used.mov").lexically_normal()};

    CacheInspector inspector;
    inspector.setLocations({{"proxies", "Proxies", dir / "proxies", 0, {".mov"}},
                            {"thumbnails", "Thumbnails", dir / "thumbnails", 0, {".wthumb"}},
                            {"peaks", "Waveforms", dir / "peaks", 0, {".wpeaks"}},
                            {"analysis", "Analysis", dir / "analysis", 0, {}}});
    auto usage = inspector.scan(inUse);
    REQUIRE(usage.size() == 4);
    CHECK(usage[0].bytes == 12000);
    CHECK(usage[0].files == 3);
    CHECK(usage[0].inUseBytes == 5000);
    CHECK(usage[1].bytes == 3000);
    CHECK(usage[2].bytes == 300);
    CHECK(usage[3].bytes == 0); // missing folder

    // Limit: the oldest unused proxies go first; the one in use never does.
    inspector.setLimit("proxies", 8500);
    CHECK(inspector.enforceLimits(inUse) == 4000);
    CHECK_FALSE(fs::exists(dir / "proxies" / "orphan-old.mov"));
    CHECK(fs::exists(dir / "proxies" / "orphan-new.mov"));
    inspector.setLimit("proxies", 1000);
    CHECK(inspector.enforceLimits(inUse) == 3000);
    CHECK(fs::exists(dir / "proxies" / "used.mov"));

    // Clean regenerable data: everything not in use.
    CHECK(inspector.clean("thumbnails", inUse) == 3000);
    CHECK(fs::exists(dir / "thumbnails" / "notes.txt"));
    CHECK(inspector.clean("", inUse) == 300);
    usage = inspector.scan(inUse);
    CHECK(usage[0].bytes == 5000);
    CHECK(usage[1].bytes == 0);

    CHECK(formatBytes(0) == "0 B");
    CHECK(formatBytes(1500) == "1.5 KB");
    CHECK(formatBytes(340'000'000) == "340 MB");
    CHECK(formatBytes(1'234'000'000) == "1.2 GB");
}
