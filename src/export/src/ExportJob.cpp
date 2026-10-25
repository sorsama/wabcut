#include "Encode.h"
#include "ExportLoudness.h"
#include "FrameEffects.h"
#include "MetadataWriter.h"
#include "Mixdown.h"
#include "SmartRender.h"
#include "SubtitleStream.h"

#include <wabcut/export/CaptionSidecar.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/interchange/CaptionFiles.h>
#include <wabcut/export/RenderedVideoSource.h>
#include <wabcut/model/Resolve.h>

#include <wabcut/audio/effects/EffectChain.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <exception>
#include <format>
#include <map>
#include <random>
#include <set>
#include <span>
#include <system_error>
#include <tuple>

namespace wabcut::exporter {

namespace {

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

/// Deletes a file when it goes out of scope, unless kept (the temporary output of a failed export).
struct TempFile {
    std::filesystem::path path;
    bool keep = false;
    ~TempFile() {
        if (!keep && !path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
};

/// 0.2: the folder an image sequence is written into before its files move into place; removed with what is left.
struct TempDir {
    std::filesystem::path path;
    ~TempDir() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }
};

/// 0.2, Container::Image: the name of still `index` of `count` next to `output`: "<name><index>.<ext>" with at least
/// three digits (Premiere's Export As Sequence), or the output itself for a single still.
std::filesystem::path stillPath(const std::filesystem::path& output, std::int64_t index, std::int64_t count, bool sequence) {
    if (!sequence) {
        return output;
    }
    const int digits = std::max<int>(3, static_cast<int>(std::to_string(std::max<std::int64_t>(count - 1, 0)).size()));
    std::filesystem::path name = output.stem();
    name += std::format("{:0{}}", index, digits);
    name += output.extension();
    return output.parent_path() / name;
}

/// Stops a video source on every exit path (outstanding frames are abandoned).
/// Cancelling an export stops its video source at once: a stage blocked in IVideoFrameSource::next() (a slow
/// render, a stalled decoder) then returns instead of holding the job (stop() is safe from another thread).
struct StopSourceOnCancel {
    IVideoFrameSource* source = nullptr;
    void operator()() const noexcept { source->stop(); }
};

struct SourceStopper {
    IVideoFrameSource* source = nullptr;
    ~SourceStopper() {
        if (source) {
            source->stop();
        }
    }
};

/// The first video stage: on its own thread, requests frames from the source (maxInFlight ahead), converts
/// them (swscale) and queues at most kDepth converted frames, so conversion overlaps encoding on the job
/// thread. Destruction or stop() stops the source and joins.
class ConvertStage {
public:
    static constexpr std::size_t kDepth = 2;

    /// `frames`: the output frames to render, in order (0.2: smart rendering copies the others); nullopt = all of them.
    ConvertStage(IVideoFrameSource& source, detail::FrameConverter& converter, std::int64_t frameCount,
                 std::optional<std::vector<std::int64_t>> frames = std::nullopt)
        : m_source(source), m_converter(converter), m_frames(frames.value_or(std::vector<std::int64_t>{})),
          m_frameCount(frames ? static_cast<std::int64_t>(m_frames.size()) : frameCount), m_all(!frames),
          m_thread([this](std::stop_token stop) { run(stop); }) {}

    ~ConvertStage() { stop(); }
    ConvertStage(const ConvertStage&) = delete;
    ConvertStage& operator=(const ConvertStage&) = delete;

    /// The next converted frame in order (blocks until it is ready).
    std::expected<detail::FramePtr, Error> next() {
        std::unique_lock lock(m_mutex);
        m_changed.wait(lock, [&] { return !m_queue.empty(); });
        auto item = std::move(m_queue.front());
        m_queue.pop_front();
        m_changed.notify_all();
        return item;
    }

    void stop() {
        if (!m_thread.joinable()) {
            return;
        }
        m_thread.request_stop();
        m_changed.notify_all();
        m_source.stop(); // unblocks a next() the stage thread may be waiting in
        m_thread.join();
    }

private:
    void push(std::expected<detail::FramePtr, Error> item) {
        std::lock_guard lock(m_mutex);
        m_queue.push_back(std::move(item));
        m_changed.notify_all();
    }

    void run(const std::stop_token& stop) {
        std::int64_t requested = 0;
        for (std::int64_t k = 0; k < m_frameCount; ++k) {
            {
                std::unique_lock lock(m_mutex);
                if (!m_changed.wait(lock, stop, [&] { return m_queue.size() < kDepth; })) {
                    return;
                }
            }
            while (requested < m_frameCount && requested < k + m_source.maxInFlight()) {
                if (auto ok = m_source.request(frameAt(requested)); !ok) {
                    push(std::unexpected(ok.error()));
                    return;
                }
                ++requested;
            }
            auto frame = m_source.next();
            if (!frame) {
                push(std::unexpected(frame.error()));
                return;
            }
            auto converted = m_converter.convert(*frame, frameAt(k));
            const bool failed = !converted.has_value();
            push(std::move(converted));
            if (failed) {
                return;
            }
        }
    }

    std::int64_t frameAt(std::int64_t i) const { return m_all ? i : m_frames[static_cast<std::size_t>(i)]; }

    IVideoFrameSource& m_source;
    detail::FrameConverter& m_converter;
    std::vector<std::int64_t> m_frames;
    std::int64_t m_frameCount;
    bool m_all = true;
    std::mutex m_mutex;
    std::condition_variable_any m_changed;
    std::deque<std::expected<detail::FramePtr, Error>> m_queue;
    std::jthread m_thread; // last: started after the state above exists
};

/// The audio of an export with video, mixed and encoded on its own thread so it overlaps video encoding. It
/// is kept within `lead` samples of the video in both directions (the muxer buffers whichever stream is ahead
/// to interleave them, so that bounds its memory). Muxer writes are serialized by the Muxer.
class AudioStage {
public:
    using Pump = std::function<std::expected<void, Error>(std::int64_t upTo)>;
    using Flush = std::function<std::expected<void, Error>()>;

    AudioStage(Pump pump, Flush flush, std::int64_t total, std::int64_t lead)
        : m_pump(std::move(pump)), m_flush(std::move(flush)), m_total(total), m_lead(lead),
          m_thread([this](std::stop_token stop) { run(stop); }) {}

    ~AudioStage() { stop(); }
    AudioStage(const AudioStage&) = delete;
    AudioStage& operator=(const AudioStage&) = delete;

    /// The video has been encoded up to `samples` (in audio samples). Blocks while the audio is more than
    /// `lead` behind it; returns false once the audio failed.
    bool videoReached(std::int64_t samples) {
        std::unique_lock lock(m_mutex);
        m_video = samples;
        m_changed.notify_all();
        m_changed.wait(lock, [&] { return m_finished || m_audio + m_lead >= samples; });
        return !m_error;
    }

