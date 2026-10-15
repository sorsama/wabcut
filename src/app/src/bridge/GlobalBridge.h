#pragma once

// AI-12 going global in the app (docs/ai.md "Going global"): two QML singletons over captions.translate and
// dub.create / narration.create, built like the creator bridges (CreatorBridge.h): a job runs its command on a scratch
// copy of the project on a worker, and the live session changes only on apply(), as one undo step guarded by the
// revision the preview was made at.
//
//   TranslateCaptions  the "Translate captions" dialog of the Captions workspace: target languages as chips, on this
//                      computer (OPUS-MT, a download per language pair) or by the user's cloud model (consent, the spend
//                      cap; never for a no-cloud project), a glossary and a tone. preview() translates into a staged
//                      state with a report per language (cues, extended / merged / split, too fast, warnings);
//                      estimate() prices a cloud run without sending anything; download() fetches a pair's model.
//   Dub                the "Dub" dialog: a caption track (or a script: narration), the voice model and voice, the cost
//                      estimate, a hard cap, consent, cloning only with "I have the speaker's permission" ticked.
//                      run() makes the speech (paid) into a staged state with the timing-fit report (lines stretched,
//                      moved, flagged); previewVoice() speaks one short line for the voice picker (paid, priced first).
//                      The assistant's dub.create / narration.create calls prefill this dialog (proposalRequested).

#include "CreatorBridge.h"

#include <wabcut/ai/dub/DubCommands.h>

#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <map>
#include <memory>
#include <string>

class AiController;

namespace wabcut::app {

class TranslateBridge : public CreatorBase {
    Q_OBJECT
    /// The current sequence's caption tracks: [{id, name, language, cues}].
    Q_PROPERTY(QVariantList captionTracks READ captionTracks NOTIFY resultChanged FINAL)
    /// The user's cloud chat model can translate (a model is set up, not offline, the project is not no-cloud).
    Q_PROPERTY(bool cloudAvailable READ cloudAvailable NOTIFY resultChanged FINAL)
    /// That model's name ("" without one).
    Q_PROPERTY(QString cloudModelName READ cloudModelName NOTIFY resultChanged FINAL)
    /// After preview(): {sourceTrack, sourceLanguage, cues, corrected, redacted, languages: [{language, name, captionTrack,
    ///  provider, model, cues, kept, extended, merged, rebalanced, split, tooFast, maxCharsPerSecond, readingSpeed,
    ///  retried, untranslated, warnings}]}; after estimate(): {estimate: {usd, ceilingUsd, languages}}.
    Q_PROPERTY(QVariantMap report READ report NOTIFY resultChanged FINAL)

public:
    static TranslateBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    explicit TranslateBridge(QObject* parent = nullptr);

    QVariantList captionTracks() const;
    bool cloudAvailable() const;
    QString cloudModelName() const;
    QVariantMap report() const { return m_report; }

    /// The languages the chips offer for captions in `source`: [{code, name, nativeName, readingSpeed, local, localModel,
    /// installed, downloadMB}] ("local": a model on this computer can translate the pair).
    Q_INVOKABLE QVariantList languages(const QString& source) const;
    /// options: captionTrack, languages (list of codes), provider ("auto" | "local" | "cloud"), glossary (text: one
    /// "term = translation" per line, or a list of {source, target}), tone, notes, readingSpeed, allowOnce (the consent
    /// prompt's "Allow once").
    Q_INVOKABLE QString preview(const QVariantMap& options);
    Q_INVOKABLE QString estimate(const QVariantMap& options);
    /// Downloads the model that translates `source` into `target` (pinned, hash-checked; through the AI network stack).
    Q_INVOKABLE QString download(const QString& source, const QString& target);
    /// What the consent prompt shows before captions go to the cloud model.
    Q_INVOKABLE QVariantMap consentPrompt(int cues) const;
    Q_INVOKABLE QString apply();

