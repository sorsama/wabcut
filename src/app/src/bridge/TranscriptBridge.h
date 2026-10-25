#pragma once

// TranscriptBridge (M5e; QML singleton `Transcript`): edit the video by editing its text.
//
// It keeps the current sequence's SequenceTranscript (wabcut_ai_edit) - the transcripts of the media its audio clips
// play, in timeline order - up to date with every edit (SequenceTranscript::update with the edit's ChangeSet), and
// shows it as paragraphs of words (TranscriptParagraphModel: a new paragraph at a speaker change or a long pause).
// Transcripts come from this session's transcriptions and the analysis cache (CachedTranscriptProvider: what AI-2's
// TranscriptionService and earlier sessions stored, found by the media's source fingerprint).
//
// Text edits are the catalog's transcript commands through CommandEngine (wabcut_ai_edit's editingCommands() with
// TranscriptServices), so they are validated and ONE undo step each:
//   Delete (the selection)         transcript.deleteRanges   previewed first: the words strike through and the
//   Keep only (the selection)      transcript.keepRanges     timeline shows the cuts as a red ghost (dry run)
//   drag a paragraph               transcript.moveRange
//   Clean up (review items)        transcript.deleteRanges   fillers as words, silences as gaps
// Selected words light their time range on the timeline (ProjectController::setTimelineHighlights). A click on a word
// moves the playhead there; the word under the playhead is `currentWord`.
//
// Transcription: providers are "local" (whisper.cpp through TranscriptionService, when the wabcut-infer helper was
// built: WABCUT_WITH_LOCAL_AI) and the cloud transcription models AiController lists (consent and keys are the AI
// platform's). With neither, `setupNeeded` says how to set one up. A job transcribes the media of the sequence (or of
// the selected clips) that have no transcript yet, on a worker thread, with progress and cancel; results are stored in
// the analysis cache and appear in the panel when it ends.
//
// QML sees indices of words (into this sequence transcript) and display text; never ticks or ids (principle 9).

#include "ProjectController.h"

#include <wabcut/ai/Transcript.h>
#include <wabcut/ai/edit/Review.h>
#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/ai/edit/TextEdits.h>
#include <wabcut/command/Command.h>
#include <wabcut/model/ProjectState.h>

#include <QAbstractListModel>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <thread>
#include <vector>

class QJSEngine;
class QQmlEngine;
class AiController;

namespace wabcut::ai::edit {
class TranscriptServices;
class TranscriptProviderChain;
} // namespace wabcut::ai::edit

namespace wabcut::app {

/// Where to put the playhead (which is on frames, snapped down) to show a word or a cue: its start when that is on a
/// frame, else the next frame start if it is still inside the range (so the word is the current one), else its start.
Ticks seekPointIn(const TimeRange& range, Rate rate);

/// Paragraphs of the transcript: [{ speaker, timeText, first, last, words: [{ i, text }] }]. Word text carries the
/// space that follows it in space-delimited languages (none in Thai: word units sit side by side).
class TranscriptParagraphModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by Transcript.paragraphs")
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged FINAL)

public:
    enum Role { SpeakerRole = Qt::UserRole + 1, TimeRole, FirstRole, LastRole, WordsRole, SpeakerIndexRole };
    struct Paragraph {
        QString speaker, timeText;
        int speakerIndex = -1;
        int first = 0, last = -1;
        QVariantList words;
    };
    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariantMap get(int row) const;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void reset(std::vector<Paragraph> paragraphs);
    const std::vector<Paragraph>& paragraphs() const { return m_rows; }

signals:
    void countChanged();

private:
    std::vector<Paragraph> m_rows;
};

class TranscriptBridge : public QObject {
    Q_OBJECT