    /// Waits until the audio is completely encoded and flushed (call after videoReached(total)).
    std::expected<void, Error> finish() {
        if (m_thread.joinable()) {
            m_thread.join();
        }
        std::lock_guard lock(m_mutex);
        if (m_error) {
            return std::unexpected(*m_error);
        }
        return {};
    }

    void stop() {
        if (m_thread.joinable()) {
            m_thread.request_stop();
            m_changed.notify_all();
            m_thread.join();
        }
    }

private:
    void run(const std::stop_token& stop) {
        std::int64_t done = 0;
        while (done < m_total) {
            std::int64_t target = 0;
            {
                std::unique_lock lock(m_mutex);
                if (!m_changed.wait(lock, stop, [&] { return m_video + m_lead > done; })) {
                    finishWith(lock, std::nullopt);
                    return; // cancelled: the job reports it
                }
                target = std::min(m_total, m_video + m_lead);
            }
            if (auto ok = m_pump(target); !ok) {
                std::unique_lock lock(m_mutex);
                finishWith(lock, ok.error());
                return;
            }
            done = target;
            std::lock_guard lock(m_mutex);
            m_audio = done;
            m_changed.notify_all();
        }
        auto flushed = m_flush();
        std::unique_lock lock(m_mutex);
        finishWith(lock, flushed ? std::nullopt : std::optional<Error>(flushed.error()));
    }

    void finishWith(std::unique_lock<std::mutex>&, std::optional<Error> error) {
        m_error = std::move(error);
        m_finished = true;
        m_changed.notify_all();
    }

