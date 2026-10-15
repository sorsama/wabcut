#pragma once

// HighlightsBridge: the gaming Highlights panel (singleton `Highlights`, M5o) on the AI platform's G.2 commands
// (draft 3) highlights.importChat, highlights.find, highlights.toReel and highlights.toShort (all local):
//
//   * importChat(file): highlights.importChat as a job: the chat log (TwitchDownloader, yt-dlp live chat,
//     chat-downloader JSON) is read and kept with the recording's analyses (no project change, no undo step); its
//     format, message count and recording are shown, and every later find counts the chat on its own.
//   * find(options): highlights.find on the current sequence (signals, count, minScore, the chat's offset) as a command
//     job: a copy of the project on a worker thread, its "measuring 'VOD.mkv' 42%" checkpoints as activity and
//     progress, cancel() at the next checkpoint. The highlight markers it writes (Marker::highlight, C.15) become one
//     undo step when nothing changed meanwhile.
//   * The moments are read back from those markers (every marker with highlight metadata, whatever found it), so they
//     are listed again on open, on a sequence switch and after undo / redo; a sequence without highlights (a reel just
//     made, a new sequence) keeps the last list (otherSequence). Best score first: rank, range, score, confidence,
//     title, reasons, the signals the last find measured (as bars), a waveform of the range (the timeline's peaks) and
//     a frame at the peak (image://wabcut-frames: decoded off the GUI thread, cached, source redactions applied).
//     accept(rank) / reject(rank) review them, kept here by the marker's range (so a re-read keeps them); rejected
//     moments are listed last.
//   * jump(rank): the playhead to the peak (revealRequested: Main keeps it in view); preview(rank) plays the range
//     from its start and pauses at its end.
//   * makeReel(options): highlights.toReel with the accepted moments (else every moment not rejected) in time order;
//     makeShort(rank, options): highlights.toShort (on layout.apply: auto, vertical split or blurred fill, the facecam
//     clip or a facecam region) for that moment's range. Both run as jobs and open the new sequence; the Short's
//     layout is said in the message.
//   * available: the catalog has highlights.find (G.2). Until then the panel says highlights arrive with the next
//     engine update.

#include <wabcut/model/Ids.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/Time.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::command {
class CommandRegistry;
class CommandServices;
struct CommandResult;
}

namespace wabcut::app {

class BridgeJob;
class PlaybackBridge;
class ProjectController;

class HighlightsBridge : public QObject {
    Q_OBJECT
    /// The catalog has highlights.find (G.2).
    Q_PROPERTY(bool available READ available NOTIFY catalogChanged FINAL)
    Q_PROPERTY(bool reelAvailable READ reelAvailable NOTIFY catalogChanged FINAL)
    Q_PROPERTY(bool shortAvailable READ shortAvailable NOTIFY catalogChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY jobChanged FINAL)
    /// What runs: "find", "reel", "short", or "".
    Q_PROPERTY(QString job READ job NOTIFY jobChanged FINAL)
    /// 0..1, or -1 when unknown.
    Q_PROPERTY(double progress READ progress NOTIFY jobChanged FINAL)
    /// The last checkpoint without its percentage ("Measuring 'VOD.mkv'").
    Q_PROPERTY(QString activity READ activity NOTIFY jobChanged FINAL)
    /// The highlight markers' moments, best first, rejected ones last: [{ rank, start, end, peak, thumbnail, duration,
    /// score, confidence, confidenceLabel, title, reasons: [..], signals: [{ key, label, value }] (from the last find),
    /// waveform: [0..1 x 32], image, rangeText, durationText, peakText, state: "accepted" | "rejected" | "" }].
    Q_PROPERTY(QVariantList moments READ moments NOTIFY momentsChanged FINAL)
    /// A find has finished on the listed sequence (the list may still be empty: nothing stood out).
    Q_PROPERTY(bool searched READ searched NOTIFY momentsChanged FINAL)
    /// The sequence the moments are on, and whether it is not the current one (a reel or a new sequence is shown).
    Q_PROPERTY(QString sequenceName READ sequenceName NOTIFY shownChanged FINAL)
    Q_PROPERTY(bool otherSequence READ otherSequence NOTIFY shownChanged FINAL)
    Q_PROPERTY(int acceptedCount READ acceptedCount NOTIFY momentsChanged FINAL)
    /// How many moments makeReel() would use.
    Q_PROPERTY(int reelCount READ reelCount NOTIFY momentsChanged FINAL)
    /// The last find's recordings: [{ media, name, audioRoles, video, markers, chat, fromCache }].
    Q_PROPERTY(QVariantList recordings READ recordings NOTIFY momentsChanged FINAL)
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY momentsChanged FINAL)
    /// The last find's recordings and the chat messages it counted (an imported log), when it ran on the listed
    /// sequence; else 0. The panel words them ("Recordings: 1 · Chat messages: 18,234").
    Q_PROPERTY(int recordingCount READ recordingCount NOTIFY momentsChanged FINAL)
    Q_PROPERTY(int chatMessages READ chatMessages NOTIFY momentsChanged FINAL)
    /// The moment Make a Short uses and the list's keyboard position (0 = none).
    Q_PROPERTY(int selectedRank READ selectedRank WRITE setSelectedRank NOTIFY selectedRankChanged FINAL)
    /// The moment preview() is playing (0 = none).
    Q_PROPERTY(int previewRank READ previewRank NOTIFY previewChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)
    /// The catalog has highlights.importChat (G.2 draft 2).
    Q_PROPERTY(bool chatAvailable READ chatAvailable NOTIFY catalogChanged FINAL)
    /// The chat log importChat() read last (a local path, "" = none) and its file name.
    Q_PROPERTY(QString chatFile READ chatFile NOTIFY chatChanged FINAL)
    Q_PROPERTY(QString chatFileName READ chatFileName NOTIFY chatChanged FINAL)
    /// What the import read: { format, messages, media, name, first, last, offset }, or {} (none yet).
    Q_PROPERTY(QVariantMap chat READ chat NOTIFY chatChanged FINAL)
    /// "Twitch chat · Messages: 18,234 · Gameplay.mkv" (the count in the locale's format), or "".
    Q_PROPERTY(QString chatSummary READ chatSummary NOTIFY chatChanged FINAL)
    /// Chat time + offset = recording time (s): sent with the import; find() sends it when it changed since.
    Q_PROPERTY(double chatOffset READ chatOffset WRITE setChatOffset NOTIFY chatChanged FINAL)
    Q_PROPERTY(QStringList chatFileFilters READ chatFileFilters CONSTANT FINAL)

public:
    explicit HighlightsBridge(QObject* parent = nullptr);
    ~HighlightsBridge() override;
    static HighlightsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setController(ProjectController* controller);
    void setPlayback(PlaybackBridge* playback);
    /// Tests: the catalog the commands run against (default: the editing commands); null = the default.
    void setRegistry(const command::CommandRegistry* registry);
    /// The services the commands get (default: the assistant's, made on the GUI thread per job).
    void setServicesFactory(std::function<std::shared_ptr<command::CommandServices>()> factory) {
        m_servicesFactory = std::move(factory);
    }
    /// Tests: jobs run on the calling thread.
    void setSynchronous(bool synchronous) { m_synchronous = synchronous; }
    /// Tests: what preview() calls to play (true) and pause (false) instead of the Program Monitor's playback.
    void setTransport(std::function<void(bool play)> transport) { m_transport = std::move(transport); }