    /// What the assistant's captions.translate gets: the installed local models, and the cloud model when the user has
    /// already allowed it (an assistant run cannot show the consent prompt), the Qt line breaker.
    std::shared_ptr<command::CommandServices> agentServices();

private:
    command::Json paramsOf(const QVariantMap& options) const;
    std::shared_ptr<command::CommandServices> jobServices(bool allowOnce);
    QVariantMap m_report;
    QPointer<::AiController> m_ai;
    QPointer<TranscriptBridge> m_transcripts;
};

class DubBridge : public CreatorBase {
    Q_OBJECT
    Q_PROPERTY(QVariantList captionTracks READ captionTracks NOTIFY resultChanged FINAL)
    /// The project's speakers, for cloning: [{id, name}].
    Q_PROPERTY(QVariantList speakers READ speakers NOTIFY resultChanged FINAL)
    /// After estimate(): dub.create / narration.create's plan (lines, characters, estimate {usd, ceilingUsd}, voice, track).
    Q_PROPERTY(QVariantMap plan READ plan NOTIFY resultChanged FINAL)
    /// After run(): the timing-fit report (fit {fitted, stretched, shifted, flagged, maxSpeed}, flagged lines, placed).
    Q_PROPERTY(QVariantMap report READ report NOTIFY resultChanged FINAL)
    /// The assistant's request, when the dialog was opened for one: the options to prefill.
    Q_PROPERTY(QVariantMap proposal READ proposal NOTIFY proposalChanged FINAL)
    /// The project is no-cloud (or offline): voice-over is unavailable.
    Q_PROPERTY(bool cloudAvailable READ cloudAvailable NOTIFY resultChanged FINAL)

public:
    static DubBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    explicit DubBridge(QObject* parent = nullptr);

    QVariantList captionTracks() const;
    QVariantList speakers() const;
    QVariantMap plan() const { return m_plan; }
    QVariantMap report() const { return m_report; }
    QVariantMap proposal() const { return m_proposal; }
    bool cloudAvailable() const;

    /// The voices of a model: [{id, name, description}].
    Q_INVOKABLE QVariantList voices(const QString& model) const;
    /// options: kind ("dub" | "narration"), captionTrack, text, language, model, voice, style, maxStretch, duck, name,
    /// cloneVoice, speaker, rightsConfirmed, at (seconds), onBeats, maxCost (dollars), allowOnce.
    Q_INVOKABLE QString estimate(const QVariantMap& options);
    Q_INVOKABLE QString run(const QVariantMap& options);
    /// Speaks `text` (or the first line) with the chosen voice into a short WAV and opens it; paid (a few cents at most).
    Q_INVOKABLE QString previewVoice(const QVariantMap& options);
    /// The consent prompt for a run: speech of the text, or with `clone` the upload of the speaker's recording too
    /// (a consent of its own, "cloneVoice").
    Q_INVOKABLE QVariantMap consentPrompt(const QString& model, int characters, bool clone = false) const;
    Q_INVOKABLE QString apply();
    Q_INVOKABLE void clearProposal();

    /// The assistant's dub.create / narration.create: prefill the dialog (GUI thread). A request for a caption track the
    /// project does not have yet (the assistant's own staged translation) waits until it does (the edit was applied).
    void propose(const QVariantMap& options);
    /// What the assistant's commands get: a host that only proposes.
    std::shared_ptr<command::CommandServices> proposalServices();

    /// Tests (with setServices: a DirectDubServices on a scripted provider): the sample's provider; no playback.
    void setPreviewSpeech(std::shared_ptr<ai::ISpeechSynth> synth) { m_previewSynth = std::move(synth); }
    void setOpenSamples(bool open) { m_openSamples = open; }
    QString lastSample() const { return m_lastSample; }

signals:
    void proposalChanged();
    /// Main.qml opens the dialog.
    void proposalRequested();

private:
    command::Json paramsOf(const QVariantMap& options, bool estimate) const;
    bool hasCaptionTrack(qint64 id) const;
    void openPending();
    std::shared_ptr<command::CommandServices> jobServices(bool allowOnce);
    QVariantMap m_plan;
    QVariantMap m_report;
    QVariantMap m_proposal;
    QVariantMap m_pending;
    QPointer<::AiController> m_ai;
    QPointer<TranscriptBridge> m_transcripts;
    std::shared_ptr<std::map<std::string, std::shared_ptr<ai::ISpeechSynth>>> m_openedSpeech;
    std::shared_ptr<ai::ISpeechSynth> m_previewSynth;
    bool m_openSamples = true;
    QString m_lastSample;
};

/// Captions in Thai, Lao, Khmer and Burmese break between words: Qt's line breaker (ICU / the platform's dictionary).
std::shared_ptr<const LineBreaker> qtLineBreaker();

struct TranslateBridgeForeign {
    Q_GADGET
    QML_FOREIGN(TranslateBridge)
    QML_NAMED_ELEMENT(TranslateCaptions)
    QML_SINGLETON
public:
    static TranslateBridge* create(QQmlEngine* e, QJSEngine* j) { return TranslateBridge::create(e, j); }
};

struct DubBridgeForeign {
    Q_GADGET
    QML_FOREIGN(DubBridge)
    QML_NAMED_ELEMENT(Dub)
    QML_SINGLETON
public:
    static DubBridge* create(QQmlEngine* e, QJSEngine* j) { return DubBridge::create(e, j); }
};

} // namespace wabcut::app
