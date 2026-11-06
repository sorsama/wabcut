#include "SourcesBridge.h"

#include "CommandJob.h"
#include "ProjectController.h"
#include "TranscriptBridge.h"

#include <wabcut/command/Engine.h>
#include <wabcut/command/SourceList.h>
#include <wabcut/model/Sequence.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPageSize>
#include <QPdfWriter>
#include <QQmlEngine>
#include <QSaveFile>
#include <QTextDocument>

#include <algorithm>

namespace wabcut::app {

using command::Json;

namespace {

QString fromStd(const std::string& s) { return QString::fromStdString(s); }

} // namespace

SourcesBridge::SourcesBridge(QObject* parent) : QObject(parent) {}

SourcesBridge* SourcesBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new SourcesBridge;
    if (qmlEngine) {
        bridge->m_transcripts = qmlEngine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    }
    return bridge;
}

void SourcesBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::revisionChanged, this, &SourcesBridge::refresh);
        connect(controller, &ProjectController::sequenceChanged, this, &SourcesBridge::refresh);
        connect(controller, &ProjectController::playheadChanged, this, &SourcesBridge::sourcesChanged); // atPlayhead
    }
    refresh();
}

void SourcesBridge::setOnlyNeeded(bool only) {
    if (m_onlyNeeded == only)
        return;
    m_onlyNeeded = only;
    emit sourcesChanged();
}

void SourcesBridge::setMessage(const QString& message) {
    if (m_message == message)
        return;
    m_message = message;
    emit messageChanged();
}

void SourcesBridge::refresh() {
    int count = 0;
    int needed = 0;
    if (m_controller) {
        if (const SequencePtr seq = m_controller->currentSequence()) {
            for (const command::SourceEntry& e : command::sourceEntries(*seq)) {
                ++count;
                needed += e.needed() ? 1 : 0;
            }
        }
    }
    m_count = count;
    m_needed = needed;
    emit sourcesChanged();
}

QVariantList SourcesBridge::items() const { return list(m_onlyNeeded); }

QVariantList SourcesBridge::list(bool filtered) const {
    QVariantList out;
    if (!m_controller)
        return out;
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq)
        return out;
    const Ticks playhead = m_controller->playhead();
    for (const command::SourceEntry& e : command::sourceEntries(*seq)) {
        if (filtered && !e.needed())
            continue;
        out.append(QVariantMap{{QStringLiteral("index"), static_cast<int>(e.marker)},
                               {QStringLiteral("number"), e.number},
                               {QStringLiteral("claim"), fromStd(e.claim)},
                               {QStringLiteral("note"), fromStd(e.note)},
                               {QStringLiteral("url"), fromStd(e.url)},
                               {QStringLiteral("title"), fromStd(e.citation.title)},
                               {QStringLiteral("author"), fromStd(e.citation.author)},
                               {QStringLiteral("publisher"), fromStd(e.citation.publisher)},
                               {QStringLiteral("date"), fromStd(e.citation.date)},
                               {QStringLiteral("archive"), fromStd(e.citation.archive)},
                               {QStringLiteral("screenshot"), fromStd(e.citation.screenshot)},
                               {QStringLiteral("needed"), e.needed()},
                               {QStringLiteral("cited"), e.cited()},
                               {QStringLiteral("atText"), m_controller->formatTimecode(e.start)},
                               {QStringLiteral("clock"), fromStd(command::clockText(e.start))},
                               {QStringLiteral("atSeconds"), e.start.toSeconds()},
                               {QStringLiteral("duration"), e.duration.toSeconds()},
                               {QStringLiteral("atPlayhead"), playhead >= e.start && playhead < e.start + std::max(e.duration, Ticks{1})}});
    }
    return out;
}

QString SourcesBridge::runCommand(const std::string& name, const QVariantMap& params) {
    if (!m_controller)
        return tr("No project");
    Json p = Json::parse(QJsonDocument(QJsonObject::fromVariantMap(params)).toJson(QJsonDocument::Compact).toStdString());
    const command::CommandResult result = CommandJob::runNow(m_controller, nullptr, name, std::move(p));
    const QString error = commandErrorText(result);
    setMessage(error);
    return error;
}

