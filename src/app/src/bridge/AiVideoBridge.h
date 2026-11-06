#pragma once

// AiVideoBridge: AI Restore and AI Denoise in the app (singleton `AiVideo`, 0.7; docs/ai.md "AI video effects").
//
// The effects are ordinary clip effects (Effect Controls rows, keyframable Amount, saved with the project, one undo step per
// edit); their pictures are made ahead of time by the runner (wabcut/ai/video) and kept per clip in the processed cache.
// This bridge is the part the interface needs:
//   * effectInfo(section): the state of one effect for its header in Effect Controls (AiEffectStatus.qml): which models it
//     needs and whether they are on this computer (with the size and licence card to show before a download), how many of the
//     clip's pictures are processed, an estimate of what is left, whether the chain does anything with these settings;
//   * process(section) / processAll(): the runner on a worker thread (the helper process does the inference; the GUI thread
//     never waits for it), progress and Cancel; the playhead frame shows each picture as soon as it is stored;
//   * downloadModels(section): the models of the chain through the model store with their licence card shown first (the
//     AI network stack: proxies, offline mode, the project's no-cloud lock);
//   * exportNeeds(range): the clips an export reaches whose AI pictures are missing, and whether it can make them (the Export
//     panel's notice; the export itself makes what is missing, playback::ScopedSourceStageMode).
// Nothing here changes the project: settings are edited through Effect Controls like any effect's.

#include <wabcut/ai/video/Backend.h>
#include <wabcut/ai/video/MediaFrames.h>
#include <wabcut/ai/video/ProcessedCache.h>
#include <wabcut/model/AiEffects.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace wabcut::app {

class ProjectController;

class AiVideoBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(AiVideo)
    QML_SINGLETON

    /// The effect being processed ("e12"; "*" = all of the sequence; "" = none).
    Q_PROPERTY(QString section READ section NOTIFY changed FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY changed FINAL)
    /// 0..1 of the frames of the run.
    Q_PROPERTY(double progress READ progress NOTIFY changed FINAL)
    /// What the run is doing or how the last one ended ("Processing 12 of 240 frames on DirectML", "Cancelled", why it failed).
    Q_PROPERTY(QString message READ message NOTIFY changed FINAL)
    /// A model download is running ("e12" = for that effect), its progress 0..1.
    Q_PROPERTY(QString downloadSection READ downloadSection NOTIFY changed FINAL)
    Q_PROPERTY(bool downloading READ downloading NOTIFY changed FINAL)
    Q_PROPERTY(double downloadProgress READ downloadProgress NOTIFY changed FINAL)
    /// Changes whenever pictures were made or deleted: bindings that read effectInfo() depend on it.
    Q_PROPERTY(int revision READ revision NOTIFY changed FINAL)

