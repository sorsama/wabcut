#include "HistoryBridge.h"

#include "BridgeIds.h"
#include "CommandJob.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "TimelineView.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectDiff.h>
#include <wabcut/model/Timecode.h>

#include <QDateTime>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QLocale>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStringList>
#include <QWindow>

#include <algorithm>

namespace wabcut::app {

using command::Json;

namespace {

QString text(const std::string& s) { return QString::fromStdString(s); }

QStringList textList(const std::vector<std::string>& lines) {
    QStringList out;
    out.reserve(static_cast<qsizetype>(lines.size()));
    for (const std::string& l : lines)
        out << text(l);
    return out;
}

QString kindText(HistoryKind kind) {
    switch (kind) {
    case HistoryKind::Save:
        return HistoryBridge::tr("Saved");
    case HistoryKind::Auto:
        return HistoryBridge::tr("Autosave");
    case HistoryKind::Agent:
        return HistoryBridge::tr("Before AI edit");
    case HistoryKind::Manual:
        return HistoryBridge::tr("Version");
    case HistoryKind::Restore:
        return HistoryBridge::tr("Before restore");
    }
    return {};
}

QString createdText(const std::string& iso) {
    QDateTime when = QDateTime::fromString(text(iso), Qt::ISODate);
    if (!when.isValid())
        return text(iso);
    when = when.toLocalTime();
    const QDate today = QDate::currentDate();
    const QString time = QLocale().toString(when.time(), QLocale::ShortFormat);
    if (when.date() == today)
        return HistoryBridge::tr("Today %1").arg(time);
    if (when.date() == today.addDays(-1))
        return HistoryBridge::tr("Yesterday %1").arg(time);
    return QLocale().toString(when, QLocale::ShortFormat);
}

/// Every TimelineView showing `controller`'s project, in any window.
std::vector<TimelineView*> timelineViews(const ProjectController* controller) {
    std::vector<TimelineView*> out;
    for (QWindow* window : QGuiApplication::topLevelWindows()) {
        auto* quick = qobject_cast<QQuickWindow*>(window);
        if (!quick)
            continue;
        QList<TimelineView*> found = quick->findChildren<TimelineView*>();
        if (quick->contentItem())
            found += quick->contentItem()->findChildren<TimelineView*>();
        for (TimelineView* view : found) {
            if (view->controller() == controller && std::ranges::find(out, view) == out.end())
                out.push_back(view);
        }
    }
    return out;
}

/// "V1" / "A2" of a sequence -> its track id.
std::optional<TrackId> trackByLabel(const Sequence& seq, const std::string& label) {
    if (label.size() < 2 || (label[0] != 'V' && label[0] != 'A'))
        return std::nullopt;
    const auto& tracks = label[0] == 'V' ? seq.videoTracks : seq.audioTracks;
    std::size_t index = 0;
    for (std::size_t i = 1; i < label.size(); ++i) {
        if (label[i] < '0' || label[i] > '9')
            return std::nullopt;
        index = index * 10 + static_cast<std::size_t>(label[i] - '0');
    }
    if (index < 1 || index > tracks.size())
        return std::nullopt;
    return tracks[index - 1]->id;
}

QString clipWhat(const ClipDiff& d) {
    QStringList parts;
    if (d.moved)
        parts << HistoryBridge::tr("moved");
    if (d.trimmed)
        parts << HistoryBridge::tr("trimmed");
    if (d.retimed)
        parts << HistoryBridge::tr("retimed");
    if (d.source)
        parts << HistoryBridge::tr("new source");
    if (d.enabled)
        parts << HistoryBridge::tr("enabled or disabled");
    if (d.renamed)
        parts << HistoryBridge::tr("renamed");
    if (d.relinked)
        parts << HistoryBridge::tr("linked or unlinked");
    const std::size_t effects = d.effectsAdded.size() + d.effectsRemoved.size() + d.effectsChanged.size();
    if (effects > 0)
        parts << HistoryBridge::tr("%n effect(s)", nullptr, static_cast<int>(effects));
    for (const std::string& k : d.keyframes)
        parts << HistoryBridge::tr("%1 keyframes").arg(text(k));
    for (const std::string& p : d.parameters)
        parts << text(p);
    return parts.join(QStringLiteral(", "));
}

} // namespace

HistoryBridge::HistoryBridge(QObject* parent) : QObject(parent) {
    m_reason = tr("Save the project to keep a version history");
    m_autoTimer.setInterval(m_autoSeconds * 1000);
    connect(&m_autoTimer, &QTimer::timeout, this, &HistoryBridge::autoCheckpoint);
}

HistoryBridge::~HistoryBridge() = default;

HistoryBridge* HistoryBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new HistoryBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setLifecycle(qmlEngine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile"));
    }
    return bridge;
}

void HistoryBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller)
        connect(controller, &ProjectController::revisionChanged, this, &HistoryBridge::onRevisionChanged);
    reopen();
}

void HistoryBridge::setLifecycle(ProjectLifecycle* lifecycle) {
    if (m_lifecycle == lifecycle)
        return;
    if (m_lifecycle)
        disconnect(m_lifecycle, nullptr, this, nullptr);
    m_lifecycle = lifecycle;
    if (lifecycle) {
        m_wasDirty = lifecycle->dirty();
        connect(lifecycle, &ProjectLifecycle::documentChanged, this, &HistoryBridge::onDocumentChanged);
    }
    reopen();
}

void HistoryBridge::setAutoIntervalSeconds(int seconds) {
    seconds = std::max(1, seconds);
    if (seconds == m_autoSeconds)
        return;
    m_autoSeconds = seconds;
    m_autoTimer.setInterval(seconds * 1000);
    emit settingsChanged();
}

void HistoryBridge::setMessage(const QString& message) {
    m_message = message;
    emit messageChanged();
}

void HistoryBridge::reopen() {
    const std::filesystem::path file = m_controller ? m_controller->projectFile() : std::filesystem::path{};
    if (file == m_file && (m_history || file.empty()))
        return;
    m_file = file;
    m_history.reset();
    m_services.reset();
    m_autoTimer.stop();
    clearComparison();
    if (file.empty()) {
        m_reason = tr("Save the project to keep a version history");
    } else {
        auto opened = ProjectHistory::open(ProjectHistory::folderFor(file), m_options);
        if (!opened) {
            // A damaged folder: editing goes on without history; the history commands fail cleanly.
            m_reason = tr("The version history cannot be read: %1").arg(text(opened.error().message));
        } else {
            m_history = std::make_shared<ProjectHistory>(std::move(*opened));
            m_services = std::make_shared<command::HistoryServices>(m_history);
            m_reason.clear();
            m_autoTimer.start();
        }
    }
    m_checkpointRevision = m_controller ? m_controller->revision() : -1;
    emit availabilityChanged();
    refresh();
}

void HistoryBridge::onDocumentChanged() {
    const bool dirty = m_lifecycle && m_lifecycle->dirty();
    const std::filesystem::path file = m_controller ? m_controller->projectFile() : std::filesystem::path{};
    if (file != m_file) {
        // Opened another project or saved as another file: its own history.
        reopen();
        if (m_history && m_wasDirty && !dirty)
            onSaved(); // "save as" of an edited project
    } else if (m_wasDirty && !dirty) {
        onSaved();
    }
    m_wasDirty = dirty;
}

void HistoryBridge::onRevisionChanged() {
    if (m_previewShown) {
        clearPreview(); // the highlights belong to the project as it was compared
        if (!m_comparison.isEmpty()) {
            m_comparison.insert(QStringLiteral("stale"), true);
            emit comparisonChanged();
        }
    }
}

void HistoryBridge::onSaved() {
    if (m_history)
        checkpoint(QStringLiteral("save"));
}