    Q_PROPERTY(wabcut::app::TranscriptParagraphModel* paragraphs READ paragraphs CONSTANT FINAL)
    Q_PROPERTY(int wordCount READ wordCount NOTIFY transcriptChanged FINAL)
    Q_PROPERTY(QString language READ language NOTIFY transcriptChanged FINAL)
    /// The word under the playhead, or -1.
    Q_PROPERTY(int currentWord READ currentWord NOTIFY currentWordChanged FINAL)
    /// Bumped whenever the selection, the preview or the search marks change (bind word delegates to it).
    Q_PROPERTY(int markRevision READ markRevision NOTIFY marksChanged FINAL)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY marksChanged FINAL)
    /// "12 words · 00:00:04:02" (or "").
    Q_PROPERTY(QString selectionInfo READ selectionInfo NOTIFY marksChanged FINAL)
    /// A text edit is previewed (struck-through words, red ghost on the timeline) and waits for apply/cancel.
    Q_PROPERTY(bool previewing READ previewing NOTIFY marksChanged FINAL)
    /// "Removes 00:00:04:02 in 2 cuts" for the pending edit.
    Q_PROPERTY(QString previewInfo READ previewInfo NOTIFY marksChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

    // ---- Search ----
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY searchChanged FINAL)
    Q_PROPERTY(int matchCount READ matchCount NOTIFY searchChanged FINAL)
    /// 0-based index of the current match, or -1.
    Q_PROPERTY(int matchIndex READ matchIndex NOTIFY searchChanged FINAL)

    // ---- Clean-up review ----
    /// [{ index, kind: "filler"|"silence", label, context, durationText, confidence, remove }]
    Q_PROPERTY(QVariantList review READ review NOTIFY reviewChanged FINAL)
    Q_PROPERTY(bool reviewBusy READ reviewBusy NOTIFY reviewProgressChanged FINAL)
    Q_PROPERTY(double reviewProgress READ reviewProgress NOTIFY reviewProgressChanged FINAL)
    Q_PROPERTY(int reviewRemoveCount READ reviewRemoveCount NOTIFY reviewChanged FINAL)
    Q_PROPERTY(bool reviewFillers READ reviewFillers WRITE setReviewFillers NOTIFY reviewOptionsChanged FINAL)
    /// Pauses at least this long are silences; 0 = do not look for silences.
    Q_PROPERTY(double silenceSeconds READ silenceSeconds WRITE setSilenceSeconds NOTIFY reviewOptionsChanged FINAL)
    /// Padding kept around cut words, in milliseconds.
    Q_PROPERTY(int paddingMs READ paddingMs WRITE setPaddingMs NOTIFY reviewOptionsChanged FINAL)

    // ---- Transcription ----
    /// [{ id, label, detail, available, reason }]: "local", then the cloud transcription models.
    Q_PROPERTY(QVariantList providers READ providers NOTIFY providersChanged FINAL)
    Q_PROPERTY(QString provider READ provider WRITE setProvider NOTIFY providersChanged FINAL)
    /// No provider can transcribe: the panel shows how to set one up.
    Q_PROPERTY(bool setupNeeded READ setupNeeded NOTIFY providersChanged FINAL)
    /// "idle", "running", "done", "error".
    Q_PROPERTY(QString jobStatus READ jobStatus NOTIFY jobChanged FINAL)
    Q_PROPERTY(double jobProgress READ jobProgress NOTIFY jobChanged FINAL)
    Q_PROPERTY(QString jobText READ jobText NOTIFY jobChanged FINAL)
    /// 0.4: the sequence's audio tracks for the Tracks checklist: [{ index, label ("A2 · Mic"), name, role
    /// ("dialogue", "music", ... or ""), muted, chosen }]. Only chosen tracks are transcribed.
    Q_PROPERTY(QVariantList audioTracks READ audioTracks NOTIFY audioTracksChanged FINAL)

