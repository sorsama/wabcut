#pragma once

// CaptionsBridge (M5e, the C.7 caption workshop in M5m; QML singleton `Captions`): caption tracks of the current
// sequence (Sequence::captionTracks) and their style, which lives in the model (CaptionStyle, docs/internal/c7-caption-handoff.md).
//
//   tracks / track         the caption tracks; the one shown, edited and drawn on the program monitor
//   style / setStyle()     the shown track's CaptionStyle as a map; a change is edit::setCaptionStyle, and a slider
//                          drag merges into one undo step until endStyleEdit()
//   presets                built in (Clean, Bold Box, Podcast, Shorts, Thai-friendly) and the project's; apply, save
//                          the track's style as a project preset, delete one
//   cues / cue edits       text and timing (typing merges until endEdit()), nudge by frames, split at the playhead,
//                          merge with the next, add at the playhead, delete, the speaker (name and colour)
//   generate() / update    captions.generateFromTranscript; captions.updateFromTranscript after transcript corrections
//                          (`updateSuggested` after a fix on the Transcript tab)
//   exportFile()           SRT / WebVTT through wabcut_ai_edit's writers
//   activeCue              the cue under the playhead (the list highlights it)
//   selectedCue            0.5: the cue selected on the timeline's caption lanes (TimelineView; selectCue), editCue()
//                          asks the page to open its text, setTrackEnabledFor() is the lanes' eye toggle
//
// The compositor draws the shown track in the picture (I.4: the program monitor and burned-in exports alike) with
// `style` (CaptionStyleMapping.h maps it to the renderer's style), so every edit previews live.

#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <wabcut/model/CaptionStyle.h>
#include <wabcut/model/Ids.h>

#include <string>

class QJSEngine;
class QQmlEngine;

namespace wabcut {
struct CaptionTrack;
}

namespace wabcut::app {

class ProjectController;
class TranscriptBridge;

/// The style as QML sees it: colours "#RRGGBB", enums as strings, sizes as fractions (fontSize of the frame height,
/// em-relative outline/shadow/box sizes), plus fontSizePx (on a 1080-line frame) and positionAnimated.
QVariantMap captionStyleMap(const CaptionStyle& style, Ticks at = {});
/// Applies one field of captionStyleMap to `style` (clamped to the command's ranges). False for an unknown key or a
/// value of the wrong kind.
bool setCaptionStyleField(CaptionStyle& style, const QString& key, const QVariant& value);

class CaptionsBridge : public QObject {
    Q_OBJECT