    bool available() const;
    bool reelAvailable() const;
    bool shortAvailable() const;
    bool busy() const;
    QString job() const { return m_jobKind; }
    double progress() const { return m_progress; }
    QString activity() const { return m_activity; }
    QVariantList moments() const { return m_list; }
    bool searched() const { return m_searched; }
    QString sequenceName() const;
    bool otherSequence() const;
    int acceptedCount() const;
    int reelCount() const;
    QVariantList recordings() const { return m_recordings; }
    QStringList warnings() const { return m_warnings; }
    int recordingCount() const { return m_recordingCount; }
    int chatMessages() const { return m_chatMessages; }
    int selectedRank() const { return m_selectedRank; }
    void setSelectedRank(int rank);
    int previewRank() const { return m_previewRank; }
    QString message() const { return m_message; }
    bool chatAvailable() const;
    QString chatFile() const { return m_chatFile; }
    QString chatFileName() const;
    QVariantMap chat() const { return m_chat; }
    QString chatSummary() const;
    double chatOffset() const { return m_chatOffset; }
    void setChatOffset(double seconds);
    QStringList chatFileFilters() const;

    /// highlights.importChat for the current sequence's recording (a file dialog's URL or a path), with chatOffset,
    /// as a job; the chat then counts in every find (the "chat" signal). "" or why not.
    Q_INVOKABLE QString importChat(const QString& fileOrUrl);
    /// highlights.find's parameters for `options` { signals: [..], count, minScore, minGap, chatDelay, keywords: [..] },
    /// clamped to the command's ranges, with the current sequence (and chatOffset when it changed since the
    /// chat log was imported).
    Q_INVOKABLE QVariantMap findParams(const QVariantMap& options) const;
    /// Runs highlights.find as a job. "" or why not.
    Q_INVOKABLE QString find(const QVariantMap& options = {});
    /// Stops the running job at its next checkpoint (nothing is changed).
    Q_INVOKABLE void cancel();
    /// Toggles a moment's review: accepted <-> undecided, rejected <-> undecided.
    Q_INVOKABLE void accept(int rank);
    Q_INVOKABLE void reject(int rank);
    /// The playhead to the moment's peak, on its sequence; selects it. "" or why not.
    Q_INVOKABLE QString jump(int rank);
    /// Plays the moment's range (again: stops). "" or why not.
    Q_INVOKABLE QString preview(int rank);
    Q_INVOKABLE void stopPreview();
    /// The moments makeReel() sends: accepted ones (else every one not rejected), in time order.
    Q_INVOKABLE QVariantList reelMoments() const;
    /// highlights.toReel's parameters for `options` { order, maxDuration, transition, music, slowMotion, count, name }.
    Q_INVOKABLE QVariantMap reelParams(const QVariantMap& options) const;
    Q_INVOKABLE QString makeReel(const QVariantMap& options = {});
    /// highlights.toShort's parameters for moment `rank` and `options` { layout: "auto" | "verticalSplit" |
    /// "verticalBlurFill", facecam ("c12"), facecamRegion { x, y, width, height }, split, facecamOnTop, blur, zoom,
    /// titleSeconds, captions, hookTitle, normalize }, clamped to layout.apply's ranges.
    Q_INVOKABLE QVariantMap shortParams(int rank, const QVariantMap& options) const;
    Q_INVOKABLE QString makeShort(int rank, const QVariantMap& options = {});
    /// Music for the reel: the project's audio-only media, [{ id: "m3", name }].
    Q_INVOKABLE QVariantList musicChoices() const;

signals:
    void catalogChanged();
    void jobChanged();
    void momentsChanged();
    /// The listed or the current sequence changed (sequenceName, otherSequence).
    void shownChanged();
    void selectedRankChanged();
    void previewChanged();
    void messageChanged();
    void chatChanged();
    /// The playhead moved to a moment: the timeline should keep it in view.
    void revealRequested();
    /// A reel or a short was made and is now the current sequence.
    void sequenceOpened(const QString& name);

private:
    enum class Review { Undecided, Accepted, Rejected };
    struct Moment {
        int rank = 0;
        Ticks startTicks, endTicks;
        double start = 0, end = 0, peak = 0, thumbnail = 0, duration = 0;
        double score = 0, confidence = 0;
        QString confidenceLabel;
        QString title;
        QStringList reasons;
        QVariantList signalBars;
        QVariantList waveform;
        QString image;
        Review review = Review::Undecided;
    };
    /// What the last find said about a moment that its marker does not keep (the signals, the numeric confidence).
    struct FindExtra {
        int rank = 0;
        double start = 0;
        double confidence = 0;
        QVariantList signalBars;
    };
    using ReviewKey = std::tuple<std::uint64_t, std::int64_t, std::int64_t>; ///< sequence, marker start, marker end
    using Done = std::function<void(const command::CommandResult& result, bool stale)>;