QString SourcesBridge::add(const QString& claim, const QVariantMap& fields, const QString& range) {
    if (!m_controller)
        return tr("No project");
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq)
        return tr("Open a sequence first");
    if (claim.trimmed().isEmpty()) {
        setMessage(tr("Write the claim the source backs"));
        return m_message;
    }
    Ticks start = m_controller->playhead();
    Ticks duration{};
    if (range == QLatin1String("marks") && seq->markIn && seq->markOut && *seq->markOut > *seq->markIn) {
        start = *seq->markIn;
        duration = *seq->markOut - *seq->markIn;
    } else if (range == QLatin1String("transcript")) {
        const QVariantMap span = m_transcripts ? m_transcripts->selectionTimeSpan() : QVariantMap{};
        if (span.isEmpty()) {
            setMessage(tr("Select words in the Transcript tab first"));
            return m_message;
        }
        start = Ticks{span.value(QStringLiteral("start")).toLongLong()};
        duration = Ticks{span.value(QStringLiteral("duration")).toLongLong()};
    } else if (range == QLatin1String("selection")) {
        bool any = false;
        Ticks from = Ticks::max();
        Ticks to{};
        for (const ClipId id : m_controller->selection()) {
            if (const auto located = seq->findClip(id)) {
                from = std::min(from, located->clip().start());
                to = std::max(to, located->clip().end());
                any = true;
            }
        }
        if (!any) {
            setMessage(tr("Select a clip first"));
            return m_message;
        }
        start = from;
        duration = to - from;
    }
    QVariantMap params = fields;
    params.insert(QStringLiteral("claim"), claim.trimmed());
    params.insert(QStringLiteral("time"), static_cast<qlonglong>(start.value));
    if (duration.value > 0)
        params.insert(QStringLiteral("duration"), static_cast<qlonglong>(duration.value));
    return runCommand("sources.add", params);
}

QString SourcesBridge::update(int index, const QVariantMap& fields) {
    QVariantMap params = fields;
    params.insert(QStringLiteral("index"), index);
    return runCommand("sources.update", params);
}

QString SourcesBridge::remove(int index) { return runCommand("sources.remove", QVariantMap{{QStringLiteral("index"), index}}); }

void SourcesBridge::seekTo(int index) {
    if (!m_controller)
        return;
    if (const SequencePtr seq = m_controller->currentSequence(); seq && index >= 0 && index < static_cast<int>(seq->markers.size()))
        m_controller->setPlayhead(seq->markers[static_cast<std::size_t>(index)].start);
}

QVariantMap SourcesBridge::exportText(const QString& format, bool includeUncited) const {
    QVariantMap out{{QStringLiteral("text"), QString()}, {QStringLiteral("cited"), 0}, {QStringLiteral("uncited"), 0}};
    if (!m_controller)
        return out;
    const SequencePtr seq = m_controller->currentSequence();
    const auto parsed = command::sourceFormatFromName(format.toStdString());
    if (!seq || !parsed)
        return out;
    command::SourceListOptions options;
    options.includeUncited = includeUncited;
    const command::SourceListText text = command::formatSources(command::sourceEntries(*seq), *parsed, options);
    QStringList warnings;
    for (const std::string& w : text.warnings)
        warnings << fromStd(w);
    return QVariantMap{{QStringLiteral("text"), fromStd(text.text)},
                       {QStringLiteral("cited"), text.cited},
                       {QStringLiteral("uncited"), text.uncited},
                       {QStringLiteral("warnings"), warnings}};
}

QString SourcesBridge::exportFile(const QUrl& file, const QString& format, bool includeUncited) {
    const QVariantMap exported = exportText(format, includeUncited);
    const QString text = exported.value(QStringLiteral("text")).toString();
    if (text.isEmpty()) {
        setMessage(tr("Nothing to export: no claim has a source yet"));
        return m_message;
    }
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    if (path.isEmpty()) {
        setMessage(tr("Choose a file"));
        return m_message;
    }
    if (path.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive)) {
        // A PDF of the same list: the Markdown form rendered by Qt's document, A4, written through a temporary file so a
        // failed save never leaves half a PDF where a good one was.
        QTextDocument document;
        QString markdown;
        if (format == QLatin1String("apa")) {
            QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            if (!lines.isEmpty())
                lines.removeFirst(); // the "References" heading, replaced by a Markdown one
            markdown = QStringLiteral("# References\n\n") + lines.join(QStringLiteral("\n\n"));
        } else if (format == QLatin1String("markdown")) {
            markdown = text;
        } else {
            markdown = exportText(QStringLiteral("markdown"), includeUncited).value(QStringLiteral("text")).toString();
        }
        document.setMarkdown(markdown);
        QSaveFile out(path);
        if (!out.open(QIODevice::WriteOnly)) {
            setMessage(tr("Cannot write %1").arg(QFileInfo(path).fileName()));
            return m_message;
        }
        QPdfWriter pdf(&out);
        pdf.setPageSize(QPageSize(QPageSize::A4));
        pdf.setTitle(tr("Sources"));
        document.print(&pdf);
        if (!out.commit()) {
            setMessage(tr("Cannot write %1").arg(QFileInfo(path).fileName()));
            return m_message;
        }
    } else {
        QSaveFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text) || out.write(text.toUtf8()) < 0 || !out.commit()) {
            setMessage(tr("Cannot write %1").arg(QFileInfo(path).fileName()));
            return m_message;
        }
    }
    setMessage({});
    return {};
}

QString SourcesBridge::addLowerThirds(double seconds) {
    return runCommand("sources.lowerThirds", QVariantMap{{QStringLiteral("seconds"), std::clamp(seconds, 0.5, 30.0)}});
}

} // namespace wabcut::app
