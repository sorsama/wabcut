#include <wabcut/editor/ProxyManager.h>

#include "PathSafety.h"

#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/ImageSequence.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <tuple>

namespace wabcut::editor {

namespace {

using namespace std::chrono_literals;

bool isFile(const std::filesystem::path& p) {
    std::error_code ec;
    return !p.empty() && std::filesystem::is_regular_file(p, ec);
}

std::string hex8(std::string_view text) {
    std::uint64_t h = 1469598103934665603ull;
    for (const char c : text) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return std::format("{:08x}", static_cast<std::uint32_t>(h ^ (h >> 32)));
}

bool hasProxyableVideo(const MediaItem& item) {
    // 0.2: image sequences and spanned clips are decoded through their paths (media::detail::openInput) like any file,
    // so they get proxies too; a layered file's item is a still.
    return !item.isGenerator() && !item.info.isStillImage && !item.info.video.empty();
}

std::string prettySettings(const std::string& settings) {
    exporter::ProxySettings s;
    if (!exporter::parseProxySettings(settings, s)) {
        return settings;
    }
    const char* codec = s.codec == exporter::ProxyCodec::ProResProxy ? "ProRes Proxy"
                        : s.codec == exporter::ProxyCodec::DnxhrLb   ? "DNxHR LB"
                                                                     : "H.264 intra";
    return std::format("{} 1/{}", codec, s.divisor);
}

// ---- The proxy folder's index ("proxies.json") ----------------------------------------------------------------

constexpr const char* kIndexName = "proxies.json";

struct IndexEntry {
    std::string file; ///< file name inside the folder (UTF-8)
    std::string sourcePath;
    ProxyProvenance provenance;
};

nlohmann::json toJson(const IndexEntry& e) {
    return {{"file", e.file},
            {"sourcePath", e.sourcePath},
            {"generator", e.provenance.generator},
            {"settings", e.provenance.settings},
            {"generation", e.provenance.generation},
            {"source",
             {{"size", e.provenance.source.size},
              {"mtimeNs", e.provenance.source.mtimeNs},
              {"sampleHash", e.provenance.source.sampleHash}}}};
}

std::vector<IndexEntry> readIndex(const std::filesystem::path& folder) {
    std::vector<IndexEntry> out;
    std::ifstream in(folder / kIndexName, std::ios::binary);
    if (!in) {
        return out;
    }
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object() || !j.contains("proxies") || !j["proxies"].is_array()) {
        return out;
    }
    for (const nlohmann::json& p : j["proxies"]) {
        if (!p.is_object()) {
            continue;
        }
        IndexEntry e;
        e.file = p.value("file", "");
        e.sourcePath = p.value("sourcePath", "");
        e.provenance.generator = p.value("generator", "");
        e.provenance.settings = p.value("settings", "");
        e.provenance.generation = p.value("generation", std::uint64_t{0});
        if (p.contains("source") && p["source"].is_object()) {
            const nlohmann::json& s = p["source"];
            e.provenance.source.size = s.value("size", std::int64_t{-1});
            e.provenance.source.mtimeNs = s.value("mtimeNs", std::int64_t{0});
            e.provenance.source.sampleHash = s.value("sampleHash", "");
        }
        if (!e.file.empty()) {
            out.push_back(std::move(e));
        }
    }
    return out;
}

std::filesystem::path untitledProxyFolder(const ProxyManagerOptions& options) {
    std::error_code ec;
    const std::filesystem::path root =
        options.untitledProxyRoot.empty() ? std::filesystem::temp_directory_path(ec) / "Wabcut" : options.untitledProxyRoot;
    return root / "Untitled Proxies";
}

void writeIndex(const std::filesystem::path& folder, const std::vector<IndexEntry>& entries) {
    nlohmann::json list = nlohmann::json::array();
    for (const IndexEntry& e : entries) {
        list.push_back(toJson(e));
    }
    const nlohmann::json j = {{"version", 1}, {"generator", std::string(exporter::kProxyGenerator)}, {"proxies", list}};
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path tmp = folder / "proxies.json.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return;
        }
        out << j.dump(2);
    }
    std::filesystem::rename(tmp, folder / kIndexName, ec);
}

} // namespace

// ---- Policy -------------------------------------------------------------------------------------------------

namespace {

/// Inter-frame codecs: decoding a frame needs the frames since the last keyframe. Intra-only codecs (ProRes, DNxHR,
/// CineForm, MJPEG, FFV1, ...) decode each frame on its own.
bool isLongGop(const VideoStreamInfo& s) {
    static constexpr std::string_view kLongGop[] = {"h264", "hevc", "av1", "vp9", "vp8", "vvc", "mpeg2video", "mpeg4"};
    return std::ranges::find(kLongGop, std::string_view(s.codec)) != std::end(kLongGop);
}

bool isHighFrameRate(const VideoStreamInfo& s) {
    return s.frameRate.isValid() && s.frameRate.toDouble() >= kHighFrameRateForProxy;
}

/// UHD 4K itself (larger sizes have their own rule).
bool isUhd(const VideoStreamInfo& s) {
    const std::int64_t pixels = static_cast<std::int64_t>(s.width) * s.height;
    return pixels >= kUhdPixelsForProxy && s.width <= 3840 && s.height <= 2160;
}

/// "240", "119.88", "143.86".
std::string fpsText(Rate rate) {
    return std::format("{:g}", std::round(rate.toDouble() * 100.0) / 100.0);
}

std::string codecTitle(const std::string& codec) {
    static constexpr std::pair<std::string_view, std::string_view> kTitles[] = {
        {"h264", "H.264"}, {"hevc", "HEVC"}, {"av1", "AV1"}, {"vp9", "VP9"}, {"vp8", "VP8"}, {"vvc", "VVC"}};
    for (const auto& [name, title] : kTitles) {
        if (codec == name) {
            return std::string(title);
        }
    }
    return codec;
}

} // namespace

