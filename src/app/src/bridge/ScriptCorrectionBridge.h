#pragma once

// ScriptCorrectionBridge (0.6, P1 round #37; QML singleton `ScriptCorrection`): the engine of "Correct the transcript
// from your script" (ScriptCorrectionSheet.qml; Sequence > Captions > Correct Transcript from Script..., the Transcript
// tab's button). Over transcript.correctFromScript (wabcut_ai_roughcut ScriptCorrections.h):
//
//   find(text)   runs it as a dry run on a scratch copy of the project, on a worker (CreatorBase: busy, phase,
//                progress, cancel()), so an hour of speech and a long script never hold the window; `result` lists what
//                it would change ("time  before -> after"), the lines it skipped and the differences it keeps as said.
//   apply()      commits that copy as ONE undo step ("Correct from Script") when the project has not changed since;
//                when it has, the corrections are found again (on a worker) for the user to check and apply.
//   reveal(i)    moves the playhead to a listed change (or kept difference).
//
// QML sees display text only (times as timecode); the ticks stay here (principle 9).

#include "CreatorBridge.h"

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

#include <cstdint>
#include <vector>

namespace wabcut::app {

class ScriptCorrectionBridge : public CreatorBase {
    Q_OBJECT
    /// The last find: { corrected, caseFixes, beats, beatsMatched, beatsPartial, beatsSkipped, keptCount, leftOut, words,
    /// changes: [{ time, line, before, after, kind ("words" | "case") }] (at most 200), skipped: [{ line, text,
    /// quality }], kept: [{ time, line, said, script }] (at most 50 each), warnings: [...] }; {} before one.
    Q_PROPERTY(QVariantMap result READ result NOTIFY resultChanged FINAL)
    /// How many corrections the last apply() made; -1 before one (a new find resets it).
    Q_PROPERTY(int applied READ applied NOTIFY resultChanged FINAL)

public:
    static ScriptCorrectionBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    explicit ScriptCorrectionBridge(QObject* parent = nullptr);

    QVariantMap result() const { return m_result; }
    int applied() const { return m_applied; }

    /// Finds what `text` (the script the narration was read from) corrects in the current sequence's transcript: a
    /// dry run on a worker. "" when it started, else why not.
    Q_INVOKABLE QString find(const QString& text);
    /// Makes the found corrections as one undo step. "" or why not (the project changed since: found again).
    Q_INVOKABLE QString apply();
    /// Moves the playhead to listed change `index` (a kept difference when `kept`).
    Q_INVOKABLE void reveal(int index, bool kept = false);
    /// Reads a .txt / .md / .fountain file the user picked (the file dialog's URL) as the script text; "" and a message
    /// when it cannot.
    Q_INVOKABLE QString readScript(const QUrl& file);
    /// Forgets the found corrections (the script changed, the sheet closed); cancels a find that is running.
    void discard() override;

private:
    void setResult(const command::Json& data, const std::vector<std::string>& warnings);

    QVariantMap m_result;
    QString m_text;
    int m_applied = -1;
    std::vector<std::int64_t> m_changeTicks; ///< per listed change: its start on the timeline
    std::vector<std::int64_t> m_keptTicks;   ///< per listed kept difference
    std::uint64_t m_generation = 0;          ///< bumped by find() and discard(): an older find's result is dropped
};

struct ScriptCorrectionBridgeForeign {
    Q_GADGET
    QML_FOREIGN(ScriptCorrectionBridge)
    QML_NAMED_ELEMENT(ScriptCorrection)
    QML_SINGLETON
public:
    static ScriptCorrectionBridge* create(QQmlEngine* e, QJSEngine* j) { return ScriptCorrectionBridge::create(e, j); }
};

} // namespace wabcut::app