    /// [{ id: "k…", name, language, cueCount, enabled }]
    Q_PROPERTY(QVariantList tracks READ tracks NOTIFY tracksChanged FINAL)
    /// The track shown and edited ("" when there is none).
    Q_PROPERTY(QString track READ track WRITE setTrack NOTIFY tracksChanged FINAL)
    /// The shown track's name, language, visibility and Track Style: { name, language, enabled, trackStyle } (empty
    /// without a track).
    Q_PROPERTY(QVariantMap trackInfo READ trackInfo NOTIFY tracksChanged FINAL)
    /// Cues of `track`: [{ index, text, startText, endText, durationText, start, end (seconds), speaker ("v…" or ""),
    /// speakerName, speakerColor, cps (characters a second), fast (above the language's reading speed) }]
    Q_PROPERTY(QVariantList cues READ cues NOTIFY cuesChanged FINAL)
    /// The cue under the playhead (-1), highlighted in the list.
    Q_PROPERTY(int activeCue READ activeCue NOTIFY activeChanged FINAL)
    /// The transcript has words to make captions from.
    Q_PROPERTY(bool canGenerate READ canGenerate NOTIFY canGenerateChanged FINAL)
    /// The shown track's style (captionStyleMap); the default look without a track.
    Q_PROPERTY(QVariantMap style READ style NOTIFY styleChanged FINAL)
    /// [{ name, builtin, current (the shown track looks like it), fill, box, boxColor, outlineWidth, fontWeight }]
    Q_PROPERTY(QVariantList presets READ presets NOTIFY styleChanged FINAL)
    /// The project's speakers: [{ key: "v…", name, color: "#RRGGBB" }]
    Q_PROPERTY(QVariantList speakers READ speakers NOTIFY tracksChanged FINAL)
    /// The transcript was corrected since the cues were made or updated: "Update from transcript" is worth a click.
    Q_PROPERTY(bool updateSuggested READ updateSuggested NOTIFY updateSuggestedChanged FINAL)
    /// Show the captions on the program monitor (the compositor draws them; Playback follows this).
    Q_PROPERTY(bool overlay READ overlay WRITE setOverlay NOTIFY styleChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)
    /// p02 audio: Premiere's "Hide profanity": captions generated or updated from the transcript show profane words as
    /// "f***". With the extra words (comma-separated; a trailing * matches words that start with it) and the allowed
    /// ones, it is kept in the settings.
    Q_PROPERTY(bool hideProfanity READ hideProfanity WRITE setHideProfanity NOTIFY profanityChanged FINAL)
    Q_PROPERTY(QString profanityWords READ profanityWords WRITE setProfanityWords NOTIFY profanityChanged FINAL)
    Q_PROPERTY(QString allowedWords READ allowedWords WRITE setAllowedWords NOTIFY profanityChanged FINAL)
    /// 0.5: the cue selected on the timeline's caption lanes (an index into `cues` of the shown track), or -1. The cue
    /// list highlights it and scrolls to it; selecting clips, a gap or a transition on the timeline clears it.
    Q_PROPERTY(int selectedCue READ selectedCue NOTIFY cueSelectionChanged FINAL)

public:
    explicit CaptionsBridge(QObject* parent = nullptr);
    static CaptionsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setController(ProjectController* controller);
    void setTranscript(TranscriptBridge* transcript);

    QVariantList tracks() const;
    QString track() const;
    void setTrack(const QString& id);
    QVariantMap trackInfo() const;
    QVariantList cues() const;
    int activeCue() const { return m_activeCue; }
    bool canGenerate() const;
    QVariantMap style() const;
    QVariantList presets() const;
    QVariantList speakers() const;
    bool updateSuggested() const { return m_updateSuggested; }
    bool overlay() const { return m_overlay; }
    void setOverlay(bool on);
    QString message() const { return m_message; }
    bool hideProfanity() const { return m_hideProfanity; }
    void setHideProfanity(bool on);
    QString profanityWords() const { return m_profanityWords; }
    void setProfanityWords(const QString& words);
    QString allowedWords() const { return m_allowedWords; }
    void setAllowedWords(const QString& words);

    // ---- Profanity (p02 audio) ----
    /// Masks the profane words of the shown track's cues now (captions.hideProfanity).
    Q_INVOKABLE QString hideProfanityNow();
    /// Bleeps ("bleep") or mutes ("mute") the profane words in the sound (audio.bleepProfanity: audio redactions; the
    /// picture is untouched). "" or why not.
    Q_INVOKABLE QString bleepProfanity(const QString& mode);