    Pump m_pump;
    Flush m_flush;
    const std::int64_t m_total;
    const std::int64_t m_lead;
    std::mutex m_mutex;
    std::condition_variable_any m_changed;
    std::int64_t m_video = 0; // samples' worth of video encoded
    std::int64_t m_audio = 0; // samples encoded
    bool m_finished = false;
    std::optional<Error> m_error;
    std::jthread m_thread; // last: started after the state above exists
};

std::filesystem::path tempPathFor(const std::filesystem::path& output) {
    std::random_device rd;
    const std::uint64_t tag = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    std::filesystem::path name = output.stem();
    name += std::format(".wabcut-partial-{:016x}", tag);
    name += output.extension();
    return output.parent_path() / name;
}

/// What a walk of used media has seen: the media of (sequence, kind, tracks read, nested range), computed once
/// (review wave 5 E5-P2-9: a nested sequence played by many clips was walked again for each).
struct UsedMediaWalk {
    const ProjectState& project;
    std::map<std::tuple<std::uint64_t, bool, std::uint64_t, std::int64_t, std::int64_t>, std::set<MediaId>> memo;
};

/// The media of `tracks` (indices of `seq`'s tracks of one kind) clips overlapping `range` play, nested ones through
/// the tracks each nested or multicam clip reads (a multicam clip: only its active angles, review wave 5 E5-P2-10).
void collectUsedMedia(UsedMediaWalk& walk, const Sequence& seq, bool isVideo, std::span<const std::size_t> tracks,
                      TimeRange range, std::size_t depth, std::set<MediaId>& used) {
    if (depth >= kMaxNestingDepth) {
        return;
    }
    const std::vector<TrackPtr>& all = seq.tracks(isVideo ? TrackKind::Video : TrackKind::Audio);
    for (const std::size_t index : tracks) {
        if (index >= all.size() || !all[index]) {
            continue;
        }
        for (const Clip& clip : all[index]->clips) {
            if (!clip.enabled || !clip.timelineRange.overlaps(range)) {
                continue;
            }
            if (clip.media.isValid()) {
                used.insert(clip.media);
                continue;
            }
            const Sequence* nested = clip.isNested() ? walk.project.findSequence(clip.sequence) : nullptr;
            if (!nested) {
                continue;
            }
            const TimeRange inside = clip.isRemapped() ? TimeRange{Ticks{}, std::max(nested->duration, nested->contentEnd())}
                                                       : TimeRange::fromStartEnd(clip.sourceIn, clip.sourceOut());
            const std::vector<std::size_t> read = nestedTracksRead(*nested, clip);
            std::uint64_t readKey = 0;
            for (const std::size_t i : read) {
                readKey = readKey * 1'000'003u + i + 1;
            }
            const auto key = std::tuple{nested->id.value, isVideo, readKey, inside.start.value, inside.duration.value};
            auto found = walk.memo.find(key);
            if (found == walk.memo.end()) {
                std::set<MediaId> deeper;
                collectUsedMedia(walk, *nested, isVideo, read, inside, depth + 1, deeper);
                found = walk.memo.insert_or_assign(key, std::move(deeper)).first; // map nodes stay put
            }
            used.insert(found->second.begin(), found->second.end());
        }
    }
}

/// The media items an export reads: clips overlapping `range` on the tracks that contribute (enabled video
/// tracks; audible audio tracks; every angle of a multicam source exported itself), enabled clips only - and,
/// through nested and multicam clips (C.8), the media of the tracks they read in the nested sequences, over the nested
/// time they can show (the whole nested sequence for a remapped clip), so an offline file inside a nest stops the export
/// like any other, and one in an angle never cut to does not.
std::set<MediaId> usedMedia(const ProjectState& project, const Sequence& seq, TimeRange range, bool video, bool audio) {
    std::set<MediaId> used;
    UsedMediaWalk walk{project, {}};
    if (video) {
        std::vector<std::size_t> tracks;
        for (std::size_t i = 0; i < seq.videoTracks.size(); ++i) {
            if (seq.videoTracks[i] && (seq.videoTracks[i]->enabled || seq.multicam)) {
                tracks.push_back(i);
            }
        }
        collectUsedMedia(walk, seq, true, tracks, range, 0, used);
    }
    if (audio) {
        const bool anySolo = std::any_of(seq.audioTracks.begin(), seq.audioTracks.end(),
                                         [](const TrackPtr& t) { return t && t->solo; });
        std::vector<std::size_t> tracks;
        for (std::size_t i = 0; i < seq.audioTracks.size(); ++i) {
            const TrackPtr& t = seq.audioTracks[i];
            if (t && ((t->enabled && !t->muted && (!anySolo || t->solo)) || seq.multicam)) {
                tracks.push_back(i);
            }
        }
        collectUsedMedia(walk, seq, false, tracks, range, 0, used);
    }
    return used;
}

/// Why plugin effects the export's audio runs through will not be heard as the project asks (missing on this computer,
/// failed to load): the master's, the audible tracks', the enabled clips' overlapping `range`, and through nested clips
/// the stacks they play.
void collectPluginProblems(const ProjectState& project, const Sequence& seq, TimeRange range, std::size_t depth,
                           std::vector<std::string>& out) {
    constexpr std::size_t kMaxDepth = 64; // as Resolve.h's nesting bound
    if (depth >= kMaxDepth) {
        return;
    }
    const auto check = [&](const EffectStack& stack) {
        for (const EffectInstance& effect : stack) {
            std::string problem = audio::externalEffectProblem(effect);
            if (!problem.empty()) {
                problem += " - the export does not have this effect";
                if (std::find(out.begin(), out.end(), problem) == out.end()) {
                    out.push_back(std::move(problem));
                }
            }
        }
    };
    check(seq.audioEffects);
    const bool anySolo =
        std::any_of(seq.audioTracks.begin(), seq.audioTracks.end(), [](const TrackPtr& t) { return t && t->solo; });
    for (const TrackPtr& t : seq.audioTracks) {
        if (!t || (!seq.multicam && (!t->enabled || t->muted || (anySolo && !t->solo)))) {
            continue;
        }
        check(t->effects);
        for (const Clip& clip : t->clips) {
            if (!clip.enabled || !clip.timelineRange.overlaps(range)) {
                continue;
            }
            check(clip.effects());
            if (const Sequence* nested = clip.isNested() ? project.findSequence(clip.sequence) : nullptr) {
                const TimeRange inside = clip.isRemapped() ? TimeRange{Ticks{}, std::max(nested->duration, nested->contentEnd())}
                                                           : TimeRange::fromStartEnd(clip.sourceIn, clip.sourceOut());
                collectPluginProblems(project, *nested, inside, depth + 1, out);
            }
        }
    }
}

/// 0.2, Premiere's Metadata: chapters from chapter markers (MP4, MOV, MKV, M4A), the start timecode (a 'tmcd' track
/// in MOV / MP4, a Broadcast WAV's time reference), the title and description tags.
std::expected<void, Error> addMetadata(detail::Muxer& muxer, const ExportSettings& s, const Sequence& seq,
                                       const ResolvedExport& r, ExportResult& result) {
    using namespace detail;
    const Container c = s.container;
    if (s.metadata.chapters && (c == Container::Mp4 || c == Container::Mov || c == Container::Mkv || c == Container::M4a)) {
        for (const ChapterMark& m : chapterMarks(seq, r.range)) {
            muxer.addChapter(m.startMs, m.endMs, m.title);
            ++result.chapters;
        }
    }
    if (!s.metadata.title.empty()) {
        muxer.setMetadata("title", s.metadata.title);
    }
    if (!s.metadata.description.empty()) {
        muxer.setMetadata(c == Container::Wav ? "comment" : "description", s.metadata.description);
    }
    if (!s.metadata.timecode) {
        return {};
    }
    const Ticks start = exportStartTimecode(s, seq, r.range);
    if (s.video.enabled && (c == Container::Mov || c == Container::Mp4)) {
        result.startTimecode = timecodeText(start, r.frameRate, seq.dropFrame);
        muxer.setMetadata("timecode", result.startTimecode);
    } else if (c == Container::Wav && s.audio.enabled) {
        // Broadcast WAV: the time reference counts samples since midnight.
        const auto samples = rescale(start, TimeBase{1, s.audio.sampleRate}, Rounding::Nearest);
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        muxer.setBroadcastWave(true);
        muxer.setMetadata("time_reference", std::to_string(samples.value_or(0)));
        muxer.setMetadata("originator", "Wabcut");
        muxer.setMetadata("description", s.metadata.title.empty() ? seq.name : s.metadata.title);
        muxer.setMetadata("origination_date", std::format("{:%Y-%m-%d}", now));
        muxer.setMetadata("origination_time", std::format("{:%H:%M:%S}", now));
        result.startTimecode = timecodeText(start, seq.frameRate, seq.dropFrame);
    }
    return {};
}

struct Originals {
    ProjectState project; ///< locators point at the resolved original files, proxies dropped
    std::vector<std::filesystem::path> files;
};

/// Original media only (never proxies): every used item must be online and found (absolute path, else
/// relative to the project file). Errors: NotFound listing the missing items.
std::expected<Originals, Error> resolveOriginals(const ProjectState& snapshot, const std::set<MediaId>& used,
                                                 const std::filesystem::path& projectFile) {
    Originals out;
    out.project = snapshot;
    std::vector<std::string> missing;
    for (MediaPtr& item : out.project.media) {
        if (!item || !used.contains(item->id) || item->isGenerator()) {
            continue; // generated items (titles) have no file: the renderer draws them
        }
        const auto path = item->locator.offline ? std::nullopt : resolveMediaPath(item->locator, projectFile);
        if (!path) {
            missing.push_back(item->locator.path.empty()
                                  ? item->name
                                  : std::format("{} ({})", item->name, item->locator.path));
            continue;
        }
        auto copy = std::make_shared<MediaItem>(*item);
        copy->locator.path = pathToUtf8(*path);
        copy->locator.proxy.reset();
        out.files.push_back(*path);
        item = std::move(copy);
    }
    for (const MediaId id : used) {
        if (!snapshot.findMedia(id)) {
            missing.push_back(std::format("unknown media #{}", id.value));
        }
    }
    if (!missing.empty()) {
        std::string list;
        for (const std::string& m : missing) {
            list += (list.empty() ? "" : ", ") + m;
        }
        return std::unexpected(Error{ErrorCode::NotFound,
                                     std::format("Original media offline or missing: {}. Relink it to export "
                                                 "(exports never use proxies).",
                                                 list)});
    }
    return out;
}

} // namespace

std::string_view exportPhaseName(ExportPhase phase) {
    switch (phase) {
    case ExportPhase::Queued:
        return "queued";
    case ExportPhase::Preparing:
        return "preparing";
    case ExportPhase::Rendering:
        return "rendering";
    case ExportPhase::Finalizing:
        return "finalizing";
    case ExportPhase::Done:
        return "done";
    case ExportPhase::Failed:
        return "failed";
    case ExportPhase::Cancelled:
        return "cancelled";
    case ExportPhase::Paused:
        return "paused";
    }
    return "unknown";
}

void PauseGate::pause() {
    std::lock_guard lock(m_mutex);
    m_paused = true;
}

void PauseGate::resume() {
    {
        std::lock_guard lock(m_mutex);
        m_paused = false;
    }
    m_changed.notify_all();
}

bool PauseGate::paused() const {
    std::lock_guard lock(m_mutex);
    return m_paused;
}

bool PauseGate::wait(const std::stop_token& stop) {
    std::unique_lock lock(m_mutex);
    return m_changed.wait(lock, stop, [&] { return !m_paused; });
}

VideoSourceFactory defaultVideoSourceFactory() {
    return renderedVideoSourceFactory();
}

VideoSourceFactory syntheticVideoSourceFactory(SyntheticPattern pattern) {
    return [pattern](
               const ProjectState&, const Sequence&,
               const VideoSourceFormat& format) -> std::expected<std::unique_ptr<IVideoFrameSource>, Error> {
        return makeSyntheticVideoSource(format, pattern);
    };
}

// ---- ExportJob --------------------------------------------------------------------------------------------

struct ExportJob::Impl {
    ExportRequest request;
    mutable std::mutex mutex;
    ExportProgress progress;
    Clock::time_point started = Clock::now();
    Clock::time_point lastNotify{};