public:
    explicit AiVideoBridge(QObject* parent = nullptr);
    ~AiVideoBridge() override;

    QString section() const { return m_section; }
    bool busy() const { return !m_section.isEmpty(); }
    double progress() const { return m_progress; }
    QString message() const { return m_message; }
    QString downloadSection() const { return m_downloadSection; }
    bool downloading() const { return !m_downloadSection.isEmpty(); }
    double downloadProgress() const { return m_downloadProgress; }
    int revision() const { return m_revision; }

    /// What the header of effect `section` shows. {ai: false} for any other effect. Otherwise:
    ///   ai, kind ("restore" | "denoise"), label ("Restore 2x, Denoise" - the clip's whole chain),
    ///   state: "unsupported" (a title, graphic, still, nested clip), "inactive" (these settings change nothing), "noHelper"
    ///          (this build has no AI helper), "needsModel" (a download first), "processing", "notProcessed", "partial",
    ///          "ready";
    ///   message (a sentence for the state), frames (the clip's pictures to make), processed, percent (0..100),
    ///   estimate ("about 4 min" for what is left), outputSize ("3840 x 2160"), models (names), downloadBytes,
    ///   downloadText ("Download 131 MB"), card (the licences, shown before the download), device (what it last ran on).
    Q_INVOKABLE QVariantMap effectInfo(const QString& section) const;
    /// Makes the pictures of the effect's clip (the whole chain of the clip, the part of the media it uses) in the
    /// background. False when one runs, the effect is not an AI effect, nothing is left to do or a model is missing (the
    /// message says which).
    Q_INVOKABLE bool process(const QString& section);
    /// Every clip of the current sequence with AI effects that has pictures missing (one job, one progress bar).
    Q_INVOKABLE bool processAll();
    Q_INVOKABLE void cancel();
    /// Deletes the processed pictures of the effect's chain for this media (they are made again by Process).
    Q_INVOKABLE void discard(const QString& section);
    /// Downloads the models of the effect's chain that are not on this computer (the card was shown before: calling this
    /// is the user's yes). False when offline mode is on, one runs, or nothing is missing.
    Q_INVOKABLE bool downloadModels(const QString& section);
    Q_INVOKABLE void cancelDownload();
    /// 0.7 (face-aware Morph Cut): what the Effect Controls header of the selected Morph Cut shows about its face detector. {morphCut: false}
    /// when the selected transition is not a Morph Cut. Otherwise: morphCut, faceAware (its Face Aware switch), installed (YuNet, 0.2 MB, is on
    /// this computer), state ("off" | "ready" | "needsModel" | "noHelper"), message, downloadText ("Download 0.2 MB"), card (the licence, shown before
    /// the download). downloadModels("morph") downloads the detector.
    Q_INVOKABLE QVariantMap morphInfo() const;
    /// {count (clips with AI pictures missing in the exported range), frames, names ["V1: Interview at 00:00:12:00 (Restore 2x)"],
    ///  canMake (the models are on this computer and the helper is in this build), why (when it cannot), estimate}.
    Q_INVOKABLE QVariantMap exportNeeds(const QString& range) const;
    /// {count (clips with AI effects), pending (not fully processed)} for the current sequence.
    Q_INVOKABLE QVariantMap overview() const;

    // ---- Tests ----
    /// Replaces the cache, the model backend and the decoder-backed frame source (default: the process-wide services).
    void setServices(std::shared_ptr<ai::video::ProcessedCache> cache, std::shared_ptr<ai::video::IVideoBackend> backend,
                     std::function<std::unique_ptr<ai::video::IFrameSource>(const std::string& path, int containerStream)> sources = {});
    /// The model store root (default ModelStore::defaultRoot()); what the effects say about their models is read again.
    void setModelRoot(std::filesystem::path root) {
        m_modelRoot = std::move(root);
        ++m_revision;
        emit changed();
    }
    /// Runs process() on the calling thread.
    void setSynchronous(bool synchronous) { m_synchronous = synchronous; }
    /// Whether this build has the AI helper (default: it is next to the executable; tests with a fake backend say yes).
    void setHelperAvailable(bool available) { m_helperOverride = available; }

signals:
    void changed();

private:
    struct Target;
    struct Found;
    ProjectController* controller() const;
    std::optional<Found> find(const QString& section) const;
    std::optional<Target> targetOf(const Found& found) const;
    std::vector<Target> targetsOfSequence(bool onlyIncomplete, const std::optional<TimeRange>& within) const;
    bool helperAvailable() const;
    std::shared_ptr<ai::video::ProcessedCache> cache() const;
    std::filesystem::path modelRoot() const;
    QVariantMap infoOf(const Target& target, const QString& section) const;
    std::size_t coveredOf(const Target& target) const;
    bool run(std::vector<Target> targets, const QString& section);
    void finish(const QString& message);
    void refreshPlayback();
    void touch();

    QString m_section;
    double m_progress = 0.0;
    QString m_message;
    QString m_downloadSection;
    double m_downloadProgress = 0.0;
    int m_revision = 0;
    QString m_lastDevice;

    std::shared_ptr<ai::video::ProcessedCache> m_cache;
    std::shared_ptr<ai::video::IVideoBackend> m_backend;
    std::function<std::unique_ptr<ai::video::IFrameSource>(const std::string&, int)> m_sources;
    std::filesystem::path m_modelRoot;
    bool m_synchronous = false;
    std::optional<bool> m_helperOverride;
    std::stop_source m_cancel;
    std::jthread m_worker;
    std::stop_source m_downloadStop;
    std::jthread m_download;
    std::uint64_t m_seenCacheRevision = 0;
};

} // namespace wabcut::app