public:
    /// A transcriber: media file + container stream -> transcript (worker thread; `progress` takes 0..1).
    using Transcriber = std::function<std::expected<ai::Transcript, Error>(
        const std::string& path, int containerStream, const std::function<void(float)>& progress, std::stop_token stop)>;

    explicit TranscriptBridge(QObject* parent = nullptr);
    ~TranscriptBridge() override;
    static TranscriptBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setController(ProjectController* controller);
    void setAiController(AiController* ai);
    ProjectController* controller() const { return m_controller; }

    // ---- C++ ----
    /// Adds a transcript of `media`'s audio stream (index into MediaInfo::audio) for this session.
    void addTranscript(MediaId media, int audioStream, std::shared_ptr<const ai::Transcript> transcript);
    /// Uses only the transcripts given with addTranscript (tests: no analysis cache).
    void setUseAnalysisCache(bool use);
    /// Replaces a provider's transcriber (tests; "fake" adds a provider).
    void setTranscriber(const QString& providerId, const QString& label, Transcriber transcriber);
    const ai::edit::SequenceTranscript& sequenceTranscript() const { return m_transcript; }
    std::shared_ptr<ai::edit::TranscriptServices> services() const { return m_services; }
    /// Runs a command of the editing catalog (transcript and caption commands) on the controller's session.
    command::CommandResult run(const std::string& name, command::Json params, bool dryRun = false);

    // ---- QML ----
    TranscriptParagraphModel* paragraphs() const { return m_paragraphs; }
    int wordCount() const { return static_cast<int>(m_transcript.size()); }
    QString language() const;
    int currentWord() const { return m_currentWord; }
    int markRevision() const { return m_markRevision; }
    bool hasSelection() const { return !m_selection.empty(); }
    QString selectionInfo() const;
    bool previewing() const { return m_pending.has_value(); }
    QString previewInfo() const { return m_pending ? m_pending->info : QString(); }
    QString message() const { return m_message; }
    QString searchQuery() const { return m_query; }
    void setSearchQuery(const QString& query);
    int matchCount() const { return static_cast<int>(m_matches.size()); }
    int matchIndex() const { return m_matchIndex; }
    QVariantList review() const;
    int reviewRemoveCount() const;
    bool reviewFillers() const { return m_reviewFillers; }
    void setReviewFillers(bool on);
    double silenceSeconds() const { return m_silenceSeconds; }
    void setSilenceSeconds(double seconds);
    int paddingMs() const { return m_paddingMs; }
    void setPaddingMs(int ms);
    QVariantList providers() const;
    QString provider() const { return m_provider; }
    void setProvider(const QString& id);
    bool setupNeeded() const;
    QString jobStatus() const { return m_jobStatus; }
    double jobProgress() const { return m_jobProgress; }
    QString jobText() const { return m_jobText; }
    QVariantList audioTracks() const { return m_audioTracks; }

    Q_INVOKABLE bool isSelected(int word) const;
    Q_INVOKABLE bool isRemoving(int word) const;
    Q_INVOKABLE bool isMatch(int word) const;
    /// A click on a word: the playhead goes to it; it becomes the selection (Shift extends from the anchor).
    Q_INVOKABLE void clickWord(int word, int modifiers);
    /// Selects words first..last (a drag over the text).
    Q_INVOKABLE void selectWords(int first, int last);
    Q_INVOKABLE void clearSelection();
    /// Previews deleting the selection (transcript.deleteRanges, dry run); applyPreview() runs it. "" or an error.
    Q_INVOKABLE QString previewDelete();
    /// Previews keeping only the selection (transcript.keepRanges).
    Q_INVOKABLE QString previewKeep();
    /// Previews removing the review items marked "remove".
    Q_INVOKABLE QString previewReview();
    /// Runs the previewed edit: one undo step. "" or an error.
    Q_INVOKABLE QString applyPreview();
    Q_INVOKABLE void cancelPreview();
    /// The selected words' text (one range), for the Fix field.
    Q_INVOKABLE QString selectedText() const;
    /// 0.4 Find All: the first selected run as its language writes it (Thai without spaces), the way the search
    /// matches the shown text; "" without a selection.
    Q_INVOKABLE QString selectionSearchText() const;
    /// C.7 corrections: the selected words read `text` from now on (transcript.correctWord / correctRange, saved with
    /// the project over the cached transcript; captions follow on "Update from transcript"). "" or why not.
    Q_INVOKABLE QString correctSelection(const QString& text);
    /// Moves paragraph `paragraph` before paragraph `before` (== count: after the last) (transcript.moveRange).
    Q_INVOKABLE QString moveParagraph(int paragraph, int before);
    /// Search: the next/previous match becomes the selection and the playhead goes there; selectAllMatches() selects
    /// every match.
    Q_INVOKABLE void nextMatch(int direction = 1);
    Q_INVOKABLE void selectAllMatches();
    /// Finds fillers and silences with the current options.
    Q_INVOKABLE void refreshReview();
    Q_INVOKABLE void cancelReview();
    bool reviewBusy() const { return m_reviewBusy; }
    double reviewProgress() const { return m_reviewProgress; }
    using ReviewRunner = std::function<std::vector<ai::edit::ReviewItem>(const ai::edit::SequenceTranscript&,
        const ProjectState&, const ai::edit::ReviewOptions&, std::stop_token, const std::function<void(float)>&)>;
    void setReviewRunner(ReviewRunner runner) { m_reviewRunner = std::move(runner); }
    Q_INVOKABLE void setReviewRemove(int item, bool remove);
    Q_INVOKABLE void setAllReviewRemove(bool remove);
    /// Starts a transcription of the sequence ("sequence") or the selected clips ("selection") with `provider`.
    /// Returns "" or why not. A cloud provider needs recorded consent, or `allowOnce`: true only when the user just
    /// answered the consent prompt (Allow once / Always). Without either nothing is uploaded.
    Q_INVOKABLE QString transcribe(const QString& scope, bool allowOnce = false);
    Q_INVOKABLE void cancelTranscription();
    /// What a cloud provider needs before it may send audio: AiController.consentPrompt(...) for the job, or {}.
    Q_INVOKABLE QVariantMap consentFor(const QString& scope) const;

    // ---- 0.4: follow the playhead, the word menu, voice tracks ----
    /// The paragraph (row of `paragraphs`) that holds word `word`, or -1.
    Q_INVOKABLE int paragraphOfWord(int word) const;
    /// The first selected word, or -1.
    Q_INVOKABLE int selectionFirst() const;
    /// Copies the selected words to the system clipboard as plain text (separate runs on separate lines);
    /// `withTimecodes`: one "[00:01:02:03] Speaker: text" line per run and paragraph. "" or why not (also `message`).
    Q_INVOKABLE QString copySelection(bool withTimecodes);
    /// Sequence In at the start of the first selected word's frame, Out at the end of the last one's: one undo step
    /// "Mark In and Out". "" or why not.
    Q_INVOKABLE QString markSelection();
    /// A range marker over the selected span named from its words (`chapter`: a chapter point marker), one undo step
    /// "Add Marker". "" or why not.
    Q_INVOKABLE QString addMarkerFromSelection(bool chapter);
    /// Lifts the selected span (first to last word) from the tracks its words are heard on and their linked partners'
    /// tracks, leaving a gap; clears In/Out and parks the playhead at the gap. One undo step "Lift". "" or why not.
    Q_INVOKABLE QString liftSelection();
    /// Chooses whether audio track `index` (0 = A1) is transcribed; remembered by track name for every project.
    Q_INVOKABLE void setTrackChosen(int index, bool chosen);