    /// Applies `update` to the progress and reports it (always when `force`, else at most every 100 ms).
    template <class Fn> void publish(Fn&& update, bool force) {
        ExportProgress copy;
        {
            std::lock_guard lock(mutex);
            update(progress);
            progress.elapsedSeconds = secondsSince(started);
            if (progress.phase == ExportPhase::Rendering && progress.elapsedSeconds > 0.0) {
                progress.fps = static_cast<double>(progress.framesDone) / progress.elapsedSeconds;
                if (progress.fps > 0.0 && progress.framesTotal > 0) {
                    progress.etaSeconds =
                        static_cast<double>(progress.framesTotal - progress.framesDone) / progress.fps;
                }
            }
            copy = progress;
        }
        const auto now = Clock::now();
        if (request.onProgress && (force || now - lastNotify >= std::chrono::milliseconds(100))) {
            lastNotify = now;
            request.onProgress(copy);
        }
    }

    void setPhase(ExportPhase phase) {
        publish([phase](ExportProgress& p) { p.phase = phase; }, true);
    }

    /// 0.2: holds the export here while its pause gate is engaged (phase Paused, then Rendering again). False when
    /// it was cancelled meanwhile.
    bool pausePoint(const std::stop_token& stop) {
        if (!request.pause || !request.pause->paused()) {
            return true;
        }
        setPhase(ExportPhase::Paused);
        const bool resumed = request.pause->wait(stop);
        if (resumed) {
            setPhase(ExportPhase::Rendering);
        }
        return resumed;
    }