    // ---- Tracks ----
    /// Makes captions from the transcript (captions.generateFromTranscript with its speakers), replacing the shown
    /// track. "" or why not.
    Q_INVOKABLE QString generate(int maxCharsPerLine = 42, int maxLines = 2);
    /// 0.2: Find and Replace in the shown caption track or every caption track (captions.findReplace), or in the
    /// transcript (transcript.findReplace); one undo step. "" or why not; message() says how many were replaced.
    Q_INVOKABLE QString findReplace(const QString& find, const QString& replace, bool matchCase, bool wholeWord,
                                    const QString& scope);
    /// 0.2: Find (the arrows of Find and Replace): the matches in `scope` ("track", "all" or "transcript") in order -
    /// captions: [{ captionTrack, cue, start, end (UTF-16 of the cue's text), time, text }]; the transcript:
    /// [{ words: [first, last], time, text }] (captions.find, transcript.find).
    Q_INVOKABLE QVariantList findMatches(const QString& find, bool matchCase, bool wholeWord, const QString& scope);
    /// 0.2: Replace - one match findMatches() gave (captions.findReplace "at", transcript.findReplace "words"); one
    /// undo step. "" or why not.
    Q_INVOKABLE QString replaceMatch(const QString& find, const QString& replace, bool matchCase, bool wholeWord,
                                     const QString& scope, const QVariantMap& match);
    /// 0.2 spell check (Spelling): the shown track's misspelled words in its language, with suggestions:
    /// [{ cue, start, end (UTF-16 of the cue's text), word, action, suggestions }].
    Q_INVOKABLE QVariantList spellingIssues();
    /// Check Spelling's Change: the cue's text with [start, end) (UTF-16) replaced by `with` ("" deletes a repeated
    /// word and a space); one undo step.
    Q_INVOKABLE QString changeWord(int cue, int start, int end, const QString& with);
    /// Check Spelling's Change All: every whole-word `word` (same case) of the shown track becomes `with`
    /// (captions.findReplace); one undo step.
    Q_INVOKABLE QString changeAll(const QString& word, const QString& with);
    /// 0.2: a caption track from an .srt, .vtt or timed .txt file (captions.import); it becomes the shown track.
    Q_INVOKABLE QString importFile(const QUrl& file);
    /// 0.2: the transcript as a .txt or .csv file (transcript.export).
    Q_INVOKABLE QString exportTranscript(const QUrl& file);
    /// Rewrites the cues' text from the corrected transcript, keeping their timing (captions.updateFromTranscript).
    Q_INVOKABLE QString updateFromTranscript();
    /// A new empty caption track styled by `preset` ("" = the default look); it becomes the shown track.
    Q_INVOKABLE QString addTrack(const QString& name, const QString& preset = {});
    Q_INVOKABLE QString setTrackName(const QString& name);
    Q_INVOKABLE QString setTrackLanguage(const QString& language);
    Q_INVOKABLE QString setTrackEnabled(bool enabled);
    Q_INVOKABLE QString removeTrack();

    // ---- Style and presets ----
    /// One field of the shown track's style (keys of `style`); a drag of the same field merges until endStyleEdit().
    Q_INVOKABLE QString setStyle(const QString& key, const QVariant& value);
    Q_INVOKABLE void endStyleEdit();
    /// The style of a preset, built in or the project's (captions.applyPreset).
    Q_INVOKABLE QString applyPreset(const QString& name);
    /// Saves the shown track's style as a project preset (replacing one of that name; built-in names are refused).
    Q_INVOKABLE QString savePreset(const QString& name);
    Q_INVOKABLE QString removePreset(const QString& name);
    /// 0.2: Premiere's Track Style - the shown track follows a caption preset ("" = none; captions.setTrackStyle), so
    /// saving the preset again restyles it; Push to Track Style makes the track's look the preset's
    /// (captions.pushTrackStyle) and restyles every track that follows it. One undo step each.
    Q_INVOKABLE QString setTrackStyle(const QString& preset);
    Q_INVOKABLE QString pushTrackStyle();

    // ---- Cues ----
    Q_INVOKABLE QString setCueText(int index, const QString& text);
    /// New start and end (timecode); kept between the neighbouring cues unless the style allows overlaps.
    Q_INVOKABLE QString setCueTiming(int index, const QString& start, const QString& end);
    /// Moves a cue's "start", "end" or "both" by whole frames (negative: earlier), kept between its neighbours.
    Q_INVOKABLE QString nudgeCue(int index, const QString& edge, int frames);
    /// Cuts the cue in two at the playhead (it must be strictly inside the cue).
    Q_INVOKABLE QString splitCueAtPlayhead(int index);
    /// Joins the cue with the next one.
    Q_INVOKABLE QString mergeWithNext(int index);
    /// A new cue at the playhead (two seconds, or up to the next cue) with `text`.
    Q_INVOKABLE QString addCueAtPlayhead(const QString& text);
    Q_INVOKABLE QString removeCue(int index);
    /// The cue's speaker ("v…"), or "" for none.
    Q_INVOKABLE QString setCueSpeaker(int index, const QString& speaker);
    /// A new project speaker; returns its key ("" when refused, the reason in `message`).
    Q_INVOKABLE QString addSpeaker(const QString& name);
    /// Renames a speaker and/or changes its colour ("#RRGGBB"; "" keeps it).
    Q_INVOKABLE QString renameSpeaker(const QString& speaker, const QString& name, const QString& color = {});
    /// Closes a typing undo step.
    Q_INVOKABLE void endEdit();
    Q_INVOKABLE void seekToCue(int index);
    /// 0.4 Shift Captions…: moves the shown track's cues by `delta` - signed frames ("-12") or a signed timecode in the
    /// sequence rate ("+00:00:00:12", "-1:12") - for `scope` "all" or "fromPlayhead" (the cue under the playhead, else
    /// the first one after it, and every later one) (captions.shiftCues). One undo step "Shift Captions"; refused with
    /// nothing changed when a cue would start before 0 or overlap. "" or why not (also `message`).
    Q_INVOKABLE QString shiftCues(const QString& delta, const QString& scope);

