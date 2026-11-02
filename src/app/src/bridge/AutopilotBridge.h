#pragma once

// The creator autopilot in the app (P1 #62, docs/creator-autopilot.md): the `Autopilot` QML singleton behind the
// Creator Autopilot sheet (Sequence > Creator Autopilot...). Three jobs over the autopilot.* catalog commands, each run
// on a worker (CreatorBase: progress, Stop) on a scratch copy of the project; nothing changes before Apply, and Apply is
// one undo step guarded by the revision the preview was made at.
//
//   B-roll          previewBroll(options) runs autopilot.broll as a dry run: the suggestions (drawn as the timeline
//                   ghost) and the lines nothing matched. setBrollChosen() switches one off or on (the staged edit is
//                   made again from the chosen ones); applyBroll() commits "Auto B-roll". generateFor(line) hands a
//                   line nothing matched to the Generate panel (its prompt and time; the cost and consent are there).
//   Publishing kit  makeKit(options) previews the title ideas, description, chapters, tags and thumbnail candidates;
//                   writeKit(folder, edits) writes them as files (nothing is uploaded). With options.cloud the user's
//                   chat model writes the copy from the transcript text: kitQuote() first says what it would cost and
//                   whether consent is needed.
//   Pacing          analysePacing(reference, options): the reference video's cut rhythm against the sequence's and the
//                   suggested cuts (staged as markers for the ghost); setPacingChosen(); applyPacing("markers" | "cuts")
//                   commits "Pacing Match".

#include "CreatorBridge.h"

#include <wabcut/ai/autopilot/Autopilot.h>

#include <QUrl>

namespace wabcut::app {

class AutopilotBridge : public CreatorBase {
    Q_OBJECT
    /// {lines, candidates, visual, placed, track: {name, label, created}, suggestions: [{index, line, text, keywords,
    ///  media, mediaName, at, atTicks, duration, durationTicks, sourceIn, sourceInTicks, timecode, score, how, evidence,
    ///  reason, chosen, thumb}], unmatched: [{line, text, keywords, at, duration, timecode, prompt}], done (applied)};
    ///  empty before a preview.
    Q_PROPERTY(QVariantMap broll READ broll NOTIFY resultChanged FINAL)
    /// {name, title, titles, description, lead, chapters: [{seconds, time, title}], chaptersText, chaptersFrom, tags,
    ///  thumbnails: [{seconds, time, timecode, score, reasons, thumb, file}], cloud, written, folder, files}.
    Q_PROPERTY(QVariantMap kit READ kit NOTIFY resultChanged FINAL)
    /// {reference: {name, length, cuts, medianShot, averageShot, cutsPerMinute, curve, shots}, edit: {...}, after: {...},
    ///  suggestions: [{index, seconds, timecode, kind, reason, target, shot, chosen}], fasterShots, done (applied)}.
    Q_PROPERTY(QVariantMap pacing READ pacing NOTIFY resultChanged FINAL)
    /// What the staged preview is: "" | "broll" | "pacing".
    Q_PROPERTY(QString stagedKind READ stagedKind NOTIFY resultChanged FINAL)
    /// The user's cloud chat model can write the kit's copy (a model is set up, AI is online, the project is not no-cloud).
    Q_PROPERTY(bool cloudAvailable READ cloudAvailable NOTIFY resultChanged FINAL)
    /// The folder the kit is written into (remembered; the Videos folder at first).
    Q_PROPERTY(QUrl kitFolder READ kitFolder NOTIFY resultChanged FINAL)
    /// Fix round 1 (ARGON-17): the B-roll or pacing preview no longer fits the timeline (it was edited while the job ran
    /// or since the preview, or Apply was refused for it): rerun() makes it again on the timeline as it is now. Fix round
    /// 3: the publishing kit too (its chapters and thumbnails); writeKit() refuses an outdated kit.
    Q_PROPERTY(bool outdated READ outdated NOTIFY outdatedChanged FINAL)

public:
    static AutopilotBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    explicit AutopilotBridge(QObject* parent = nullptr);

    QVariantMap broll() const { return m_broll; }
    QVariantMap kit() const { return m_kit; }
    QVariantMap pacing() const { return m_pacing; }
    QString stagedKind() const { return staged() ? m_stagedKind : QString(); }
    bool cloudAvailable() const;
    QUrl kitFolder() const;
    bool outdated() const { return m_outdatedShown; }
    /// Fix round 1 (ARGON-17): the last B-roll preview, publishing kit or pacing analysis again, with the same options, on
    /// the timeline as it is now (the suggestions are made afresh). "" once started, else why not.
    Q_INVOKABLE QString rerun();