bool needsCapabilityProbe(const VideoStreamInfo& s) {
    const bool chroma420 = s.chromaSubsampling.empty() || s.chromaSubsampling == "4:2:0";
    const std::int64_t pixels = static_cast<std::int64_t>(s.width) * s.height;
    return (s.codec == "h264" && (s.bitDepth > 8 || !chroma420)) || (s.codec == "hevc" && s.bitDepth > 8) ||
           (isHighFrameRate(s) && pixels <= kHighFrameRatePixelsForProxy) || (isUhd(s) && isLongGop(s));
}

std::string proxyReason(const VideoStreamInfo& s, const DecodeCapability& capability, std::uint64_t lateFrames) {
    if (s.alpha != AlphaMode::None) {
        return {};
    }
    const std::int64_t pixels = static_cast<std::int64_t>(s.width) * s.height;
    if (s.width > 3840 || s.height > 2160 || pixels > kUhdPixelsForProxy) {
        return std::format("{}x{} is larger than 4K", s.width, s.height);
    }
    const bool hardware = capability.known && capability.hardwareDecodes;
    const bool chroma420 = s.chromaSubsampling.empty() || s.chromaSubsampling == "4:2:0";
    if (s.codec == "h264" && (s.bitDepth > 8 || !chroma420) && !hardware) {
        return std::format("H.264 {} {}-bit: no hardware decoder here", s.chromaSubsampling.empty() ? "4:2:0" : s.chromaSubsampling,
                           s.bitDepth);
    }
    if (s.codec == "hevc" && s.bitDepth > 8 && !hardware) {
        const int cores = capability.cpuCores > 0 ? capability.cpuCores
                                                  : static_cast<int>(std::thread::hardware_concurrency());
        if (cores < 8 || pixels > std::int64_t{1920} * 1080) {
            return std::format("HEVC {}-bit long-GOP without hardware decoding", s.bitDepth);
        }
    }
    if (isHighFrameRate(s) && pixels > kHighFrameRatePixelsForProxy) {
        return std::format("{} fps {}p: a high frame rate above 1080p", fpsText(s.frameRate), s.height);
    }
    if (isHighFrameRate(s) && !hardware) {
        return std::format("{} fps {}p without hardware decoding", fpsText(s.frameRate), s.height);
    }
    if (isUhd(s) && isLongGop(s) && !hardware) {
        return std::format("{}x{} long-GOP {} without hardware decoding", s.width, s.height, codecTitle(s.codec));
    }
    if (lateFrames >= kLateFramesForProxy) {
        return std::format("dropped {} frames in playback", lateFrames);
    }
    return {};
}

std::string_view policyName(ProxyPolicy policy) {
    switch (policy) {
    case ProxyPolicy::Off:
        return "off";
    case ProxyPolicy::Auto:
        return "auto";
    case ProxyPolicy::Always:
        return "always";
    }
    return "auto";
}

std::optional<ProxyPolicy> parsePolicy(std::string_view name) {
    for (const ProxyPolicy p : {ProxyPolicy::Off, ProxyPolicy::Auto, ProxyPolicy::Always}) {
        if (policyName(p) == name) {
            return p;
        }
    }
    return std::nullopt;
}

std::string_view stateName(ProxyState state) {
    switch (state) {
    case ProxyState::None:
        return "none";
    case ProxyState::Queued:
        return "queued";
    case ProxyState::Generating:
        return "generating";
    case ProxyState::Paused:
        return "paused";
    case ProxyState::Ready:
        return "ready";
    case ProxyState::Stale:
        return "stale";
    case ProxyState::Failed:
        return "failed";
    }
    return "none";
}

// ---- Resource governor ----------------------------------------------------------------------------------------

void ResourceGovernor::setActive(Activity activity, bool active) {
    {
        std::lock_guard lock(m_mutex);
        m_activities = active ? (m_activities | activity) : (m_activities & ~static_cast<unsigned>(activity));
    }
    m_cv.notify_all();
}

unsigned ResourceGovernor::activities() const {
    std::lock_guard lock(m_mutex);
    return m_activities;
}

bool ResourceGovernor::waitIdle(std::stop_token stop, const std::function<bool()>& abort) const {
    bool counted = false;
    for (;;) {
        {
            std::unique_lock lock(m_mutex);
            if (m_activities == 0) {
                return !stop.stop_requested();
            }
            if (!counted) {
                ++m_waits;
                counted = true;
            }
            m_cv.wait_for(lock, stop, 50ms, [&] { return m_activities == 0; });
            if (m_activities == 0) {
                return !stop.stop_requested();
            }
        }
        if (stop.stop_requested() || (abort && abort())) {
            return false;
        }
    }
}

