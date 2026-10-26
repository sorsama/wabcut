#pragma once

// SourceTranscriptBridge (P0 round; QML singleton `SourceTranscript`): the Transcript panel's Source mode - the transcript
// of the clip loaded in the Source Monitor, so a soundbite can be read, selected as text and edited into the sequence
// with Insert (,) or Overwrite (.) (Premiere's Text panel on a source clip).
//
//   * The words: ai::edit::MediaTranscript of the Source Monitor's media item and one of its audio streams (`stream`:
//     the choice made for that item, else the first stream with a transcript, else its voice stream - Mic, Voice,
//     Commentary by the stream's name - else the first), from TranscriptBridge's transcripts (this session's
//     transcriptions and the analysis cache, which keeps them per media file and stream) with the project's
//     corrections; redacted words read "[bleep]". Shown as paragraphs (TranscriptParagraphModel) with source timecodes.
//   * A click on a word moves the Source Monitor there. A selection is one run of words (a click, Shift- or Ctrl-click
//     extends it, a drag, a Find jump) and sets the source In at the start of its first word's frame and the Out at the
//     end of its last word's frame (SourceMonitorBridge::setMarks, source.setMarks: one undo step "Set Source Marks").
//     A drag sets them once: when it ends (applyMarks) or `marksDelay` ms after the last change.
//   * Insert / Overwrite are the keymap's clip.insert / clip.overwrite (Main.qml's sourceEdit: the source marks, the
//     patching and the sequence marks, as the Source Monitor's buttons); Copy puts the words on the clipboard.
//   * Transcribe this clip: TranscriptBridge::transcribeMedia (the provider chosen in the panel, the same job with
//     progress and Cancel, consent and redactions).
// The word under the Source Monitor's position is `currentWord`.
//
// QML sees word indices and display text; never ticks or ids (principle 9).

#include "TranscriptBridge.h"

#include <wabcut/ai/edit/MediaTranscript.h>
#include <wabcut/ai/edit/TextEdits.h>
#include <wabcut/model/ProjectState.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class ProjectController;
class SourceMonitorBridge;

class SourceTranscriptBridge : public QObject {
    Q_OBJECT

    Q_PROPERTY(wabcut::app::TranscriptParagraphModel* paragraphs READ paragraphs CONSTANT FINAL)
    /// "none" (nothing in the Source Monitor), "preview" (a Media Browser file not imported yet), "noAudio" (a clip
    /// without sound) or "ready" (with or without words yet).
    Q_PROPERTY(QString state READ state NOTIFY sourceChanged FINAL)
    Q_PROPERTY(QString mediaName READ mediaName NOTIFY sourceChanged FINAL)
    /// The clip's audio streams: [{ index, label ("A2 · Mic"), hasTranscript }].
    Q_PROPERTY(QVariantList streams READ streams NOTIFY sourceChanged FINAL)
    /// The audio stream shown (an index of `streams`); remembered per media item for this session.
    Q_PROPERTY(int stream READ stream WRITE setStream NOTIFY sourceChanged FINAL)
    Q_PROPERTY(int wordCount READ wordCount NOTIFY transcriptChanged FINAL)
    Q_PROPERTY(QString language READ language NOTIFY transcriptChanged FINAL)
    /// The word under the Source Monitor's position, or -1.
    Q_PROPERTY(int currentWord READ currentWord NOTIFY currentWordChanged FINAL)
    /// Bumped whenever the selection or the search marks change (bind word delegates to it).
    Q_PROPERTY(int markRevision READ markRevision NOTIFY marksChanged FINAL)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY marksChanged FINAL)
    /// "12 words · 00:00:04:02" (or "").
    Q_PROPERTY(QString selectionInfo READ selectionInfo NOTIFY marksChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY searchChanged FINAL)
    Q_PROPERTY(int matchCount READ matchCount NOTIFY searchChanged FINAL)
    /// 0-based index of the current match, or -1.
    Q_PROPERTY(int matchIndex READ matchIndex NOTIFY searchChanged FINAL)

