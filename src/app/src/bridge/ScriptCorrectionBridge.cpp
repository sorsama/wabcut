#include "ScriptCorrectionBridge.h"

#include "ProjectController.h"
#include "TranscriptBridge.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <memory>
#include <utility>

namespace wabcut::app {

using command::Json;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

/// Why it could not run, in the user's words where the command's are technical.
QString reasonOf(const command::CommandError& error) {
    switch (error.code) {
    case command::ErrorCode::Cancelled:
        return ScriptCorrectionBridge::tr("Cancelled");
    case command::ErrorCode::EditRejected:
        return ScriptCorrectionBridge::tr("Transcribe the sequence first: there are no words to correct yet");
    case command::ErrorCode::InvalidParams:
        return error.message.find("no lines") != std::string::npos ? ScriptCorrectionBridge::tr("The script has no lines to correct from")
                                                                    : qs(error.message);
    default:
        return qs(error.message);
    }
}

} // namespace

ScriptCorrectionBridge* ScriptCorrectionBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new ScriptCorrectionBridge;
    bridge->setEngine(qmlEngine);
    return bridge;
}

ScriptCorrectionBridge::ScriptCorrectionBridge(QObject* parent) : CreatorBase(parent) {}

QString ScriptCorrectionBridge::find(const QString& text) {
    if (!m_controller) {
        return tr("No project");
    }
    if (text.trimmed().isEmpty()) {
        return tr("Paste or open your script first");
    }
    if (busy()) {
        return tr("Busy");
    }
    const ProjectSnapshot base = m_controller->snapshot();
    const SequenceId sequence = m_controller->currentSequenceId();
    if (!base->findSequence(sequence)) {
        return tr("Open a sequence first");
    }
    discard();
    m_text = text;
    m_applied = -1;
    // The command only needs the transcripts: the Transcript tab's (this session's and the analysis cache's).
    std::shared_ptr<command::CommandServices> services = m_services;
    if (!services) {
        auto* transcripts = m_engine ? m_engine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript") : nullptr;
        if (!transcripts) {
            return tr("Transcripts are not available");
        }
        services = transcripts->services();
    }
    const std::uint64_t revision = m_controller->session().revision();
    const std::uint64_t generation = ++m_generation;
    QPointer<ScriptCorrectionBridge> self(this);
    const bool started = runJob(
        tr("Finding your lines in the transcript"), base, "transcript.correctFromScript",
        Json{{"text", text.toStdString()}, {"sequence", sequence.value}}, true,
        [self, base, revision, generation](Run&& run) {
            if (!self || self->m_generation != generation) {
                return; // discarded meanwhile (the script changed, the sheet closed)
            }
            if (run.error) {
                self->m_result.clear();
                self->m_changeTicks.clear();
                self->m_keptTicks.clear();
                self->m_message = reasonOf(*run.error);
            } else {
                self->setResult(run.data, run.warnings);
                self->m_message.clear();
                if (run.preview && run.data.value("corrected", 0) > 0) {
                    self->m_staged = run.preview;
                    self->m_stagedBase = base;
                    self->m_stagedRevision = revision;
                }
            }
            emit self->resultChanged();
        },
        std::move(services));
    emit resultChanged();
    return started ? QString() : tr("Busy");
}

void ScriptCorrectionBridge::setResult(const Json& data, const std::vector<std::string>& warnings) {
    m_changeTicks.clear();
    m_keptTicks.clear();
    Json shown = data;
    // Times stay here; QML gets the timecodes (and no word indices).
    for (Json& change : shown["changes"]) {
        m_changeTicks.push_back(change.value("ticks", std::int64_t{0}));
        change.erase("ticks");
        change.erase("words");
    }
    for (Json& kept : shown["kept"]) {
        m_keptTicks.push_back(kept.value("ticks", std::int64_t{0}));
        kept.erase("ticks");
        kept.erase("words");
    }
    const std::string json = shown.dump(-1, ' ', false, Json::error_handler_t::replace);
    m_result = QJsonDocument::fromJson(QByteArray::fromStdString(json)).object().toVariantMap();
    QStringList lines;
    for (const std::string& w : warnings) {
        lines << qs(w);
    }
    m_result.insert(QStringLiteral("warnings"), lines);
}

QString ScriptCorrectionBridge::apply() {
    if (busy()) {
        return tr("Busy");
    }
    if (!staged()) {
        return tr("Find the corrections first");
    }
    const int count = m_result.value(QStringLiteral("corrected")).toInt();
    if (QString error = commitStaged(tr("Correct from Script")); !error.isEmpty()) {
        // The project changed since they were found (an edit, an undo): find them again on it, for the user to check.
        if (m_controller && m_controller->session().revision() != m_stagedRevision && !m_text.isEmpty()) {
            const QString text = m_text;
            if (find(text).isEmpty()) {
                error = tr("The project changed since the corrections were found: they are being found again, check them and apply");
            }
        }
        m_message = error;
        emit resultChanged();
        return error;
    }
    m_applied = count;
    m_message = count == 1 ? tr("Applied 1 correction from the script")
                           : tr("Applied %1 corrections from the script").arg(count);
    emit resultChanged();
    // The Captions tab offers "Update from transcript" for captions made before the corrections.
    if (auto* transcripts = m_engine ? m_engine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript") : nullptr) {
        emit transcripts->corrected();
    }
    return {};
}

void ScriptCorrectionBridge::reveal(int index, bool kept) {
    const std::vector<std::int64_t>& ticks = kept ? m_keptTicks : m_changeTicks;
    const SequencePtr sequence = m_controller ? m_controller->currentSequence() : nullptr;
    if (!sequence || index < 0 || static_cast<std::size_t>(index) >= ticks.size()) {
        return;
    }
    const Ticks at{ticks[static_cast<std::size_t>(index)]};
    m_controller->setPlayhead(seekPointIn(TimeRange{at, framesToTicks(1, sequence->frameRate)}, sequence->frameRate));
}

QString ScriptCorrectionBridge::readScript(const QUrl& file) {
    QFile in(file.isLocalFile() ? file.toLocalFile() : file.toString());
    const QString lower = in.fileName().toLower();
    if (!lower.endsWith(QStringLiteral(".txt")) && !lower.endsWith(QStringLiteral(".md")) && !lower.endsWith(QStringLiteral(".markdown")) &&
        !lower.endsWith(QStringLiteral(".fountain"))) {
        m_message = tr("Choose a .txt, .md or .fountain file");
        emit resultChanged();
        return {};
    }
    if (in.size() > 256 * 1024 || !in.open(QIODevice::ReadOnly)) {
        m_message = tr("The file cannot be read or is larger than 256 KB");
        emit resultChanged();
        return {};
    }
    m_message.clear();
    emit resultChanged();
    return QString::fromUtf8(in.readAll());
}

void ScriptCorrectionBridge::discard() {
    ++m_generation;
    if (busy()) {
        cancel();
    }
    m_result.clear();
    m_changeTicks.clear();
    m_keptTicks.clear();
    m_applied = -1;
    m_message.clear();
    CreatorBase::discard(); // emits resultChanged
}

} // namespace wabcut::app