    /// options: perMinute, minSeconds, maxSeconds, visual (bool).
    Q_INVOKABLE QString previewBroll(const QVariantMap& options = {});
    /// Suggestion `index` (1-based) on or off; the staged edit is made again from the chosen ones.
    Q_INVOKABLE QString setBrollChosen(int index, bool chosen);
    Q_INVOKABLE QString applyBroll();
    /// Opens the Generate panel for unmatched line `line` (1-based, as `broll.unmatched` numbers them): emits
    /// generateRequested with the proposal; Main shows the panel, then calls proposeGeneration().
    Q_INVOKABLE QString generateFor(int line);
    Q_INVOKABLE void proposeGeneration(const QVariantMap& proposal);

    /// options: thumbnails, chapters ("auto" | "markers" | "topics" | "none"), cloud (bool), allowOnce (bool).
    Q_INVOKABLE QString makeKit(const QVariantMap& options = {});
    /// What "Write with my AI" would send and cost: {available, model, provider, cost, needed (consent), payload, prompt
    /// (the consent prompt for AiConsentDialog)}.
    Q_INVOKABLE QVariantMap kitQuote() const;
    /// Writes the kit (the last preview, with the user's edits: title, description, tags) into `folder`.
    Q_INVOKABLE QString writeKit(const QUrl& folder, const QVariantMap& edits = {});
    /// Shows the written kit's folder in the file manager.
    Q_INVOKABLE void revealKit() const;

    /// The project's videos that can be a pacing reference: [{key, name, duration}].
    Q_INVOKABLE QVariantList referenceVideos() const;
    /// options: strength (0..1), tolerance.
    Q_INVOKABLE QString analysePacing(const QString& reference, const QVariantMap& options = {});
    Q_INVOKABLE QString setPacingChosen(int index, bool chosen);
    /// "markers" | "cuts".
    Q_INVOKABLE QString applyPacing(const QString& mode);

    void discard() override;

signals:
    void generateRequested(const QVariantMap& proposal);
    void outdatedChanged();

private:
    std::shared_ptr<command::CommandServices> autopilotServices(std::shared_ptr<ai::IChatModel> chat = nullptr);
    QString stageBroll(const command::Json& params, const QString& phase, bool fresh);
    QString stagePacing(const command::Json& params, const QString& phase, bool fresh, const QString& commitAs = {});
    command::Json chosenPlacements() const;
    command::Json pacingParams(const QString& apply) const;
    void setBrollReport(const command::Json& data, const ProjectState& state, bool fresh);

    QPointer<::AiController> m_ai;
    QPointer<TranscriptBridge> m_transcripts;
    QVariantMap m_broll;
    QVariantMap m_kit;
    QVariantMap m_pacing;
    QString m_stagedKind;
    SequenceId m_sequence;
    command::Json m_kitOptions = command::Json::object();
    command::Json m_pacingReference = command::Json::object(); ///< {referenceCuts, referenceLength} of the last analysis
    command::Json m_pacingOptions = command::Json::object();
    QString m_kitWritten; ///< the folder the kit was last written to
    // Fix round 1 (ARGON-17): what rerun() repeats, and whether the preview is out of date.
    QString m_rerunKind; ///< "" | "broll" | "pacing" | "kit"
    command::Json m_rerunParams = command::Json::object();
    QVariantMap m_kitRequest;       ///< fix round 3: makeKit()'s options, for rerun() (without allowOnce)
    std::uint64_t m_kitRevision = 0; ///< fix round 3: the session revision the kit was made at
    bool m_outdated = false;      ///< Apply was refused: the timeline changed since the preview
    bool m_outdatedShown = false; ///< outdated()
    QPointer<ProjectController> m_watched;
    void watchRevision();
    void refreshOutdated();
    QString refuseIfOutdated();
};

struct AutopilotBridgeForeign {
    Q_GADGET
    QML_FOREIGN(AutopilotBridge)
    QML_NAMED_ELEMENT(Autopilot)
    QML_SINGLETON
public:
    static AutopilotBridge* create(QQmlEngine* e, QJSEngine* j) { return AutopilotBridge::create(e, j); }
};

} // namespace wabcut::app