public:
    explicit SourceTranscriptBridge(QObject* parent = nullptr);
    ~SourceTranscriptBridge() override;
    static SourceTranscriptBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setController(ProjectController* controller);
    void setSourceMonitor(SourceMonitorBridge* source);
    void setTranscriptBridge(TranscriptBridge* transcript);
    /// How long after the last selection change the marks are set (ms); 0 = at once (tests).
    void setMarksDelay(int ms) { m_marksDelay = ms; }

    // ---- C++ ----
    const ai::edit::MediaTranscript& mediaTranscript() const { return m_words; }
    /// The selected run of words [first, last], or nullopt.
    std::optional<std::pair<std::size_t, std::size_t>> selection() const { return m_range; }

    // ---- QML ----
    TranscriptParagraphModel* paragraphs() const { return m_paragraphs; }
    QString state() const;
    QString mediaName() const;
    QVariantList streams() const { return m_streams; }
    int stream() const { return m_stream; }
    void setStream(int stream);
    int wordCount() const { return static_cast<int>(m_words.size()); }
    QString language() const;
    int currentWord() const { return m_currentWord; }
    int markRevision() const { return m_markRevision; }
    bool hasSelection() const { return m_range.has_value(); }
    QString selectionInfo() const;
    QString message() const { return m_message; }
    QString searchQuery() const { return m_query; }
    void setSearchQuery(const QString& query);
    int matchCount() const { return static_cast<int>(m_matches.size()); }
    int matchIndex() const { return m_matchIndex; }

    Q_INVOKABLE bool isSelected(int word) const;
    Q_INVOKABLE bool isMatch(int word) const;
    /// A click on a word: the Source Monitor goes to it and it becomes the selection (Shift / Ctrl extend the run from
    /// the anchor); the marks follow.
    Q_INVOKABLE void clickWord(int word, int modifiers);
    /// Selects words first..last (a drag over the text); the marks follow.
    Q_INVOKABLE void selectWords(int first, int last);
    /// Drops the selection (the source marks stay).
    Q_INVOKABLE void clearSelection();
    /// Sets the marks for the selection now if they wait (the end of a drag). `force` (Insert / Overwrite): sets them
    /// even when nothing waits, so marks changed in the Source Monitor since (or a failed earlier try) never edit in
    /// something else than the selected words; marks that are already those make no undo step. "" or why not.
    Q_INVOKABLE QString applyMarks(bool force = false);
    /// The first selected word, or -1.
    Q_INVOKABLE int selectionFirst() const;
    /// The paragraph (row of `paragraphs`) that holds word `word`, or -1.
    Q_INVOKABLE int paragraphOfWord(int word) const;
    /// The selection as its language writes it, for Find All; "" without one.
    Q_INVOKABLE QString selectionSearchText() const;
    /// Copies the selected words to the system clipboard; `withTimecodes`: "[00:00:01:02] Speaker 1: text" lines with
    /// source timecodes. "" or why not (also `message`).
    Q_INVOKABLE QString copySelection(bool withTimecodes);
    /// Find: the next / previous match becomes the selection (and the marks) and the Source Monitor goes there.
    Q_INVOKABLE void nextMatch(int direction = 1);
    /// Transcribe this clip: the shown stream alone (TranscriptBridge::transcribeMedia). "" or why not.
    Q_INVOKABLE QString transcribe(bool allowOnce = false);
    /// What a cloud provider needs before it may send this clip's audio (AiController.consentPrompt), or {}.
    Q_INVOKABLE QVariantMap consentFor() const;

signals:
    void sourceChanged();
    void transcriptChanged();
    void currentWordChanged();
    void marksChanged();
    void messageChanged();
    void searchChanged();

private:
    void rebuild();
    /// Reads the Source Monitor's clip and its words again; `force`: as new words even when they read the same.
    void refresh(bool force);
    void rebuildStreams(const MediaItem* item);
    void rebuildParagraphs();
    int defaultStream(const MediaItem& item) const;
    void updateCurrentWord();
    void search();
    void bumpMarks();
    void setMessage(const QString& message);
    void setRange(std::size_t first, std::size_t last, std::size_t anchor);
    void seekToSelection();
    QString timecodeOf(Ticks t) const;
    std::shared_ptr<ai::edit::TranscriptProvider> provider() const;

    QPointer<ProjectController> m_controller;
    QPointer<SourceMonitorBridge> m_source;
    QPointer<TranscriptBridge> m_transcript;
    TranscriptParagraphModel* m_paragraphs = nullptr;
    ai::edit::MediaTranscript m_words;
    MediaId m_media;
    int m_stream = 0;
    std::map<std::uint64_t, int> m_streamChoice; ///< per media item: the stream chosen here
    QVariantList m_streams;
    Rate m_rate{30, 1};
    int m_currentWord = -1;
    int m_markRevision = 0;
    std::optional<std::pair<std::size_t, std::size_t>> m_range;
    std::optional<std::size_t> m_anchor;
    QTimer m_marksTimer;
    bool m_marksPending = false;
    int m_marksDelay = 150;
    QString m_message;
    QString m_query;
    std::vector<ai::edit::WordRange> m_matches;
    std::set<std::size_t> m_matchWords;
    int m_matchIndex = -1;
};

/// QML registration through the factory (see ProjectControllerSingleton).
struct SourceTranscriptBridgeSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::SourceTranscriptBridge)
    QML_NAMED_ELEMENT(SourceTranscript)
    QML_SINGLETON
public:
    static SourceTranscriptBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return SourceTranscriptBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