// ---- Queue ------------------------------------------------------------------------------------------------------

ProxyQueue::ProxyQueue(std::shared_ptr<ResourceGovernor> governor, Transcoder transcoder)
    : m_governor(governor ? std::move(governor) : std::make_shared<ResourceGovernor>()),
      m_transcoder(transcoder ? std::move(transcoder)
                              : Transcoder([](const ProxyJob& job, const exporter::ProxyControl& control) {
                                    return exporter::transcodeProxy(job.source, job.output, job.settings, control);
                                })) {
    m_thread = std::jthread([this](std::stop_token stop) { run(stop); });
}

ProxyQueue::~ProxyQueue() {
    {
        std::lock_guard lock(m_mutex);
        m_queued.clear();
        m_runningStop.request_stop();
    }
    m_thread.request_stop();
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void ProxyQueue::notify() {
    m_cv.notify_all();
    std::function<void()> listener;
    {
        std::lock_guard lock(m_mutex);
        listener = m_listener;
    }
    if (listener) {
        listener();
    }
}

void ProxyQueue::setListener(std::function<void()> listener) {
    std::lock_guard lock(m_mutex);
    m_listener = std::move(listener);
}

void ProxyQueue::enqueue(ProxyJob job) {
    {
        std::lock_guard lock(m_mutex);
        if (m_running && m_running->job.media == job.media) {
            return; // already being made
        }
        const auto it = std::ranges::find_if(m_queued, [&](const Entry& e) { return e.job.media == job.media; });
        if (it != m_queued.end()) {
            it->job = std::move(job); // keeps its place in line
        } else {
            m_queued.push_back({std::move(job), ++m_sequence});
        }
        if (preemptibleLocked()) {
            for (const Entry& e : m_queued) {
                m_preempt = m_preempt || e.job.priority < m_running->job.priority;
            }
        }
    }
    notify();
}

bool ProxyQueue::preemptibleLocked() const {
    return m_running && m_progress < (m_running->job.longSource ? kLongJobPreemptProgress : 0.75);
}

void ProxyQueue::setPriority(MediaId media, ProxyPriority priority) {
    {
        std::lock_guard lock(m_mutex);
        const auto it = std::ranges::find_if(m_queued, [&](const Entry& e) { return e.job.media == media; });
        if (it == m_queued.end() || it->job.priority == priority) {
            if (m_running && m_running->job.media == media) {
                m_running->job.priority = priority;
            }
            return;
        }
        it->job.priority = priority;
        if (preemptibleLocked() && priority < m_running->job.priority) {
            m_preempt = true;
        }
    }
    notify();
}

void ProxyQueue::cancel(MediaId media) {
    {
        std::lock_guard lock(m_mutex);
        std::erase_if(m_queued, [&](const Entry& e) { return e.job.media == media; });
        if (m_running && m_running->job.media == media) {
            m_runningStop.request_stop();
        }
    }
    notify();
}

void ProxyQueue::cancelAll() {
    {
        std::lock_guard lock(m_mutex);
        m_queued.clear();
        m_runningStop.request_stop();
    }
    notify();
}

void ProxyQueue::setPaused(bool paused) {
    {
        std::lock_guard lock(m_mutex);
        m_paused = paused;
    }
    notify();
}

bool ProxyQueue::paused() const {
    std::lock_guard lock(m_mutex);
    return m_paused;
}

bool ProxyQueue::contains(MediaId media) const {
    std::lock_guard lock(m_mutex);
    return (m_running && m_running->job.media == media) ||
           std::ranges::any_of(m_queued, [&](const Entry& e) { return e.job.media == media; });
}

std::vector<ProxyJobStatus> ProxyQueue::statuses() const {
    std::lock_guard lock(m_mutex);
    std::vector<ProxyJobStatus> out;
    if (m_running) {
        out.push_back({m_running->job.media, m_waiting ? ProxyState::Paused : ProxyState::Generating, m_progress,
                       m_running->job.priority, m_running->job.reason});
    }
    std::vector<const Entry*> queued;
    for (const Entry& e : m_queued) {
        queued.push_back(&e);
    }
    std::ranges::sort(queued, [](const Entry* a, const Entry* b) {
        return std::tuple{a->job.priority, a->job.longSource, a->sequence} <
               std::tuple{b->job.priority, b->job.longSource, b->sequence};
    });
    for (const Entry* e : queued) {
        out.push_back({e->job.media, ProxyState::Queued, 0.0, e->job.priority, e->job.reason});
    }
    return out;
}

std::vector<ProxyOutcome> ProxyQueue::takeFinished() {
    std::lock_guard lock(m_mutex);
    std::vector<ProxyOutcome> out;
    out.swap(m_finished);
    return out;
}

std::vector<MediaId> ProxyQueue::startOrder() const {
    std::lock_guard lock(m_mutex);
    return m_started;
}

bool ProxyQueue::waitIdle(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(m_mutex);
    return m_cv.wait_for(lock, timeout, [&] { return m_queued.empty() && !m_running; });
}

std::optional<ProxyQueue::Entry> ProxyQueue::takeNextLocked() {
    if (m_queued.empty()) {
        return std::nullopt;
    }
    // Most urgent first; within a rank shorter media before long recordings (a long one would hold the only worker
    // for half an hour), then first come first served.
    const auto it = std::ranges::min_element(m_queued, [](const Entry& a, const Entry& b) {
        return std::tuple{a.job.priority, a.job.longSource, a.sequence} <
               std::tuple{b.job.priority, b.job.longSource, b.sequence};
    });
    Entry e = std::move(*it);
    m_queued.erase(it);
    return e;
}

void ProxyQueue::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::stop_token jobStop;
        ProxyJob job;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, stop, [&] { return !m_queued.empty() && !m_paused; });
            if (stop.stop_requested()) {
                break;
            }
            m_running = takeNextLocked();
            if (!m_running) {
                continue;
            }
            m_runningStop = std::stop_source{};
            jobStop = m_runningStop.get_token();
            m_preempt = false;
            m_progress = 0.0;
            m_waiting = false;
            m_started.push_back(m_running->job.media);
            job = m_running->job;
        }
        notify();

        auto lastNotify = std::chrono::steady_clock::now();
        exporter::ProxyControl control;
        control.stop = jobStop;
        control.onProgress = [&](std::int64_t done, std::int64_t total) {
            {
                std::lock_guard lock(m_mutex);
                m_progress = total > 0 ? std::clamp(static_cast<double>(done) / static_cast<double>(total), 0.0, 1.0) : 0.0;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - lastNotify > 100ms) {
                lastNotify = now;
                notify();
            }
        };
        // Between frames: give way to the foreground (governor), a queue pause, a more urgent job or a cancel.
        control.yield = [&]() -> bool {
            const auto shouldAbort = [&] {
                std::lock_guard lock(m_mutex);
                return m_preempt || jobStop.stop_requested() || stop.stop_requested();
            };
            bool announced = false;
            for (;;) {
                bool hold = false;
                {
                    std::lock_guard lock(m_mutex);
                    if (m_preempt || jobStop.stop_requested() || stop.stop_requested()) {
                        m_waiting = false;
                        return false;
                    }
                    hold = m_paused || m_governor->busy();
                    m_waiting = hold;
                }
                if (!hold) {
                    if (announced) {
                        notify();
                    }
                    return true;
                }
                if (!announced) {
                    announced = true;
                    notify();
                }
                if (m_governor->busy()) {
                    m_governor->waitIdle(stop, shouldAbort);
                } else {
                    std::unique_lock lock(m_mutex);
                    m_cv.wait_for(lock, stop, 50ms, [&] { return !m_paused || m_preempt || jobStop.stop_requested(); });
                }
            }
        };

        auto result = m_transcoder(job, control);
        {
            std::lock_guard lock(m_mutex);
            const bool preempted = m_preempt && !jobStop.stop_requested() && !stop.stop_requested() && !result &&
                                   result.error().code == ErrorCode::Cancelled;
            if (preempted && m_running) {
                m_queued.push_back(std::move(*m_running)); // keeps its sequence: first again at its priority
            } else {
                m_finished.push_back({std::move(job), std::move(result)});
            }
            m_running.reset();
            m_preempt = false;
            m_waiting = false;
            m_progress = 0.0;
        }
        notify();
    }
}

