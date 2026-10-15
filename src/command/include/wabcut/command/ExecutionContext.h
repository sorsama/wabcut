#pragma once

// What an executor works with: the input state, its (already schema-validated) params, and typed readers
// that resolve the protocol's references.
//
// References a command may use:
//   sequence  sequence id; optional when the project has exactly one sequence
//   time      integer ticks (254,016,000,000 per second) or a timecode string in the sequence's rate and
//             drop-frame setting ("00:01:02:03", "00:01:02;03"; a leading '-' for negative deltas)
//   track     a label ("V1", "A2") or a track id
//   clip      a clip id, or a selector {"track": "V1", "time": ...} for the clip under that time
//   media     a media id or its exact name

#include <wabcut/command/Command.h>
#include <wabcut/model/CaptionText.h>
#include <wabcut/model/Clipboard.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/GamingOps.h>
#include <wabcut/model/History.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectOps.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {
class TextMeasurer;     // GraphicResolve.h
class PreviewCache;     // PreviewCache.h (C.15)
class PreviewRenderer;  // PreviewCache.h (C.15)
} // namespace wabcut

namespace wabcut::command {

/// Thrown by executors and the readers below; the engine turns it into the request's CommandError.
struct CommandFailure {
    CommandError error;
};

[[noreturn]] void fail(ErrorCode code, std::string message, Json details = nullptr);

/// Lets the caller of a long request follow and stop it. The engine calls checkpoint() before each command
/// of a batch and before committing; long commands call it while they work (ExecutionContext::checkpoint).
/// Stopping is cooperative: work between two checkpoints always finishes, and nothing is committed after a
/// checkpoint returned an error.
class ExecutionControl {
public:
    virtual ~ExecutionControl() = default;
    /// `message` says what comes next ("step 2 of 5: sequence.addEdit"). Returns an error (Cancelled,
    /// TimedOut) to stop the request, or nullopt to go on.
    virtual std::optional<CommandError> checkpoint(std::string_view message) = 0;
};

/// What some commands need besides the project, provided by the host (EnginePolicy::services): for example the
/// transcripts of the project's media (wabcut/ai/edit/TranscriptCommands.h). Executors downcast to the services
/// they know and fail with InvalidRequest when the host provides none.
class CommandServices {
public:
    virtual ~CommandServices() = default;
    /// The services this one bundles (ServiceBundle); findService() looks into them.
    virtual std::vector<CommandServices*> parts() const { return {}; }
};

/// Several kinds of services in one (EnginePolicy::services holds a single pointer): transcripts, media analysis.
class ServiceBundle : public CommandServices {
public:
    explicit ServiceBundle(std::vector<std::shared_ptr<CommandServices>> services) : m_services(std::move(services)) {}
    std::vector<CommandServices*> parts() const override {
        std::vector<CommandServices*> out;
        for (const auto& s : m_services) {
            out.push_back(s.get());
        }
        return out;
    }

private:
    std::vector<std::shared_ptr<CommandServices>> m_services;
};

/// The services of type T: `services` itself or one of its parts (depth first); null when there are none.
template <class T>
T* findService(CommandServices* services) {
    if (!services) {
        return nullptr;
    }
    if (auto* found = dynamic_cast<T*>(services)) {
        return found;
    }
    for (CommandServices* part : services->parts()) {
        if (auto* found = findService<T>(part)) {
            return found;
        }
    }
    return nullptr;
}

/// Caption line-break rules (CaptionText.h): a host with ICU or Qt provides its LineBreaker, so Thai, Lao, Khmer and
/// CJK text breaks between words that have no spaces; without one the caption commands use the model's
/// simpleLineBreaker() (spaces and hyphens). Bundle it with the other services (ServiceBundle).
class CaptionServices : public CommandServices {
public:
    explicit CaptionServices(std::shared_ptr<const LineBreaker> breaker) : m_breaker(std::move(breaker)) {}
    const LineBreaker& lineBreaker() const { return m_breaker ? *m_breaker : simpleLineBreaker(); }

private:
    std::shared_ptr<const LineBreaker> m_breaker;
};

/// What multicam.create needs to sync angles by their audio (C.8): the audio engineer's waveform cross-correlation
/// (AudioSyncAnalyzer, Nesting.h). Without it, "audio" sync is refused; OffsetTableAudioSync serves tests.
class MulticamServices : public CommandServices {
public:
    explicit MulticamServices(std::shared_ptr<const AudioSyncAnalyzer> audioSync) : m_audioSync(std::move(audioSync)) {}
    const AudioSyncAnalyzer* audioSync() const { return m_audioSync.get(); }

private:
    std::shared_ptr<const AudioSyncAnalyzer> m_audioSync;
};

/// The clipboard of clipboard.copy / cut / paste and clip.pasteAttributes (C.11, Clipboard.h). A host shares one
/// between its engines (the app's timeline and its assistant); an engine whose services have none keeps its own
/// (CommandEngine), so every CLI and MCP session has a clipboard. Thread-safe.
class ClipboardServices : public CommandServices {
public:
    ClipboardServices() : m_store(std::make_shared<Store>()) {}
    void set(ClipboardDocument document) {
        std::lock_guard lock(m_store->mutex);
        m_store->document = std::move(document);
    }
    std::optional<ClipboardDocument> get() const {
        std::lock_guard lock(m_store->mutex);
        return m_store->document;
    }
    void clear() {
        std::lock_guard lock(m_store->mutex);
        m_store->document.reset();
    }
    /// The same clipboard for a caller whose reads can reach a cloud model (the app's assistant): clips copied from a
    /// no-cloud project (ClipboardDocument::noCloud) are refused to it outside a no-cloud project (review wave 5,
    /// S5-P2-12).
    std::shared_ptr<ClipboardServices> forCloudCaller() const {
        auto view = std::make_shared<ClipboardServices>(*this);
        view->m_cloudBound = true;
        return view;
    }
    bool cloudBound() const { return m_cloudBound; }

private:
    struct Store {
        std::mutex mutex;
        std::optional<ClipboardDocument> document;
    };
    std::shared_ptr<Store> m_store;
    bool m_cloudBound = false;
};

/// The project's version history (C.9, History.h) for the history.* commands; the host opens it next to the saved
/// project file (ProjectHistory::folderFor). Without it, history commands are refused.
class HistoryServices : public CommandServices {
public:
    explicit HistoryServices(std::shared_ptr<ProjectHistory> history) : m_history(std::move(history)) {}
    ProjectHistory* history() const { return m_history.get(); }

private:
    std::shared_ptr<ProjectHistory> m_history;
};

class ReviewFrameSource;
/// Thumbnails for review packages (ReviewPackage.h): the host's renderer. Without it packages show placeholder cards.
class ReviewServices : public CommandServices {
public:
    explicit ReviewServices(std::shared_ptr<const ReviewFrameSource> frames) : m_frames(std::move(frames)) {}
    const ReviewFrameSource* frames() const { return m_frames.get(); }

private:
    std::shared_ptr<const ReviewFrameSource> m_frames;
};

/// Motion graphics (C.13) for the graphic.*, template.* and lottie.* commands: the user's template library folder (the app:
/// one in the user's documents; wabcut-cli: the WABCUT_TEMPLATE_LIBRARY folder), where template.save "library": "user"
/// writes and template.list / template.instantiate also look; and the host's text measurer (its font engine), so
/// graphic.resolve measures text as the renderer will. Without it only the project's template library exists and text is
/// measured approximately.
class GraphicsServices : public CommandServices {
public:
    explicit GraphicsServices(std::filesystem::path userLibrary, std::shared_ptr<const TextMeasurer> measurer = {})
        : m_userLibrary(std::move(userLibrary)), m_measurer(std::move(measurer)) {}
    /// Empty: no user library.
    const std::filesystem::path& userLibrary() const { return m_userLibrary; }
    const TextMeasurer* measurer() const { return m_measurer.get(); }

private:
    std::filesystem::path m_userLibrary;
    std::shared_ptr<const TextMeasurer> m_measurer;
};

/// C.15, the preview render cache (PreviewCache.h): the project's cache (its folder, index and budget) and the render
/// owner's PreviewRenderer that fills it. render.previewInOut needs both, render.deletePreviewFiles the cache;
/// render.status shows rendered segments when the cache is there. The host also calls PreviewCache::invalidate with
/// every ChangeSet (EditSession listener) so entries stay valid.
class PreviewServices : public CommandServices {
public:
    PreviewServices(std::shared_ptr<PreviewCache> cache, std::shared_ptr<PreviewRenderer> renderer = {})
        : m_cache(std::move(cache)), m_renderer(std::move(renderer)) {}
    PreviewCache* cache() const { return m_cache.get(); }
    PreviewRenderer* renderer() const { return m_renderer.get(); }

private:
    std::shared_ptr<PreviewCache> m_cache;
    std::shared_ptr<PreviewRenderer> m_renderer;
};

/// C.16, sequence.removeDeadTime: how busy each media item is (GamingOps.h MediaActivity: game-audio loudness and picture
/// motion over its time). The host measures it - the AI engineer's signal curves from the analysis cache when there are
/// some, else a cheap pass over the proxy (or the original) and its audio - and should cache it. `progress` takes 0..1 and
/// returns false to stop (then return ErrorCode::Cancelled). Errors (offline media...) leave that media unmeasured: the
/// command never removes time it could not measure.
class ActivityServices : public CommandServices {
public:
    using Measure = std::function<std::expected<MediaActivity, wabcut::Error>(const ProjectState& state, MediaId media,
                                                                              const std::function<bool(double)>& progress)>;
    explicit ActivityServices(Measure measure) : m_measure(std::move(measure)) {}
    std::expected<MediaActivity, wabcut::Error> activity(const ProjectState& state, MediaId media,
                                                         const std::function<bool(double)>& progress) const {
        return m_measure(state, media, progress);
    }

private:
    Measure m_measure;
};

/// C.16, sequence.makeVersion: the host's Auto Reframe (GamingOps.h ReframeFunction; the app and wabcut-cli wrap
/// ai::reframe::reframeSequence). Without it, versions without a facecam use the blur fill.
class VersionServices : public CommandServices {
public:
    explicit VersionServices(ReframeFunction reframe) : m_reframe(std::move(reframe)) {}
    const ReframeFunction& reframe() const { return m_reframe; }

private:
    ReframeFunction m_reframe;
};

class ExecutionContext {
public:
    ExecutionContext(const ProjectState& state, const Json& params, ExecutionControl* control = nullptr,
                     CommandServices* services = nullptr);