    /// One encode; `rateScale` scales a target-size export's video bitrate (a second attempt that must come out smaller).
    std::expected<ExportResult, Error> runImpl(const std::stop_token& stop, double rateScale = 1.0);
};

ExportJob::ExportJob(ExportRequest request) : m_impl(std::make_unique<Impl>()) {
    m_impl->request = std::move(request);
}

ExportJob::~ExportJob() = default;

const ExportRequest& ExportJob::request() const {
    return m_impl->request;
}

ExportProgress ExportJob::progress() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->progress;
}

std::expected<ExportResult, Error> ExportJob::run(std::stop_token stop) {
    m_impl->started = Clock::now();
    std::expected<ExportResult, Error> result;
    const auto body = [&] {
        try {
            result = m_impl->runImpl(stop);
            // I.9: a target-size export that still came out over its size is encoded once more at the rate that fits.
            const VideoSettings& v = m_impl->request.settings.video;
            if (result && v.enabled && v.rateControl == RateControl::TargetSize && result->bytes > v.targetBytes) {
                const double scale = (1.0 - kTargetSizeMargin) * static_cast<double>(v.targetBytes) / static_cast<double>(result->bytes);
                result = m_impl->runImpl(stop, scale);
                if (result) {
                    result->attempts = 2;
                }
            }
        } catch (const std::exception& e) {
            result = std::unexpected(Error{ErrorCode::Unknown, std::format("Export failed: {}", e.what())});
        }
    };
    // 0.2: Media Foundation encoders (mp3_mf) refuse a single-threaded COM apartment, which a Qt GUI thread is: such
    // an export runs on a thread of its own (the queue's worker never is one).
    if (m_impl->request.settings.audio.enabled && m_impl->request.settings.audio.codec == AudioCodec::Mp3) {
        std::thread worker(body);
        worker.join();
    } else {
        body();
    }
    if (result) {
        m_impl->setPhase(ExportPhase::Done);
    } else {
        m_impl->setPhase(result.error().code == ErrorCode::Cancelled ? ExportPhase::Cancelled
                                                                     : ExportPhase::Failed);
    }
    return result;
}

std::expected<ExportResult, Error> ExportJob::Impl::runImpl(const std::stop_token& stop, double rateScale) {
    using namespace detail;
    setPhase(ExportPhase::Preparing);
    const ExportSettings& settings = request.settings;
    const auto cancelled = [] { return std::unexpected(Error{ErrorCode::Cancelled, "Export cancelled"}); };

    // ---- Resolve ------------------------------------------------------------------------------------------
    if (!request.snapshot) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "No project snapshot to export"});
    }
    const SequencePtr sequence = request.snapshot->sequencePtr(request.sequence);
    if (!sequence) {
        return std::unexpected(Error{ErrorCode::NotFound, "The sequence to export does not exist"});
    }
    auto resolvedOr = resolve(settings, *sequence, request.snapshot->colorManagement);
    if (!resolvedOr) {
        return std::unexpected(resolvedOr.error());
    }
    const ResolvedExport r = *resolvedOr;
    const bool hasVideo = settings.video.enabled;
    const bool hasAudio = settings.audio.enabled;

    if (request.output.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "No output file"});
    }
    const std::filesystem::path output = std::filesystem::absolute(request.output);
    std::error_code ec;
    if (!std::filesystem::is_directory(output.parent_path(), ec)) {
        return std::unexpected(Error{ErrorCode::IoError, std::format("The output folder does not exist: {}",
                                                                     pathToUtf8(output.parent_path()))});
    }
    auto originals = resolveOriginals(*request.snapshot, usedMedia(*request.snapshot, *sequence, r.range, hasVideo, hasAudio),
                                      request.projectFile);
    if (!originals) {
        return std::unexpected(originals.error());
    }
    for (const std::filesystem::path& source : originals->files) {
        if (std::filesystem::exists(output, ec) && std::filesystem::equivalent(output, source, ec)) {
            return std::unexpected(
                Error{ErrorCode::InvalidArgument,
                      std::format("The output would overwrite source media {}", pathToUtf8(source))});
        }
    }

    // ---- Set up -------------------------------------------------------------------------------------------
    // 0.2: an image sequence is written into a folder of its own next to the output, its files moved into place at
    // the end (a failed or cancelled export leaves none).
    const bool images = settings.container == Container::Image;
    TempFile temp{images ? std::filesystem::path{} : tempPathFor(output)}; // declared before the muxer: closed first
    TempDir imageDir{images ? tempPathFor(output) : std::filesystem::path{}};
    if (images) {
        imageDir.path.replace_extension();
        if (!std::filesystem::create_directory(imageDir.path, ec)) {
            return std::unexpected(Error{ErrorCode::IoError, "Cannot create " + pathToUtf8(imageDir.path)});
        }
    }
    const std::filesystem::path muxPath =
        images ? imageDir.path / pathFromUtf8("frame%06d" + std::string(outputExtension(settings))) : temp.path;
    auto muxerOr = Muxer::create(settings.container, muxPath);
    if (!muxerOr) {
        return std::unexpected(muxerOr.error());
    }
    Muxer muxer = std::move(*muxerOr);

    ExportResult result;
    result.output = output;
    std::unique_ptr<IVideoFrameSource> source;
    SourceStopper stopper;
    std::optional<std::stop_callback<StopSourceOnCancel>> stopSourceOnCancel; // destroyed before the source
    VideoEncoder video;
    std::optional<FrameConverter> converter;
    AVStream* videoStream = nullptr;
    std::optional<AVMasteringDisplayMetadata> masteringDisplay; // I.9, PQ exports
    std::optional<SmartRenderPlan> smart;                        // 0.2: frames copied, not rendered
    if (hasVideo) {
        // I.9: the colour description, and an HDR10 mastering display for PQ (0.2: RGB and JPEG outputs too).
        const ColorTags tags = colorTagsFor(r.dynamicRange, settings.video.codec, settings.video.pixelFormat);
        // A target size is a bitrate with its peaks capped at it (the cap keeps the size; I.9).
        VideoSettings encode = settings.video;
        if (encode.rateControl == RateControl::TargetSize) {
            encode.rateControl = RateControl::Bitrate;
            encode.bitrate = std::max<std::int64_t>(kTargetSizeMinVideoBitrate / 2,
                                                    static_cast<std::int64_t>(static_cast<double>(r.targetVideoBitrate) * rateScale));
            encode.maxBitrate = encode.bitrate;
        }
        result.videoBitrate = encode.rateControl == RateControl::Bitrate ? encode.bitrate : 0;
        VideoEncodeParams params{encode, r.width, r.height, r.frameRate, muxer.wantsGlobalHeader()};
        params.primaries = tags.primaries;
        params.transfer = tags.transfer;
        params.matrix = tags.matrix;
        params.range = tags.range;
        // 0.2: copied packets interleave with encoded ones only if the encoder hands each frame back at once.
        params.noFrameDelay = smartRenderEligible(settings);
        if (r.dynamicRange == DynamicRange::Pq) {
            params.masteringDisplay = masteringDisplayFor(request.snapshot->colorManagement.peakNits);
        }
        masteringDisplay = params.masteringDisplay;
        params.a53Captions = settings.captions.mode == CaptionExport::Embed;
        auto choice = chooseVideoEncoder(params);
        if (!choice) {
            return std::unexpected(choice.error());
        }
        video = std::move(choice->encoder);
        result.videoEncoder = video.name;
        result.dynamicRange = r.dynamicRange;
        result.encoderFallbacks = std::move(choice->fallbacks);
        publish([&](ExportProgress& p) { p.videoEncoder = video.name; }, true);

        // 0.2: Premiere's Effects (look, limiter, overlays) on the output frame.
        std::shared_ptr<FrameEffects> effects;
        if (settings.effects.anyVideo()) {
            FrameEffects::Context context;
            context.sequenceName = sequence->name;
            context.output = output;
            context.projectFile = request.projectFile;
            context.width = r.width;
            context.height = r.height;
            context.frameRate = r.frameRate;
            context.dropFrame = sequence->dropFrame;
            context.rangeStart = r.range.start;
            context.sequenceStartTimecode = sequence->startTimecode;
            context.dynamicRange = r.dynamicRange;
            auto fx = FrameEffects::create(settings.effects, context);
            if (!fx) {
                return std::unexpected(fx.error());
            }
            effects = std::move(*fx);
        }
        FrameConverter::Setup setup;
        setup.sourceWidth = r.sourceWidth;
        setup.sourceHeight = r.sourceHeight;
        setup.renderWidth = r.renderWidth;
        setup.renderHeight = r.renderHeight;
        setup.width = r.width;
        setup.height = r.height;
        setup.format = video.inputFormat;
        setup.tags = tags;
        setup.effects = effects;
        setup.alpha = r.alpha;
        setup.deep = r.tenBitRender;
        auto conv = FrameConverter::create(setup);
        if (!conv) {
            return std::unexpected(conv.error());
        }
        converter = std::move(*conv);
        const VideoSourceFactory factory =
            request.videoSource ? request.videoSource : defaultVideoSourceFactory();
        VideoSourceFormat format{r.sourceWidth, r.sourceHeight, r.frameRate, r.range.start, r.frameCount};
        if (request.settings.captions.mode == CaptionExport::BurnIn) {
            format.burnInCaptions = request.settings.captions;
        }
        format.dynamicRange = r.dynamicRange;
        format.tenBit = r.tenBitRender;
        format.alpha = r.alpha;
        auto src = factory(originals->project, *sequence, format);
        if (!src) {
            return std::unexpected(src.error());
        }
        source = std::move(*src);
        stopper.source = source.get();
        stopSourceOnCancel.emplace(stop, StopSourceOnCancel{source.get()});
        result.videoSource = source->description();
        auto st = muxer.addStream(video.ctx.get());
        if (!st) {
            return std::unexpected(st.error());
        }
        videoStream = *st;
        if (masteringDisplay) {
            setHdrStreamMetadata(videoStream, &*masteringDisplay, nullptr); // the light levels follow at the end
        }
        // 0.2: smart rendering - the frames whose packets are copied from preview or source files.
        if (params.noFrameDelay) {
            SmartRenderPlan::Inputs in;
            in.project = &originals->project;
            in.snapshot = request.snapshot.get();
            in.sequence = sequence.get();
            in.resolved = &r;
            in.settings = &settings;
            in.encoder = videoStream->codecpar;
            in.previews = request.previews.get();
            in.projectFile = request.projectFile;
            smart = SmartRenderPlan::build(in, stop);
            if (smart->copiedFrames() == 0) {
                smart.reset();
            }
        }
    }

    std::optional<AudioEncoder> audio;
    std::unique_ptr<OfflineMix> mix;
    std::unique_ptr<NormalizedMix> normalized;     // 0.2: Loudness Normalization (renders through `mix`)
    std::optional<LoudnessMeasurement> loudness;
    AVStream* audioStream = nullptr;
    // A.6: audio plugins render here before they process (never bypassed while they load) and wait for every block;
    // what goes wrong with one is reported (RenderIncidents) and ends up in the result's warnings.
    const auto pluginIncidents = std::make_shared<wabcut::audio::RenderIncidents>();
    // A.9: channels 0 follows the master (5.1 stays 5.1); a 5.1 master exported to stereo is downmixed by the mix.
    const int channels = exportAudioChannels(settings.audio, sequence->masterFormat);
    AudioSettings audioSettings = settings.audio;
    audioSettings.channels = channels;
    if (hasAudio) {
        collectPluginProblems(*request.snapshot, *sequence, r.range, 0, result.warnings);
        auto enc = AudioEncoder::open(audioSettings, muxer.wantsGlobalHeader(), settings.container == Container::Aiff);
        if (!enc) {
            return std::unexpected(enc.error());
        }
        audio = std::move(*enc);
        result.audioEncoder = audio->name();
        publish([&](ExportProgress& p) { p.audioEncoder = audio->name(); }, true);
        const wabcut::audio::ScopedOfflineRender offline(pluginIncidents); // the master's plugins are created here
        mix = std::make_unique<OfflineMix>(originals->project, sequence, channels, settings.audio.sampleRate,
                                           r.range.start);
        // 0.2, Loudness Normalization: the mix measured first (its own render), then rendered through the gain and
        // the true-peak limiter.
        if (settings.effects.loudness.applied) {
            OfflineMix measured(originals->project, sequence, channels, settings.audio.sampleRate, r.range.start);
            auto m = measureLoudness(measured, channels, settings.audio.sampleRate, r.audioSamples, settings.effects.loudness,
                                     stop);
            if (!m) {
                return std::unexpected(m.error());
            }
            loudness = *m;
            normalized = std::make_unique<NormalizedMix>(*mix, channels, settings.audio.sampleRate, m->gainDb,
                                                         settings.effects.loudness.effectiveTruePeak());
            result.loudnessLufs = std::isfinite(m->integratedLufs) ? std::optional<double>(m->integratedLufs) : std::nullopt;
            result.loudnessGainDb = m->gainDb;
        }
        auto st = muxer.addStream(audio->context());
        if (!st) {
            return std::unexpected(st.error());
        }
        audioStream = *st;
    }
    // 0.2: Metadata - chapters, the start timecode, title and description - and a soft subtitle track.
    if (auto ok = addMetadata(muxer, settings, *sequence, r, result); !ok) {
        return std::unexpected(ok.error());
    }
    std::optional<SubtitleStream> subtitles;
    std::vector<SubtitleCue> cues;
    if (settings.subtitles.enabled && hasVideo) {
        cues = subtitleCues(*request.snapshot, *sequence, settings.captions, r.range);
        if (cues.empty()) {
            result.warnings.push_back("The subtitle track was left out: the sequence has no captions in the range");
        } else {
            auto s = SubtitleStream::add(settings.container, muxer, subtitleLanguage(*sequence, settings));
            if (!s) {
                return std::unexpected(s.error());
            }
            subtitles = std::move(*s);
        }
    }
    if (auto ok = muxer.open(); !ok) {
        return std::unexpected(ok.error());
    }

    // ---- Render -------------------------------------------------------------------------------------------
    const std::int64_t blocksTotal =
        (r.audioSamples + OfflineMix::kBlockFrames - 1) / OfflineMix::kBlockFrames;
    const std::int64_t framesTotal = hasVideo ? r.frameCount : blocksTotal;
    publish(
        [&](ExportProgress& p) {
            p.phase = ExportPhase::Rendering;
            p.framesTotal = framesTotal;
        },
        true);

    std::int64_t samplesDone = 0;
    std::vector<float> audioBuffer;
    auto pumpAudio = [&](std::int64_t upTo) -> std::expected<void, Error> {
        const wabcut::audio::ScopedOfflineRender offline(pluginIncidents); // the job's thread or the audio stage's
        upTo = std::min(upTo, r.audioSamples);
        while (samplesDone < upTo) {
            const std::int64_t n = std::min<std::int64_t>(upTo - samplesDone, 4096);
            audioBuffer.resize(static_cast<std::size_t>(n * channels));
            if (normalized) {
                normalized->render(audioBuffer);
            } else {
                mix->render(audioBuffer);
            }
            if (auto ok = audio->encode(audioBuffer, muxer, audioStream); !ok) {
                return ok;
            }
            samplesDone += n;
        }
        return {};
    };

    if (hasVideo) {
        PacketPtr packet(av_packet_alloc());
        std::optional<AudioStage> audioStage;
        if (hasAudio) {
            audioStage.emplace(
                pumpAudio, [&] { return audio->flush(muxer, audioStream); }, r.audioSamples,
                std::int64_t{2} * settings.audio.sampleRate);
        }
        // 0.2, smart rendering: copied frames are not rendered; their packets come from the preview or source files.
        std::optional<PacketCopier> copier;
        if (smart) {
            copier.emplace(*smart);
            result.copiedFrames = smart->copiedFrames();
        }
        ConvertStage stage(*source, *converter, r.frameCount,
                           smart ? std::optional<std::vector<std::int64_t>>(smart->renderedFrames()) : std::nullopt);
        // P.2 Embed In Output File: each frame's CEA-608 byte pairs as A/53 cc_data (CC1, field 1); frames without
        // captions carry a null pair so the caption channel stays continuous.
        std::map<std::int64_t, std::vector<std::array<std::uint8_t, 2>>> cc;
        const bool embedCaptions = settings.captions.mode == CaptionExport::Embed;
        if (embedCaptions) {
            if (const CaptionTrack* track = captionTrackToExport(*sequence, settings.captions)) {
                const CaptionTrack exported = captionTrackForFile(*request.snapshot, *sequence, *track,
                                                                  settings.captions.redactedWord, settings.captions.words.get());
                interchange::CaptionWriteOptions o;
                o.language = track->language;
                o.placement = track->style.placement == CaptionPlacement::Top ? CaptionPlacement::Top : CaptionPlacement::Bottom;
                o.alignment = track->style.alignment;
                o.display = static_cast<interchange::Cea608Display>(std::min<int>(settings.captions.display608, 4));
                cc = interchange::cea608ForVideo(exported.cues, r.range, r.frameRate, o);
            }
        }
        // Samples that end with video frame k: k frame periods at the audio rate (exact rational, nearest).
        const std::int64_t sampleNum = std::int64_t{settings.audio.sampleRate} * r.frameRate.den;
        for (std::int64_t k = 0; k < r.frameCount; ++k) {
            if (stop.stop_requested() || !pausePoint(stop)) {
                return cancelled();
            }
            if (const CopyFrame* copy = smart ? smart->copyFor(k) : nullptr) {
                auto copied = copier->packet(*copy, k);
                if (!copied) {
                    return std::unexpected(copied.error());
                }
                if (auto ok = muxer.write(copied->get(), video.ctx->time_base, videoStream); !ok) {
                    return std::unexpected(ok.error());
                }
            } else {
                auto converted = stage.next();
                if (!converted) {
                    return std::unexpected(converted.error());
                }
                (*converted)->pts = k;
                (*converted)->duration = 1;
                auto ready = prepareForEncoder(video, std::move(*converted));
                if (!ready) {
                    return std::unexpected(ready.error());
                }
                if (embedCaptions) {
                    const auto it = cc.find(k);
                    const std::size_t pairs = it != cc.end() ? std::min<std::size_t>(it->second.size(), 31) : 1;
                    AVFrameSideData* sd = av_frame_new_side_data(ready->get(), AV_FRAME_DATA_A53_CC, 3 * pairs);
                    if (!sd) {
                        return std::unexpected(Error{ErrorCode::EncoderError, "Cannot attach the closed captions"});
                    }
                    for (std::size_t i = 0; i < pairs; ++i) {
                        sd->data[3 * i] = 0xFC; // cc_valid, NTSC line 21 field 1
                        sd->data[3 * i + 1] = it != cc.end() ? it->second[i][0] : 0x80;
                        sd->data[3 * i + 2] = it != cc.end() ? it->second[i][1] : 0x80;
                    }
                }
                if (const int rc = avcodec_send_frame(video.ctx.get(), ready->get()); rc < 0) {
                    return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Video encoding failed", rc));
                }
                if (auto ok = drainEncoder(video.ctx.get(), packet.get(), muxer, videoStream, false); !ok) {
                    return std::unexpected(ok.error());
                }
            }
            if (audioStage) {
                const auto upTo = mulDiv(k + 1, sampleNum, r.frameRate.num, Rounding::Nearest);
                if (!audioStage->videoReached(std::min(upTo.value_or(r.audioSamples), r.audioSamples))) {
                    return std::unexpected(audioStage->finish().error());
                }
            }
            publish([&](ExportProgress& p) { p.framesDone = k + 1; }, false);
        }
        if (const int rc = avcodec_send_frame(video.ctx.get(), nullptr); rc < 0) {
            return std::unexpected(
                ffmpegError(ErrorCode::EncoderError, "Cannot flush the video encoder", rc));
        }
        if (auto ok = drainEncoder(video.ctx.get(), packet.get(), muxer, videoStream, true); !ok) {
            return std::unexpected(ok.error());
        }
        if (audioStage) {
            audioStage->videoReached(r.audioSamples);
            if (auto ok = audioStage->finish(); !ok) {
                return std::unexpected(ok.error());
            }
        }
    } else {
        for (std::int64_t b = 0; b < blocksTotal; ++b) {
            if (stop.stop_requested() || !pausePoint(stop)) {
                return cancelled();
            }
            if (auto ok = pumpAudio((b + 1) * OfflineMix::kBlockFrames); !ok) {
                return std::unexpected(ok.error());
            }
            publish([&](ExportProgress& p) { p.framesDone = b + 1; }, false);
        }
        if (auto ok = pumpAudio(r.audioSamples); !ok) {
            return std::unexpected(ok.error());
        }
        if (auto ok = audio->flush(muxer, audioStream); !ok) {
            return std::unexpected(ok.error());
        }
    }
    if (stop.stop_requested()) {
        return cancelled();
    }
    // 0.2: the soft subtitle track (its packets interleave by time with what is written already).
    if (subtitles) {
        if (auto ok = subtitles->write(cues, muxer); !ok) {
            return std::unexpected(ok.error());
        }
        result.subtitleCues = static_cast<int>(cues.size());
    }
    // The mix's plugin processors report what went wrong with them as they go away.
    normalized.reset();
    mix.reset();
    for (std::string& incident : pluginIncidents->messages()) {
        result.warnings.push_back(std::move(incident));
    }

    // ---- Finish: trailer, then the atomic rename into place -----------------------------------------------
    setPhase(ExportPhase::Finalizing);
    if (masteringDisplay && source) {
        // I.9: MaxCLL / MaxFALL as measured over every frame, into the movie header the trailer writes ('clli').
        if (const auto light = source->lightLevels(); light && light->frames > 0) {
            AVContentLightMetadata cll{};
            cll.MaxCLL = static_cast<unsigned>(std::clamp(std::ceil(light->maxCll), 0.0, 65535.0));
            cll.MaxFALL = static_cast<unsigned>(std::clamp(std::ceil(light->maxFall), 0.0, 65535.0));
            setHdrStreamMetadata(videoStream, &*masteringDisplay, &cll);
            result.maxCll = static_cast<int>(cll.MaxCLL);
            result.maxFall = static_cast<int>(cll.MaxFALL);
        }
    }
    if (auto ok = muxer.finish(); !ok) {
        return std::unexpected(ok.error());
    }
    // 0.2: XMP, embedded (MP4 / MOV / M4A, before Content Credentials sign the file) or as a sidecar.
    std::string xmpSidecar;
    if (settings.metadata.xmp != XmpMode::None) {
        std::string xmp = xmpPacket(settings, *sequence, r.range, hasVideo ? r.frameRate : sequence->frameRate, "Wabcut");
        const bool embeddable = !images && (settings.container == Container::Mp4 || settings.container == Container::Mov ||
                                            settings.container == Container::M4a);
        if (settings.metadata.xmp == XmpMode::Embed && embeddable) {
            if (auto ok = appendXmpBox(temp.path, xmp); ok) {
                result.xmpEmbedded = true;
            } else {
                result.warnings.push_back("The XMP could not be embedded (" + ok.error().message + "); it is a sidecar");
                xmpSidecar = std::move(xmp);
            }
        } else {
            xmpSidecar = std::move(xmp);
        }
    }
    if (request.finalize && !images && !stop.stop_requested()) {
        std::string warning;
        try {
            warning = request.finalize(temp.path, stop);
        } catch (const std::exception& e) {
            warning = std::string("A finishing step failed: ") + e.what();
        } catch (...) {
            warning = "A finishing step failed.";
        }
        if (!warning.empty()) {
            result.warnings.push_back(std::move(warning));
        }
    }
    std::int64_t imageBytes = 0;
    if (images) {
        // 0.2: the stills into place, numbered from 0 ("<name>000.png", ...), or the single still as the output.
        const std::string ext(outputExtension(settings));
        for (std::int64_t k = 0; k < r.frameCount; ++k) {
            const std::filesystem::path from = imageDir.path / pathFromUtf8(std::format("frame{:06}{}", k, ext));
            const std::filesystem::path to = stillPath(output, k, r.frameCount, settings.video.exportAsSequence);
            imageBytes += static_cast<std::int64_t>(std::filesystem::file_size(from, ec));
            std::filesystem::rename(from, to, ec);
            if (ec) {
                return std::unexpected(Error{ErrorCode::IoError, std::format("Cannot move {} into place: {}",
                                                                             pathToUtf8(to), ec.message())});
            }
            if (k == 0) {
                result.output = to;
            }
        }
        result.imageFiles = r.frameCount;
    } else {
        std::filesystem::rename(temp.path, output, ec); // replaces an existing output
        if (ec) {
            return std::unexpected(
                Error{ErrorCode::IoError, std::format("Cannot move the export into place at {}: {}",
                                                      pathToUtf8(output), ec.message())});
        }
        temp.keep = true;
    }
    if (!xmpSidecar.empty()) {
        const std::filesystem::path file = xmpSidecarPath(output);
        if (auto ok = writeTextFile(file, xmpSidecar); ok) {
            result.xmp = file;
        } else {
            result.warnings.push_back(ok.error().message);
        }
    }
    if (loudness && settings.effects.loudness.writeLog) {
        std::filesystem::path file = output;
        file += ".loudness.txt";
        if (auto ok = writeTextFile(file, loudnessLogText(*loudness, settings.effects.loudness, pathToUtf8(output.filename()))); ok) {
            result.loudnessLog = file;
        } else {
            result.warnings.push_back(ok.error().message);
        }
    }

    // Captions as a sidecar next to the output (SRT / WebVTT), times relative to the exported range.
    auto sidecar = writeCaptionSidecar(*request.snapshot, *sequence, request.settings.captions, r.range, output);
    if (!sidecar) {
        return std::unexpected(Error{sidecar.error().code, "Exported, but " + sidecar.error().message});
    }
    result.captions = std::move(*sidecar);

    result.bytes = images ? imageBytes : static_cast<std::int64_t>(std::filesystem::file_size(output, ec));
    result.frames = hasVideo ? r.frameCount : 0;
    result.audioSamples = hasAudio ? samplesDone : 0;
    result.duration = r.duration;
    result.seconds = secondsSince(started);
    result.realtimeFactor = result.seconds > 0.0 ? r.duration.toSeconds() / result.seconds : 0.0;
    return result;
}