// ---- Manager ------------------------------------------------------------------------------------------------

ProxyManager::ProxyManager(ProjectDocument& document, ProxyManagerOptions options)
    : m_document(document), m_options(std::move(options)) {
    if (!m_options.governor) {
        m_options.governor = std::make_shared<ResourceGovernor>();
    }
    m_queue = std::make_unique<ProxyQueue>(m_options.governor, m_options.transcoder);
    m_queue->setListener([this] { m_dirty.store(true); });
    m_document.setSaveFilter([this](ProjectState& state) { applyTo(state); });
}

ProxyManager::~ProxyManager() {
    m_document.setSaveFilter({});
    m_queue.reset(); // joins the worker before the registry goes
    m_probeThreads.clear();
}

void ProxyManager::setPolicy(ProxyPolicy policy) {
    if (policy == m_options.policy) {
        return;
    }
    m_options.policy = policy;
    m_declined.clear();
    if (policy == ProxyPolicy::Off) {
        // Automatic work stops; what the user asked for keeps going.
        for (const ProxyJobStatus& s : m_queue->statuses()) {
            if (const auto r = m_reasons.find(s.media.value); r != m_reasons.end() && r->second != "requested") {
                m_queue->cancel(s.media);
            }
        }
    }
    refresh(true);
}

void ProxyManager::setSettings(const exporter::ProxySettings& settings) {
    m_options.settings = settings;
}

std::filesystem::path ProxyManager::proxyFolder() const {
    const std::filesystem::path& file = m_document.file();
    if (!file.empty()) {
        std::filesystem::path name = file.stem();
        name += " Proxies";
        return file.parent_path() / name;
    }
    return untitledProxyFolder(m_options);
}

std::vector<std::filesystem::path> ProxyManager::managedFolders() const {
    return {proxyFolder(), untitledProxyFolder(m_options)};
}