    /// The host's services, or null.
    CommandServices* services() const { return m_services; }
    /// The caller's control, or null.
    ExecutionControl* control() const { return m_control; }
    /// The request is a dry run: the state an executor returns is not committed, so executors that act outside the
    /// project (template.save to the user's library) only say what they would do.
    bool dryRun() const { return m_dryRun; }
    void setDryRun(bool dryRun) { m_dryRun = dryRun; }
    /// What earlier commands of the same request staged for after the commit (CommandOutput::afterCommit), by name:
    /// a paste after a copy in one batch reads the copied clips before the batch commits. Set by the engine for one
    /// request; null outside one.
    using Stage = std::map<std::string, std::shared_ptr<const void>, std::less<>>;
    Stage* stage() const { return m_stage; }
    void setStage(Stage* stage) { m_stage = stage; }

    const ProjectState& state() const { return m_state; }
    const Json& params() const { return m_params; }

    /// The sequence named by "sequence", or the project's only sequence.
    const Sequence& sequence();
    SequenceId sequenceId() { return sequence().id; }

    bool has(std::string_view key) const;

    Ticks time(std::string_view key);
    std::optional<Ticks> optionalTime(std::string_view key);
    Ticks timeValue(const Json& value, const std::string& path);

    TrackId track(std::string_view key);
    TrackId trackValue(const Json& value, const std::string& path);
    std::vector<TrackId> tracks(std::string_view key);