    // ---- 0.5: the timeline's caption lanes ----
    int selectedCue() const { return m_selectedCue; }
    /// Shows caption track `track` ("k…") and selects its cue `index` (-1: none).
    Q_INVOKABLE void selectCue(const QString& track, int index);
    void selectCueOf(CaptionTrackId track, int index);
    Q_INVOKABLE void clearCueSelection();
    /// Asks the Captions page to open cue `index`'s text for typing (a double-click on the cue in the timeline).
    Q_INVOKABLE void editCue(int index);
    /// Asks the Captions page to show cue `index` (the cue menu's Select in Captions): the list scrolls to it.
    Q_INVOKABLE void showCue(int index);
    /// Shows or hides caption track `track` ("k…"; the eye of its timeline header): captions.setTrack, one undo step.
    /// "" or why not.
    Q_INVOKABLE QString setTrackEnabledFor(const QString& track, bool enabled);

    // ---- C++ ----
    /// The track the Captions page shows (invalid when there is none): the command palette's captionTrack.
    CaptionTrackId shownTrackId() const { return m_track; }

    // ---- Export ----
    /// "srt" or "vtt" text of the shown track.
    Q_INVOKABLE QString exportText(const QString& format) const;
    /// Writes the shown track to `file` (UTF-8). "" or the error.
    Q_INVOKABLE QString exportFile(const QUrl& file, const QString& format);

signals:
    void tracksChanged();
    void cuesChanged();
    void activeChanged();
    void canGenerateChanged();
    void styleChanged();
    void messageChanged();
    void updateSuggestedChanged();
    void profanityChanged();
    void cueSelectionChanged();
    /// 0.5: editCue(): the Captions page opens the cue's text.
    void editCueRequested(int index);
    /// 0.5: showCue(): the Captions page shows the cue.
    void showCueRequested(int index);

private:
    const CaptionTrack* shownTrack() const;
    void refresh();
    void updateActive();
    void setMessage(const QString& message);
    void setUpdateSuggested(bool suggested);
    /// Runs a caption command on the shown track's sequence (one undo step). "" or the reason.
    QString runCommand(const std::string& name, const QVariantMap& params);
    /// Applies a model edit to the shown track (one undo step, merged by `mergeKey`). "" or the reason.
    template <class Fn>
    QString editTrack(const char* label, Fn&& fn, const std::string& mergeKey = {});

    QPointer<ProjectController> m_controller;
    QPointer<TranscriptBridge> m_transcript;
    CaptionTrackId m_track;
    int m_activeCue = -1;
    int m_selectedCue = -1; // 0.5
    void setSelectedCue(int index);
    bool m_overlay = true;
    bool m_updateSuggested = false;
    QString m_message;
    bool m_hideProfanity = false;
    QString m_profanityWords;
    QString m_allowedWords;
};

struct CaptionsBridgeSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::CaptionsBridge)
    QML_NAMED_ELEMENT(Captions)
    QML_SINGLETON
public:
    static CaptionsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return CaptionsBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