bool ProxyManager::isOriginal(const std::filesystem::path& file, const ProjectState& state) {
    for (const MediaPtr& m : state.media) {
        if (m && !m->isGenerator() && !m->locator.path.empty() && paths::sameFile(file, pathFromUtf8(m->locator.path))) {
            return true;
        }
    }
    return false;
}

struct ProxyManager::RefreshCache {
    explicit RefreshCache(const ProjectState& state) {
        for (const MediaPtr& m : state.media) {
            if (m && !m->isGenerator() && !m->locator.path.empty()) {
                originals.add(pathFromUtf8(m->locator.path));
            }
        }
    }
    const std::vector<IndexEntry>& index(const std::filesystem::path& folder) {
        auto it = indexes.find(folder);
        if (it == indexes.end()) {
            it = indexes.emplace(folder, readIndex(folder)).first;
        }
        return it->second;
    }
    paths::FileSet originals;
    std::map<std::filesystem::path, std::vector<IndexEntry>> indexes;
};

bool ProxyManager::originalOf(const std::filesystem::path& file, const ProjectState& state) const {
    return m_refreshCache != nullptr ? m_refreshCache->originals.contains(file) : isOriginal(file, state);
}

bool ProxyManager::owns(const std::filesystem::path& file, const ProjectState& state) const {
    if (!isFile(file) || paths::isLink(file) || originalOf(file, state)) {
        return false;
    }
    const std::string name = paths::foldName(pathToUtf8(file.filename()));
    for (const std::filesystem::path& folder : managedFolders()) {
        if (!paths::isDirectChild(file, folder)) {
            continue;
        }
        const std::vector<IndexEntry> read = m_refreshCache != nullptr ? std::vector<IndexEntry>{} : readIndex(folder);
        for (const IndexEntry& e : m_refreshCache != nullptr ? m_refreshCache->index(folder) : read) {
            if (e.provenance.generator == exporter::kProxyGenerator && paths::isPlainName(e.file) &&
                paths::foldName(e.file) == name) {
                return true;
            }
        }
    }
    return false;
}

ProxyPriority ProxyManager::priorityOf(MediaId media) const {
    if (m_underPlayhead.contains(media.value)) {
        return ProxyPriority::UnderPlayhead;
    }
    if (m_onTimeline.contains(media.value)) {
        return ProxyPriority::OnTimeline;
    }
    if (m_selected.contains(media.value)) {
        return ProxyPriority::Selected;
    }
    return ProxyPriority::Background;
}

std::uint64_t ProxyManager::nextGeneration(MediaId media) const {
    const auto it = m_registry.find(media.value);
    return it == m_registry.end() ? 1 : it->second.proxy.provenance.generation + 1;
}

bool ProxyManager::queueFor(const MediaItem& item, ProxyPriority priority, std::string reason) {
    if (const std::filesystem::path folder = proxyFolder(); paths::isLink(folder)) {
        m_failed[item.id.value] = "The proxy folder " + pathToUtf8(folder) + " is a link: no proxy is written through it";
        return false;
    }
    ProxyJob job;
    job.media = item.id;
    job.source = item.locator.path;
    job.settings = m_options.settings;
    job.priority = priority;
    job.reason = reason;
    job.fingerprint = item.locator.fingerprint;
    job.generation = nextGeneration(item.id);
    const Ticks streamLength = item.info.video.empty() ? Ticks{} : item.info.video.front().duration;
    job.longSource = std::max(item.info.duration, streamLength) >= kLongSourceForProxy;
    const std::string identity = item.locator.fingerprint.isKnown()
                                     ? item.locator.fingerprint.sampleHash + ":" + std::to_string(item.locator.fingerprint.size)
                                     : item.locator.path;
    // 0.2: named after the file that stands for the item (an image sequence's first frame, a spanned clip's first
    // part): a sequence or spanned path holds characters no file name may.
    std::filesystem::path name = mediaFileToCheck(pathFromUtf8(item.locator.path)).stem();
    name += std::format(".{}.g{}.mov", hex8(identity), job.generation);
    job.output = proxyFolder() / name;
    m_reasons[item.id.value] = std::move(reason);
    m_failed.erase(item.id.value);
    m_queue->enqueue(std::move(job));
    return true;
}

void ProxyManager::adoptFromIndex(const MediaItem& item, const ProjectState& state) {
    if (!item.locator.fingerprint.isKnown()) {
        return;
    }
    const std::filesystem::path folder = proxyFolder();
    const IndexEntry* best = nullptr;
    const std::vector<IndexEntry> entries = m_refreshCache != nullptr ? m_refreshCache->index(folder) : readIndex(folder);
    for (const IndexEntry& e : entries) {
        // The index is untrusted too: a bare file name inside the folder, a file Wabcut owns, never the original.
        if (e.provenance.source.sameContentAs(item.locator.fingerprint) && paths::isPlainName(e.file) &&
            (!best || e.provenance.generation > best->provenance.generation) &&
            owns(folder / pathFromUtf8(e.file), state)) {
            best = &e;
        }
    }
    if (best) {
        m_registry[item.id.value] = {ProxyMedia{pathToUtf8(folder / pathFromUtf8(best->file)), best->provenance}, false, true};
    }
}