    const command::CommandRegistry& registry() const;
    /// Runs `name` on a copy of the project off the GUI thread; `done` on the GUI thread after the commit.
    QString runJob(const QString& kind, const QString& startText, const std::string& name, const QVariantMap& params,
                   bool staleIsError, Done done);
    void setJob(const QString& kind, const QString& activity, double progress);
    void setMessage(const QString& message);
    /// The moments again from the highlight markers (of the current sequence, else the one listed); cheap when nothing
    /// changed unless `force`.
    void refreshFromMarkers(bool force = false);
    void takeFindResult(const command::CommandResult& result, SequenceId sequence);
    void rebuild();
    ReviewKey keyOf(const Moment& moment) const;
    const Moment* findMoment(int rank) const;
    Moment* findMoment(int rank);
    bool showSequence(SequenceId sequence);
    void openCreated(const command::CommandResult& result);
    void onPlayhead();
    /// Waveforms whose peaks were not decoded yet, once they are.
    void refreshWaveforms();
    void transport(bool play);

    QPointer<ProjectController> m_controller;
    QPointer<PlaybackBridge> m_playback;
    const command::CommandRegistry* m_registry = nullptr;
    std::function<std::shared_ptr<command::CommandServices>()> m_servicesFactory;
    std::function<void(bool)> m_transport;
    std::unique_ptr<BridgeJob> m_job;
    bool m_synchronous = false;
    unsigned m_generation = 0;
    bool m_committing = false; ///< a job's result is being committed: its done() reads the markers
    QTimer m_peaksTimer;

    QString m_jobKind;
    QString m_activity;
    double m_progress = -1.0;

    std::vector<Moment> m_moments; ///< by rank
    SequenceId m_sequence;         ///< the sequence listed
    std::vector<Marker> m_listedMarkers;
    SequenceId m_shownListed, m_shownCurrent; ///< what shownChanged last told
    bool m_searched = false;
    std::set<std::uint64_t> m_foundSequences; ///< sequences a find ran on
    std::map<ReviewKey, Review> m_reviews;
    std::vector<FindExtra> m_extras;
    SequenceId m_findSequence; ///< the last find's sequence (m_extras, m_recordings, the chat count)
    QVariantList m_list;
    QVariantList m_recordings;
    QStringList m_warnings;
    int m_findChatMessages = 0; ///< the chat messages the last find counted (an imported log)
    int m_recordingCount = 0;
    int m_chatMessages = 0;
    int m_selectedRank = 0;
    int m_previewRank = 0;
    Ticks m_previewStart;
    Ticks m_previewEnd;
    QString m_message;
    QString m_chatFile;
    QVariantMap m_chat;
    double m_chatOffset = 0.0;
};

/// QML registration through the factory (a default-constructible class registered as its own singleton would be built
/// with its default constructor, and create() would never run).
struct HighlightsSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::HighlightsBridge)
    QML_NAMED_ELEMENT(Highlights)
    QML_SINGLETON
public:
    static HighlightsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return HighlightsBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