void HistoryBridge::checkpointBeforeAgent() {
    if (m_history)
        checkpoint(QStringLiteral("agent"));
}

void HistoryBridge::autoCheckpoint() {
    if (!m_history || !m_controller || m_controller->revision() == m_checkpointRevision)
        return;
    checkpoint(QStringLiteral("auto"));
}

QString HistoryBridge::run(const char* command, const QVariantMap& params) {
    if (!m_controller)
        return tr("No project");
    if (!m_history)
        return m_reason;
    const command::CommandResult result =
        CommandJob::runNow(m_controller, m_services, command, Json::parse(QJsonDocument::fromVariant(params).toJson().toStdString()));
    const QString error = commandErrorText(result);
    if (!error.isEmpty())
        setMessage(error);
    return error;
}

QString HistoryBridge::checkpoint(const QString& kind, const QString& label) {
    QVariantMap params{{QStringLiteral("kind"), kind}};
    if (!label.trimmed().isEmpty())
        params.insert(QStringLiteral("label"), label.trimmed());
    const QString error = run("history.checkpoint", params);
    if (error.isEmpty() && m_controller)
        m_checkpointRevision = m_controller->revision();
    refresh();
    return error;
}

QString HistoryBridge::saveVersion(const QString& label) {
    if (label.trimmed().isEmpty())
        return tr("Name the version");
    const QString error = checkpoint(QStringLiteral("manual"), label);
    if (error.isEmpty())
        setMessage(tr("Saved version “%1”").arg(label.trimmed()));
    return error;
}

QString HistoryBridge::name(int id, const QString& label) {
    const QString error = run("history.name", {{QStringLiteral("revision"), id}, {QStringLiteral("label"), label.trimmed()}});
    refresh();
    return error;
}

QString HistoryBridge::restore(int id) {
    const QString error = run("history.restore", {{QStringLiteral("revision"), id}});
    if (error.isEmpty()) {
        setMessage(tr("Restored version %1").arg(id));
        m_checkpointRevision = m_controller ? m_controller->revision() : -1;
    }
    refresh();
    return error;
}

QString HistoryBridge::restoreSequence(int id, const QString& sequenceKey, const QString& name) {
    const auto seq = parseSequenceKey(sequenceKey);
    if (!seq)
        return tr("Choose a sequence of the version");
    QVariantMap params{{QStringLiteral("revision"), id}, {QStringLiteral("from"), static_cast<qulonglong>(seq->value)}};
    if (!name.trimmed().isEmpty())
        params.insert(QStringLiteral("name"), name.trimmed());
    const QString error = run("history.restore", params);
    if (error.isEmpty())
        setMessage(tr("Restored the sequence as a new one"));
    refresh();
    return error;
}

QVariantList HistoryBridge::sequencesOf(int id) const {
    QVariantList out;
    if (!m_history || id <= 0)
        return out;
    auto state = m_history->load(static_cast<std::uint64_t>(id));
    if (!state)
        return out;
    for (const SequencePtr& s : state->sequences)
        out.append(QVariantMap{{QStringLiteral("key"), sequenceKey(s->id)}, {QStringLiteral("name"), text(s->name)}});
    return out;
}

QString HistoryBridge::exportVersion(int id, const QUrl& file, bool overwrite) {
    const QString local = file.isLocalFile() ? file.toLocalFile() : file.toString();
    if (local.isEmpty())
        return tr("Choose a file");
    return run("history.export", {{QStringLiteral("revision"), id}, {QStringLiteral("path"), local}, {QStringLiteral("overwrite"), overwrite}});
}