// ---- ExportQueue ------------------------------------------------------------------------------------------

struct ExportQueue::Entry {
    ExportRequest request; // moved into the job when it starts
    ExportJobStatus status;
    std::stop_source cancel;
    std::shared_ptr<PauseGate> gate = std::make_shared<PauseGate>(); // 0.2: pause / resume while it runs
    bool started = false;
    bool finished = false;
};

ExportQueue::ExportQueue(Listener listener)
    : m_listener(std::move(listener)), m_worker([this](std::stop_token stop) { run(stop); }) {}

ExportQueue::~ExportQueue() {
    // Cancelling runs the job's stop callbacks (stopping its video source), so never under m_mutex.
    std::vector<std::stop_source> cancels;
    {
        std::lock_guard lock(m_mutex);
        for (const auto& e : m_entries) {
            cancels.push_back(e->cancel);
        }
    }
    for (std::stop_source& c : cancels) {
        c.request_stop();
    }
    m_worker.request_stop();
    m_changed.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

ExportJobId ExportQueue::enqueue(ExportRequest request, bool hold) {
    std::lock_guard lock(m_mutex);
    auto entry = std::make_shared<Entry>();
    entry->status.id = m_nextId++;
    entry->status.output = request.output;
    entry->status.presetId = request.settings.presetId;
    entry->status.held = hold;
    entry->request = std::move(request);
    m_entries.push_back(entry);
    m_changed.notify_all();
    return entry->status.id;
}

void ExportQueue::start(ExportJobId id) {
    std::optional<ExportJobStatus> report;
    {
        std::lock_guard lock(m_mutex);
        for (const auto& e : m_entries) {
            if (e->status.id == id && !e->finished && !e->started && e->status.held) {
                e->status.held = false;
                report = e->status;
            }
        }
    }
    m_changed.notify_all();
    if (report) {
        notify(*report);
    }
}

void ExportQueue::pause(ExportJobId id) {
    std::optional<ExportJobStatus> report;
    {
        std::lock_guard lock(m_mutex);
        for (const auto& e : m_entries) {
            if (e->status.id != id || e->finished) {
                continue;
            }
            if (e->started) {
                e->gate->pause(); // the job reports Paused at its next frame
            } else if (!e->status.held) {
                e->status.held = true;
                report = e->status;
            }
        }
    }
    if (report) {
        notify(*report);
    }
}

void ExportQueue::resume(ExportJobId id) {
    std::shared_ptr<PauseGate> gate;
    {
        std::lock_guard lock(m_mutex);
        for (const auto& e : m_entries) {
            if (e->status.id == id && !e->finished && e->started) {
                gate = e->gate;
            }
        }
    }
    if (gate) {
        gate->resume();
        return;
    }
    start(id); // a held job: released
}

void ExportQueue::remove(ExportJobId id) {
    std::lock_guard lock(m_mutex);
    std::erase_if(m_entries, [&](const std::shared_ptr<Entry>& e) { return e->status.id == id && e->finished; });
}

void ExportQueue::cancel(ExportJobId id) {
    std::optional<ExportJobStatus> report;
    std::optional<std::stop_source> running;
    {
        std::lock_guard lock(m_mutex);
        for (const auto& e : m_entries) {
            if (e->status.id != id || e->finished) {
                continue;
            }
            if (e->started) {
                running = e->cancel; // requested below, outside the lock: it runs the job's stop callbacks
            } else {                 // never ran: finish it here
                e->cancel.request_stop();
                e->finished = true;
                e->status.progress.phase = ExportPhase::Cancelled;
                e->status.error = Error{ErrorCode::Cancelled, "Export cancelled"};
                report = e->status;
            }
        }
    }
    if (running) {
        running->request_stop();
    }
    m_changed.notify_all();
    if (report) {
        notify(*report);
    }
}

std::vector<ExportJobStatus> ExportQueue::jobs() const {
    std::lock_guard lock(m_mutex);
    std::vector<ExportJobStatus> out;
    for (const auto& e : m_entries) {
        out.push_back(e->status);
    }
    return out;
}

std::optional<ExportJobStatus> ExportQueue::status(ExportJobId id) const {
    std::lock_guard lock(m_mutex);
    for (const auto& e : m_entries) {
        if (e->status.id == id) {
            return e->status;
        }
    }
    return std::nullopt;
}

std::optional<ExportJobStatus> ExportQueue::wait(ExportJobId id) const {
    std::unique_lock lock(m_mutex);
    std::shared_ptr<Entry> entry;
    for (const auto& e : m_entries) {
        if (e->status.id == id) {
            entry = e;
        }
    }
    if (!entry) {
        return std::nullopt;
    }
    m_changed.wait(lock, [&] { return entry->finished; });
    return entry->status;
}

void ExportQueue::notify(const ExportJobStatus& status) {
    if (m_listener) {
        m_listener(status);
    }
}

void ExportQueue::run(std::stop_token stop) {
    for (;;) {
        std::shared_ptr<Entry> entry;
        {
            std::unique_lock lock(m_mutex);
            const auto next = [&] {
                const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                                             [](const auto& e) { return !e->started && !e->finished && !e->status.held; });
                return it == m_entries.end() ? nullptr : *it;
            };
            if (!m_changed.wait(lock, stop, [&] { return next() != nullptr; })) {
                return; // stop requested
            }
            entry = next();
            entry->started = true;
        }
        ExportRequest request = std::move(entry->request);
        request.pause = entry->gate;
        auto callerCallback = std::move(request.onProgress);
        request.onProgress = [this, entry, callerCallback](const ExportProgress& p) {
            // 0.4: a terminal phase (Done, Failed, Cancelled) is not reported here: the final status below reports
            // the outcome together with its result or error (a listener that saw "Done" without the result lost the
            // export's warnings, and "Failed" without the error its reason). The caller's own callback still sees it.
            const bool terminal = p.phase == ExportPhase::Done || p.phase == ExportPhase::Failed ||
                                  p.phase == ExportPhase::Cancelled;
            if (!terminal) {
                ExportJobStatus copy;
                {
                    std::lock_guard lock(m_mutex);
                    entry->status.progress = p;
                    copy = entry->status;
                }
                notify(copy);
            }
            if (callerCallback) {
                callerCallback(p);
            }
        };
        ExportJob job(std::move(request));
        auto result = job.run(entry->cancel.get_token());
        ExportJobStatus finalStatus;
        {
            std::lock_guard lock(m_mutex);
            entry->status.progress = job.progress();
            if (result) {
                entry->status.result = std::move(*result);
            } else {
                entry->status.error = result.error();
            }
            entry->finished = true;
            finalStatus = entry->status;
        }
        m_changed.notify_all();
        notify(finalStatus);
    }
}

} // namespace wabcut::exporter
