#pragma once

// AudioDescriptionBridge: Sequence > Generate Audio Description, for QML (singleton `AudioDescription`, 0.7 card 175).
//
//   1. analyze   finds the gaps in the sequence's dialogue (its transcript) that a line fits in: where, how many words
//                fit, the words said around them (AudioDescription.h findGaps);
//   2. describe  a chat model that can see pictures (your cloud model after consent, or one on this computer) is shown a
//                frame or two of each gap with the words around it and writes one line per gap, off the GUI thread with
//                progress and Cancel. Nothing is sent without the consent prompt and the cost shown; offline mode and a
//                no-cloud project allow only a model on this computer;
//   3. the lines are listed for review: edit any, drop any (a word count against what fits the gap), then
//   4. apply     puts them on the caption track "Audio Description" (audioDescription.createTrack, one undo step; switched
//                off so it is not burned in; editable in the Captions tab like any track),
//   5. voice     opens the Dub dialog for that track (the voice, the cost and the consent are asked there; the dialogue and
//                music go down under each line), or exportScript writes a script for a narrator.
//
// Principle 9: QML sees text and timecodes.

#include <wabcut/ai/dub/AudioDescription.h>
#include <wabcut/model/Ids.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

class AiController;
class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class ProjectController;
class TranscriptBridge;

class AudioDescriptionBridge : public QObject {
    Q_OBJECT
    /// [{ index, startText, endText, seconds, maxWords, before, after }]: the gaps found.
    Q_PROPERTY(QVariantList gaps READ gaps NOTIFY gapsChanged FINAL)
    /// [{ index, startText, seconds, text, maxWords, words, over }]: the lines written (and edited), in time order.
    Q_PROPERTY(QVariantList lines READ lines NOTIFY linesChanged FINAL)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged FINAL)
    Q_PROPERTY(double progress READ progress NOTIFY runningChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

public:
    explicit AudioDescriptionBridge(QObject* parent = nullptr);
    ~AudioDescriptionBridge() override;
    static AudioDescriptionBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setEngine(QQmlEngine* engine);

    QVariantList gaps() const;
    QVariantList lines() const;
    bool running() const { return m_running; }
    double progress() const { return m_progress; }
    QString message() const { return m_message; }

    /// Finds the gaps. options: minGap (seconds), wordsPerSecond, silent (the sequence has no speech). Returns "" or the
    /// problem (no sequence, no transcript).
    Q_INVOKABLE QString analyze(const QVariantMap& options = {});
    /// Which model would write the lines, what it would cost and whether it needs consent:
    /// { available, reason, model, name, local, cost, needed, prompt, pictures }.
    Q_INVOKABLE QVariantMap quote(const QVariantMap& options = {}) const;
    /// Writes a line per gap with the chat model (background job). options: allowOnce (the consent was given just now),
    /// language, style (guidance), frames (1..3 pictures per gap, default 1). Returns "" or why it did not start.
    Q_INVOKABLE QString describe(const QVariantMap& options);
    Q_INVOKABLE void cancel();
    /// Your own line for a gap (the dialog's "Write myself"), or the text of line `index` changed.
    Q_INVOKABLE QString setLineText(int index, const QString& text);
    Q_INVOKABLE void removeLine(int index);
    Q_INVOKABLE void clearLines();
    /// Puts the lines on the "Audio Description" caption track. Returns "" or the problem. `captionTrack` is its id.
    Q_INVOKABLE QString apply(const QString& language = {});
    qint64 captionTrack() const { return m_track; }
    /// Opens the Dub dialog for the track made by apply().
    Q_INVOKABLE QString voice(const QString& language = {});
    /// Writes the lines as a script for a narrator: "00:00:03:12 - 00:00:06:00 (3.2 s, 8 words max)  A woman opens...".
    Q_INVOKABLE QString exportScript(const QUrl& file) const;
    /// The script as text (what exportScript writes).
    Q_INVOKABLE QString scriptText() const;

signals:
    void gapsChanged();
    void linesChanged();
    void runningChanged();
    void messageChanged();

private:
    struct Impl;
    void setMessage(const QString& message);
    void finishDescribe();
    ai::dub::DescribeOptions describeOptions(const QVariantMap& options) const;

    QPointer<ProjectController> m_controller;
    QPointer<TranscriptBridge> m_transcripts;
    QPointer<::AiController> m_ai;
    QPointer<QQmlEngine> m_engine;
    std::vector<ai::dub::AdGap> m_gaps;
    std::vector<ai::dub::AdLine> m_lines;
    std::atomic<bool> m_cancel{false};
    bool m_running = false;
    double m_progress = 0.0;
    QString m_message;
    qint64 m_track = 0;
    SequenceId m_sequence;
    std::shared_ptr<Impl> m_job;
};

struct AudioDescriptionSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::AudioDescriptionBridge)
    QML_NAMED_ELEMENT(AudioDescription)
    QML_SINGLETON
public:
    static AudioDescriptionBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return AudioDescriptionBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