void HistoryBridge::refresh() {
    QVariantList versions;
    QString disk;
    if (m_history) {
        const auto& revisions = m_history->revisions();
        for (auto it = revisions.rbegin(); it != revisions.rend(); ++it) {
            const HistoryRevision& r = *it;
            const QString kind = text(std::string(toString(r.kind)));
            versions.append(QVariantMap{{QStringLiteral("id"), static_cast<qulonglong>(r.id)},
                                        {QStringLiteral("createdAt"), text(r.createdAt)},
                                        {QStringLiteral("createdText"), createdText(r.createdAt)},
                                        {QStringLiteral("kind"), kind},
                                        {QStringLiteral("kindText"), kindText(r.kind)},
                                        {QStringLiteral("label"), text(r.label)},
                                        {QStringLiteral("named"), r.named()},
                                        {QStringLiteral("title"), r.named() ? text(r.label) : kindText(r.kind)},
                                        {QStringLiteral("sequences"), static_cast<qulonglong>(r.sequences)},
                                        {QStringLiteral("clips"), static_cast<qulonglong>(r.clips)},
                                        {QStringLiteral("sizeText"),
                                         QLocale().formattedDataSize(static_cast<qint64>(r.addedBytes), 1, QLocale::DataSizeSIFormat)},
                                        {QStringLiteral("latest"), it == revisions.rbegin()}});
        }
        disk = tr("%1 on disk").arg(QLocale().formattedDataSize(static_cast<qint64>(m_history->diskBytes()), 1,
                                                               QLocale::DataSizeSIFormat));
    }
    if (versions != m_versions || disk != m_diskText) {
        m_versions = versions;
        m_diskText = disk;
        emit versionsChanged();
    }
}