void ProxyManager::recordInIndex(const std::filesystem::path& file, const ProxyMedia& proxy, const std::string& sourcePath) {
    const std::filesystem::path folder = file.parent_path();
    std::vector<IndexEntry> entries = readIndex(folder);
    const std::string name = pathToUtf8(file.filename());
    std::erase_if(entries, [&](const IndexEntry& e) { return e.file == name || !isFile(folder / pathFromUtf8(e.file)); });
    entries.push_back({name, sourcePath, proxy.provenance});
    writeIndex(folder, entries);
}

void ProxyManager::refresh(bool force) {
    const ProjectSnapshot snap = m_document.session().snapshot();
    if (!snap) {
        return;
    }
    std::string signature = pathToUtf8(m_document.file()) + "|" + std::string(policyName(m_options.policy));
    for (const MediaPtr& m : snap->media) {
        if (m) {
            signature += std::format("|{}:{}:{}:{}:{}", m->id.value, m->locator.path, m->locator.offline ? 1 : 0,
                                     m->locator.fingerprint.sampleHash, m->locator.proxy ? m->locator.proxy->path : "");
        }
    }
    if (!force && signature == m_signature) {
        return;
    }
    m_signature = std::move(signature);

    // The originals and the indexes, read once for every item below (P.1: once per item before, and the originals'
    // check compared every item with every other, O(media^2) file opens).
    RefreshCache cache(*snap);
    m_refreshCache = &cache;
    struct Release {
        RefreshCache*& slot;
        ~Release() { slot = nullptr; }
    } release{m_refreshCache};

    std::set<std::uint64_t> present;
    std::vector<std::pair<MediaId, std::string>> probes;
    for (const MediaPtr& m : snap->media) {
        if (!m || !hasProxyableVideo(*m)) {
            continue;
        }
        const std::uint64_t id = m->id.value;
        present.insert(id);
        m_known.insert(id);
        if (auto it = m_registry.find(id); it != m_registry.end() && !isFile(pathFromUtf8(it->second.proxy.path))) {
            m_registry.erase(it); // the proxy file went away
        }
        if (!m_registry.contains(id)) {
            const std::filesystem::path saved = m->locator.proxy ? pathFromUtf8(m->locator.proxy->path) : std::filesystem::path{};
            if (isFile(saved) && !originalOf(saved, *snap)) { // a proxy that is the original is refused outright
                m_registry[id] = {*m->locator.proxy, false, owns(saved, *snap)};
            } else {
                adoptFromIndex(*m, *snap);
            }
        }
        if (auto it = m_registry.find(id); it != m_registry.end()) {
            const SourceFingerprint& made = it->second.proxy.provenance.source;
            it->second.stale = m->locator.fingerprint.isKnown() && made.isKnown() && !made.sameContentAs(m->locator.fingerprint);
            if (it->second.stale && m_options.policy != ProxyPolicy::Off && !m->locator.offline &&
                !m_queue->contains(m->id) && !m_failed.contains(id)) {
                queueFor(*m, priorityOf(m->id), "the original changed: regenerating");
            }
            continue;
        }
        if (m->locator.offline || m_queue->contains(m->id) || m_failed.contains(id) || m_userDeleted.contains(id)) {
            continue;
        }
        const VideoStreamInfo& video = m->info.video.front();
        if (m_options.policy == ProxyPolicy::Always) {
            if (video.alpha == AlphaMode::None) {
                queueFor(*m, priorityOf(m->id), "policy: always");
            }
        } else if (m_options.policy == ProxyPolicy::Auto && !m_declined.contains(id) && !m_probing.contains(id)) {
            const auto late = m_late.find(id);
            const std::uint64_t lateFrames = late == m_late.end() ? 0 : late->second;
            if (m_options.probeHardware && needsCapabilityProbe(video) && lateFrames < kLateFramesForProxy) {
                probes.emplace_back(m->id, m->locator.path);
                m_probing.insert(id);
            } else if (std::string reason = proxyReason(video, {}, lateFrames); !reason.empty()) {
                queueFor(*m, priorityOf(m->id), std::move(reason));
            } else {
                m_declined.insert(id);
            }
        }
    }
    std::erase_if(m_registry, [&](const auto& kv) { return !present.contains(kv.first); });
    if (!probes.empty()) {
        startProbes(std::move(probes));
    }
    rebuildTable();
    changed();
}

void ProxyManager::startProbes(std::vector<std::pair<MediaId, std::string>> items) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::jthread thread([this, items = std::move(items), done](std::stop_token stop) {
        const int cores = static_cast<int>(std::thread::hardware_concurrency());
        for (const auto& [media, path] : items) {
            if (stop.stop_requested()) {
                break;
            }
            DecodeCapability cap;
            cap.cpuCores = cores;
            media::DecoderOptions options;
            options.allowHardware = true;
            options.threadCount = 1;
            if (auto decoder = media::VideoDecoder::open(path, -1, options)) {
                cap.known = true;
                cap.hardwareDecodes = (*decoder)->isHardwareAccelerated();
            }
            std::lock_guard lock(m_probeMutex);
            m_probed.emplace_back(media, cap);
        }
        done->store(true);
        m_dirty.store(true);
    });
    m_probeThreads.push_back({std::move(done), std::move(thread)});
}