signals:
    void reviewProgressChanged();
    void transcriptChanged();
    /// A correction was applied (CaptionsBridge offers "Update from transcript").
    void corrected();
    void currentWordChanged();
    void marksChanged();
    void messageChanged();
    void searchChanged();
    void reviewChanged();
    void reviewOptionsChanged();
    void providersChanged();
    void jobChanged();
    void audioTracksChanged();

private:
    struct Pending {
        std::string command;
        command::Json params;
        std::set<std::size_t> words; ///< struck through
        QString info;
        const char* label = "";
    };
    struct JobItem {
        MediaId media;
        int audioStream = 0;
        int containerStream = -1;
        std::string path;
    };

    void rebuild();
    void onModelChanged();
    void updateCurrentWord();
    void rebuildParagraphs();
    void setMessage(const QString& message);
    void bumpMarks();
    void updateHighlights();
    void setPending(std::optional<Pending> pending, const ai::edit::EditPlan* plan);
    std::vector<ai::edit::WordRange> selectionRanges() const;
    void search();
    void seekToWord(std::size_t word);
    ai::edit::CutOptions cutOptions() const;
    std::vector<JobItem> jobItems(const QString& scope) const;
    void finishJob(std::vector<std::pair<JobItem, std::expected<ai::Transcript, Error>>> results, bool cancelled);
    /// 0.4: whether audio track `index` is transcribed: the user's choice for its name, else the default (Dialogue role
    /// or a Mic / Voice / Commentary name, unmuted, not voice chat; every unmuted track when none matches).
    bool trackChosen(const Track& track, int index) const;
    /// Rebuilds audioTracks from the current sequence; emits audioTracksChanged only when it differs.
    void rebuildAudioTracks();
    /// The first..last selected words' span on the timeline (frame-snapped outward), or nullopt.
    std::optional<TimeRange> selectionSpan() const;

    QPointer<ProjectController> m_controller;
    QPointer<AiController> m_ai;
    TranscriptParagraphModel* m_paragraphs = nullptr;
    std::shared_ptr<ai::edit::MapTranscriptProvider> m_session;
    std::shared_ptr<ai::edit::TranscriptProviderChain> m_chain;
    std::shared_ptr<ai::edit::TranscriptServices> m_services;
    bool m_useCache = true;
    ai::edit::SequenceTranscript m_transcript;
    std::shared_ptr<const ProjectState> m_mapped; ///< the state m_transcript describes
    int m_currentWord = -1;
    int m_markRevision = 0;
    std::set<std::size_t> m_selection;
    std::optional<std::size_t> m_anchor;
    std::optional<Pending> m_pending;
    std::vector<TimelineHighlight> m_pendingCuts; ///< the dry run's cuts, drawn as the timeline ghost
    QString m_message;
    QString m_query;
    std::vector<ai::edit::WordRange> m_matches;
    std::set<std::size_t> m_matchWords;
    int m_matchIndex = -1;
    std::vector<ai::edit::ReviewItem> m_review;
    struct ReviewWorker {
        std::shared_ptr<std::atomic<bool>> finished;
        std::jthread thread;
    };
    std::vector<ReviewWorker> m_reviewWorkers;
    ReviewRunner m_reviewRunner;
    std::uint64_t m_reviewGeneration = 0;
    bool m_reviewBusy = false;
    double m_reviewProgress = 0;
    std::vector<bool> m_reviewRemove;
    bool m_reviewFillers = true;
    double m_silenceSeconds = 0.8;
    int m_paddingMs = 40;
    QString m_provider = QStringLiteral("local");
    std::map<QString, std::pair<QString, Transcriber>> m_fakeTranscribers;
    QString m_jobStatus = QStringLiteral("idle");
    double m_jobProgress = 0.0;
    QString m_jobText;
    std::jthread m_job;
    std::atomic<int> m_jobGeneration{0};
    QVariantList m_audioTracks;
};

/// QML registration through the factory (see ProjectControllerSingleton).
struct TranscriptBridgeSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::TranscriptBridge)
    QML_NAMED_ELEMENT(Transcript)
    QML_SINGLETON
public:
    static TranscriptBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return TranscriptBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