QString HistoryBridge::compare(const QVariant& from, const QVariant& to) {
    if (!m_controller)
        return tr("No project");
    if (!m_history)
        return m_reason;
    const auto load = [&](const QVariant& v, QString& label) -> std::optional<ProjectState> {
        if (v.toString() == QStringLiteral("current")) {
            label = tr("Now");
            return *m_controller->snapshot();
        }
        bool ok = false;
        const qulonglong id = v.toULongLong(&ok);
        if (!ok || id == 0)
            return std::nullopt;
        auto state = m_history->load(id);
        if (!state)
            return std::nullopt;
        const HistoryRevision* r = m_history->find(id);
        label = r && r->named() ? tr("“%1”").arg(text(r->label)) : tr("Version %1").arg(id);
        return std::move(*state);
    };
    QString fromText;
    QString toText;
    const auto before = load(from, fromText);
    const auto after = load(to, toText);
    if (!before || !after) {
        setMessage(tr("That version cannot be read"));
        return m_message;
    }
    const ProjectDiff diff = diffProjects(*before, *after);
    const SequenceId current = m_controller->currentSequenceId();
    QVariantList sequences;
    TimelineView::ExternalPreview preview;
    const SequencePtr shown = m_controller->currentSequence(); // the timeline shows it
    for (const SequenceDiff& s : diff.sequences) {
        const auto tc = [&](const TimeRange& r) {
            return text(ticksToTimecode(r.start, s.frameRate, s.dropFrame)) + QStringLiteral("–") +
                   text(ticksToTimecode(r.end(), s.frameRate, s.dropFrame));
        };
        QVariantList clips;
        int added = 0, removed = 0, changed = 0;
        for (const ClipDiff& d : s.clips) {
            const char* kind = d.kind == ClipDiff::Kind::Added ? "added" : d.kind == ClipDiff::Kind::Removed ? "removed" : "changed";
            (d.kind == ClipDiff::Kind::Added ? added : d.kind == ClipDiff::Kind::Removed ? removed : changed) += 1;
            clips.append(QVariantMap{{QStringLiteral("name"), text(d.name)},
                                     {QStringLiteral("kind"), QString::fromLatin1(kind)},
                                     {QStringLiteral("track"), text(d.trackAfter.empty() ? d.trackBefore : d.trackAfter)},
                                     {QStringLiteral("beforeText"), d.kind == ClipDiff::Kind::Added ? QString() : tc(d.before)},
                                     {QStringLiteral("afterText"), d.kind == ClipDiff::Kind::Removed ? QString() : tc(d.after)},
                                     {QStringLiteral("whatText"), d.kind == ClipDiff::Kind::Changed ? clipWhat(d) : QString()}});
            // The shown sequence's ranges on the timeline: the old ones struck through, the new ones highlighted.
            if (s.id == current && shown) {
                if (d.kind != ClipDiff::Kind::Added) {
                    if (const auto t = trackByLabel(*shown, d.trackBefore))
                        preview.removed.push_back({*t, d.before.start, d.before.end()});
                }
                if (d.kind != ClipDiff::Kind::Removed) {
                    if (const auto t = trackByLabel(*shown, d.trackAfter))
                        preview.added.push_back({*t, d.after.start, d.after.end()});
                }
            }
        }
        QStringList counts;
        if (added > 0)
            counts << tr("%n added", nullptr, added);
        if (removed > 0)
            counts << tr("%n removed", nullptr, removed);
        if (changed > 0)
            counts << tr("%n changed", nullptr, changed);
        const char* kind = s.kind == SequenceDiff::Kind::Added ? "added" : s.kind == SequenceDiff::Kind::Removed ? "removed" : "changed";
        sequences.append(QVariantMap{{QStringLiteral("key"), sequenceKey(s.id)},
                                     {QStringLiteral("name"), text(s.name)},
                                     {QStringLiteral("kind"), QString::fromLatin1(kind)},
                                     {QStringLiteral("countsText"), counts.join(QStringLiteral(" · "))},
                                     {QStringLiteral("settings"), textList(s.settings)},
                                     {QStringLiteral("current"), s.id == current},
                                     {QStringLiteral("clips"), clips}});
    }
    QStringList media;
    if (!diff.mediaAdded.empty())
        media << tr("%n media item(s) added", nullptr, static_cast<int>(diff.mediaAdded.size()));
    if (!diff.mediaRemoved.empty())
        media << tr("%n media item(s) removed", nullptr, static_cast<int>(diff.mediaRemoved.size()));
    if (!diff.mediaChanged.empty())
        media << tr("%n media item(s) changed", nullptr, static_cast<int>(diff.mediaChanged.size()));
    QStringList notes;
    if (diff.reviewNotesAdded > 0)
        notes << tr("%n note(s) added", nullptr, static_cast<int>(diff.reviewNotesAdded));
    if (diff.reviewNotesRemoved > 0)
        notes << tr("%n note(s) removed", nullptr, static_cast<int>(diff.reviewNotesRemoved));
    if (diff.reviewNotesChanged > 0)
        notes << tr("%n note(s) changed", nullptr, static_cast<int>(diff.reviewNotesChanged));
    m_comparison = {{QStringLiteral("from"), from},
                    {QStringLiteral("to"), to},
                    {QStringLiteral("fromText"), fromText},
                    {QStringLiteral("toText"), toText},
                    {QStringLiteral("empty"), diff.empty()},
                    {QStringLiteral("summary"), textList(diff.summary())},
                    {QStringLiteral("sequences"), sequences},
                    {QStringLiteral("mediaText"), media.join(QStringLiteral(" · "))},
                    {QStringLiteral("notesText"), notes.join(QStringLiteral(" · "))},
                    {QStringLiteral("stale"), false}};
    emit comparisonChanged();
    clearPreview();
    if (!preview.removed.empty() || !preview.added.empty()) {
        for (TimelineView* view : timelineViews(m_controller))
            view->setExternalPreview(preview);
        m_previewShown = true;
    }
    return {};
}

void HistoryBridge::clearPreview() {
    if (!m_previewShown)
        return;
    m_previewShown = false;
    for (TimelineView* view : timelineViews(m_controller))
        view->clearExternalPreview();
}

void HistoryBridge::clearComparison() {
    clearPreview();
    if (m_comparison.isEmpty())
        return;
    m_comparison.clear();
    emit comparisonChanged();
}

} // namespace wabcut::app