void ProxyManager::setFocus(const std::vector<MediaId>& underPlayhead, const std::vector<MediaId>& onTimeline,
                            const std::vector<MediaId>& selected) {
    const auto toSet = [](const std::vector<MediaId>& ids) {
        std::set<std::uint64_t> s;
        for (const MediaId id : ids) {
            s.insert(id.value);
        }
        return s;
    };
    m_underPlayhead = toSet(underPlayhead);
    m_onTimeline = toSet(onTimeline);
    m_selected = toSet(selected);
    for (const ProxyJobStatus& s : m_queue->statuses()) {
        if (const ProxyPriority p = priorityOf(s.media); p != s.priority) {
            m_queue->setPriority(s.media, p);
        }
    }
}

void ProxyManager::setLateFrames(const std::vector<std::pair<std::uint64_t, std::uint64_t>>& late) {
    bool any = false;
    for (const auto& [media, frames] : late) {
        std::uint64_t& known = m_late[media];
        if (frames != known) {
            known = frames;
            any = any || frames >= kLateFramesForProxy;
        }
    }
    if (any && m_options.policy == ProxyPolicy::Auto) {
        for (const auto& [media, frames] : late) {
            if (frames >= kLateFramesForProxy) {
                m_declined.erase(media); // look again with the new evidence
            }
        }
        refresh(true);
    }
}

int ProxyManager::generate(const std::vector<MediaId>& media) {
    const ProjectSnapshot snap = m_document.session().snapshot();
    int queued = 0;
    for (const MediaId id : media) {
        const MediaItem* item = snap ? snap->findMedia(id) : nullptr;
        if (!item || !hasProxyableVideo(*item) || m_queue->contains(id)) {
            continue;
        }
        m_userDeleted.erase(id.value);
        if (item->locator.offline) {
            m_failed[id.value] = "The original is offline: relink it first";
            continue;
        }
        if (item->info.video.front().alpha != AlphaMode::None) {
            m_failed[id.value] = "Media with alpha is not proxied (the proxy codecs have no alpha)";
            continue;
        }
        if (const auto it = m_registry.find(id.value); it != m_registry.end() && !it->second.stale &&
                                                         it->second.proxy.provenance.settings ==
                                                             exporter::describeProxySettings(m_options.settings)) {
            continue; // already there, made the same way
        }
        if (queueFor(*item, priorityOf(id), "requested")) {
            ++queued;
        }
    }
    changed();
    return queued;
}

int ProxyManager::deleteProxies(const std::vector<MediaId>& media) {
    int deleted = 0;
    const ProjectSnapshot snap = m_document.session().snapshot();
    std::map<std::filesystem::path, std::set<std::string>> unindex; // folder -> file names
    for (const MediaId id : media) {
        m_queue->cancel(id);
        m_userDeleted.insert(id.value);
        m_failed.erase(id.value);
        m_reasons.erase(id.value);
        if (const auto it = m_registry.find(id.value); it != m_registry.end()) {
            const std::filesystem::path file = pathFromUtf8(it->second.proxy.path);
            // Only a file Wabcut owns is deleted (checked again now); an attached proxy is just detached.
            if (it->second.owned && snap && owns(file, *snap)) {
                std::error_code ec;
                std::filesystem::remove(file, ec);
                unindex[file.parent_path()].insert(pathToUtf8(file.filename()));
            }
            m_registry.erase(it);
            ++deleted;
        }
    }
    for (const auto& [folder, names] : unindex) {
        std::vector<IndexEntry> entries = readIndex(folder);
        std::erase_if(entries, [&](const IndexEntry& e) { return names.contains(e.file); });
        writeIndex(folder, entries);
    }
    rebuildTable();
    changed();
    return deleted;
}

bool ProxyManager::poll() {
    bool any = m_dirty.exchange(false);
    std::vector<std::pair<MediaId, DecodeCapability>> probed;
    {
        std::lock_guard lock(m_probeMutex);
        probed.swap(m_probed);
    }
    const ProjectSnapshot snap = m_document.session().snapshot();
    for (const auto& [id, cap] : probed) {
        m_probing.erase(id.value);
        const MediaItem* item = snap ? snap->findMedia(id) : nullptr;
        if (!item || !hasProxyableVideo(*item) || m_registry.contains(id.value) || m_queue->contains(id) ||
            m_options.policy != ProxyPolicy::Auto || m_userDeleted.contains(id.value)) {
            continue;
        }
        const auto late = m_late.find(id.value);
        if (std::string reason = proxyReason(item->info.video.front(), cap, late == m_late.end() ? 0 : late->second);
            !reason.empty()) {
            queueFor(*item, priorityOf(id), std::move(reason));
        } else {
            m_declined.insert(id.value);
        }
        any = true;
    }
    for (ProxyOutcome& o : m_queue->takeFinished()) {
        any = true;
        const std::uint64_t id = o.job.media.value;
        if (o.result) {
            ProxyMedia proxy;
            proxy.path = pathToUtf8(o.result->file);
            proxy.provenance.generator = std::string(exporter::kProxyGenerator);
            proxy.provenance.settings = exporter::describeProxySettings(o.job.settings);
            proxy.provenance.source = o.job.fingerprint;
            proxy.provenance.generation = o.job.generation;
            if (const auto it = m_registry.find(id); it != m_registry.end() && it->second.proxy.path != proxy.path) {
                // The previous generation, when Wabcut owns it (may still be open in a decoder: then it stays). An
                // attached proxy is left where it is.
                const std::filesystem::path previous = pathFromUtf8(it->second.proxy.path);
                if (it->second.owned && snap && owns(previous, *snap)) {
                    std::error_code ec;
                    std::filesystem::remove(previous, ec);
                }
            }
            recordInIndex(o.result->file, proxy, o.job.source);
            m_registry[id] = {proxy, false, snap && owns(o.result->file, *snap)};
            m_known.insert(id);
            m_failed.erase(id);
        } else if (o.result.error().code != ErrorCode::Cancelled) {
            m_failed[id] = o.result.error().message;
        }
    }
    std::erase_if(m_probeThreads, [](const ProbeThread& t) { return t.done->load(); });
    if (any) {
        rebuildTable();
        changed();
    }
    return any;
}