    ClipId clip(std::string_view key);
    ClipId clipValue(const Json& value, const std::string& path);
    std::vector<ClipId> clips(std::string_view key);

    MediaId media(std::string_view key);
    /// A list of media references (empty when the key is absent).
    std::vector<MediaId> mediaList(std::string_view key);
    MediaId mediaValue(const Json& value, const std::string& path);

    std::string string(std::string_view key, std::string fallback = {}) const;
    /// An integer param; one above int64 fails with InvalidParams instead of wrapping.
    std::int64_t integer(std::string_view key, std::int64_t fallback = 0) const;
    /// An index read from a param or array element: an integer >= 0 that fits int64, else InvalidParams.
    std::size_t indexValue(const Json& value, const std::string& path) const;
    double number(std::string_view key) const;
    bool boolean(std::string_view key, bool fallback) const;
    /// "linked" (default true): whether linked partners take part.
    edit::LinkMode linkMode() const;

    /// The edit's result, or a CommandFailure carrying the model's error.
    edit::EditResult take(edit::EditOutcome outcome);
    /// Throws (as take) for a failed model operation that returns nothing.
    void take(std::expected<void, Error> outcome) {
        if (!outcome) {
            failFromModel(outcome.error());
        }
    }
    template <class T>
    T take(std::expected<T, Error> outcome) {
        if (!outcome) {
            failFromModel(outcome.error());
        }
        return std::move(*outcome);
    }

    /// Timecode of `t` in the sequence's rate and drop-frame setting.
    std::string timecode(Ticks t);
    /// "V1" / "A2" for a track of the sequence.
    std::string trackLabel(TrackId track);

    /// Long commands call this between units of work: it reports progress to the caller's ExecutionControl
    /// (if any) and throws a CommandFailure when the caller stops the request.
    void checkpoint(std::string_view message);

private:
    [[noreturn]] static void failFromModel(const Error& error);

    const ProjectState& m_state;
    const Json& m_params;
    ExecutionControl* m_control = nullptr;
    CommandServices* m_services = nullptr;
    bool m_dryRun = false;
    Stage* m_stage = nullptr;
    const Sequence* m_sequence = nullptr;
};

/// The host's caption line breaker (CaptionServices), or simpleLineBreaker().
const LineBreaker& lineBreakerOf(ExecutionContext& context);

} // namespace wabcut::command