void ProxyManager::rebuildTable() {
    auto table = std::make_shared<playback::ProxyTable>();
    for (const auto& [id, entry] : m_registry) {
        if (entry.stale) {
            continue;
        }
        exporter::ProxySettings s;
        const int divisor = exporter::parseProxySettings(entry.proxy.provenance.settings, s) ? s.divisor : 2;
        table->entries.emplace(id, playback::ProxyEntry{entry.proxy.path, divisor});
    }
    m_table = std::move(table);
}

void ProxyManager::changed() {
    if (onChanged) {
        onChanged();
    }
}

ProxyStatus ProxyManager::status(MediaId media) const {
    for (const ProxyStatus& s : statuses()) {
        if (s.media == media) {
            return s;
        }
    }
    return {media};
}

std::vector<ProxyStatus> ProxyManager::statuses() const {
    std::vector<ProxyStatus> out;
    const ProjectSnapshot snap = m_document.session().snapshot();
    if (!snap) {
        return out;
    }
    const std::vector<ProxyJobStatus> jobs = m_queue->statuses();
    for (const MediaPtr& m : snap->media) {
        if (!m || !hasProxyableVideo(*m)) {
            continue;
        }
        ProxyStatus s;
        s.media = m->id;
        s.originalOffline = m->locator.offline;
        const auto job = std::ranges::find_if(jobs, [&](const ProxyJobStatus& j) { return j.media == m->id; });
        const auto entry = m_registry.find(m->id.value);
        if (entry != m_registry.end()) {
            s.file = pathFromUtf8(entry->second.proxy.path);
        }
        if (job != jobs.end()) {
            s.state = job->state;
            s.progress = job->progress;
            s.detail = job->reason;
        } else if (entry != m_registry.end()) {
            s.state = entry->second.stale ? ProxyState::Stale : ProxyState::Ready;
            s.progress = 1.0;
            s.detail = entry->second.stale ? std::string("the original changed since this proxy was made")
                                           : prettySettings(entry->second.proxy.provenance.settings);
        } else if (const auto f = m_failed.find(m->id.value); f != m_failed.end()) {
            s.state = ProxyState::Failed;
            s.detail = f->second;
        }
        out.push_back(std::move(s));
    }
    return out;
}

ProxyCounts ProxyManager::counts() const {
    ProxyCounts c;
    for (const ProxyStatus& s : statuses()) {
        switch (s.state) {
        case ProxyState::Ready:
            ++c.ready;
            break;
        case ProxyState::Queued:
            ++c.queued;
            break;
        case ProxyState::Generating:
        case ProxyState::Paused:
            ++c.generating;
            break;
        case ProxyState::Failed:
            ++c.failed;
            break;
        case ProxyState::Stale:
            ++c.stale;
            break;
        case ProxyState::None:
            continue;
        }
        ++c.wanted;
    }
    return c;
}

std::optional<std::filesystem::path> ProxyManager::proxyFile(MediaId media) const {
    const auto it = m_registry.find(media.value);
    if (it == m_registry.end()) {
        return std::nullopt;
    }
    return pathFromUtf8(it->second.proxy.path);
}

std::set<std::filesystem::path> ProxyManager::filesInUse() const {
    std::set<std::filesystem::path> out;
    for (const auto& [id, entry] : m_registry) {
        out.insert(pathFromUtf8(entry.proxy.path).lexically_normal());
    }
    // The originals too: one that sits in a cache folder is never cleaned away with the proxies.
    if (const ProjectSnapshot snap = m_document.session().snapshot()) {
        for (const MediaPtr& m : snap->media) {
            if (m && !m->isGenerator() && !m->locator.path.empty()) {
                out.insert(pathFromUtf8(m->locator.path).lexically_normal());
            }
        }
    }
    return out;
}

void ProxyManager::applyTo(ProjectState& state) const {
    for (MediaPtr& m : state.media) {
        if (!m || !m_known.contains(m->id.value) || m->isGenerator()) {
            continue;
        }
        const auto it = m_registry.find(m->id.value);
        std::optional<ProxyMedia> proxy;
        if (it != m_registry.end() && !it->second.stale) {
            proxy = it->second.proxy;
        }
        if (m->locator.proxy != proxy) {
            auto item = std::make_shared<MediaItem>(*m);
            item->locator.proxy = std::move(proxy);
            m = std::move(item);
        }
    }
}

} // namespace wabcut::editor
