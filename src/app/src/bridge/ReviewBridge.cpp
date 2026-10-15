#include "ReviewBridge.h"

#include "BridgeIds.h"
#include "ProjectController.h"
#include "ReviewFrames.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/Timecode.h>

#include <QDateTime>
#include <QLocale>
#include <QQmlEngine>

#include <algorithm>
#include <cmath>

namespace wabcut::app {

using command::Json;

namespace {

QString text(const std::string& s) { return QString::fromStdString(s); }

QString createdText(const std::string& iso) {
    if (iso.empty())
        return {};
    QDateTime when = QDateTime::fromString(text(iso), Qt::ISODate);
    if (!when.isValid())
        return text(iso);
    when = when.toLocalTime();
    const QString time = QLocale().toString(when.time(), QLocale::ShortFormat);
    const QDate today = QDate::currentDate();
    if (when.date() == today)
        return ReviewBridge::tr("Today %1").arg(time);
    if (when.date() == today.addDays(-1))
        return ReviewBridge::tr("Yesterday %1").arg(time);
    return QLocale().toString(when, QLocale::ShortFormat);
}

QVariantMap markMap(const ReviewMark& m) {
    return {{QStringLiteral("x"), m.x}, {QStringLiteral("y"), m.y}, {QStringLiteral("width"), m.width}, {QStringLiteral("height"), m.height}};
}

/// The note covers `t`: its range, or the frame it is on.
bool covers(const ReviewNote& n, Ticks t, Rate rate) {
    if (n.duration.value > 0)
        return t >= n.start && t < n.end();
    const Ticks frame = rate.isValid() ? framesToTicks(1, rate) : Ticks{kTicksPerSecond / 30};
    return t >= n.start && t < n.start + frame;
}

QString localPath(const QUrl& url) { return url.isLocalFile() ? url.toLocalFile() : url.toString(); }

} // namespace

ReviewBridge::ReviewBridge(QObject* parent) : QObject(parent) {}

ReviewBridge::~ReviewBridge() = default;

ReviewBridge* ReviewBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new ReviewBridge;
    if (qmlEngine)
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    return bridge;
}

void ReviewBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::revisionChanged, this, &ReviewBridge::rebuild);
        connect(controller, &ProjectController::sequenceChanged, this, &ReviewBridge::rebuild);
        connect(controller, &ProjectController::playheadChanged, this, &ReviewBridge::updateCurrent);
    }
    rebuild();
}

std::shared_ptr<command::CommandServices> ReviewBridge::services() {
    if (!m_services) {
        QPointer<ProjectController> controller = m_controller;
        m_services = std::make_shared<command::ReviewServices>(std::make_shared<ReviewFrames>([controller] {
            return controller ? controller->projectFile() : std::filesystem::path{};
        }));
    }
    return m_services;
}

void ReviewBridge::setMessage(const QString& message) {
    m_message = message;
    emit messageChanged();
}

void ReviewBridge::setStatusFilter(const QString& status) {
    const QString next = status == QStringLiteral("open") || status == QStringLiteral("resolved") ? status : QStringLiteral("all");
    if (next == m_status)
        return;
    m_status = next;
    emit filterChanged();
    rebuild();
}

void ReviewBridge::setAllSequences(bool all) {
    if (all == m_allSequences)
        return;
    m_allSequences = all;
    emit filterChanged();
    rebuild();
}

void ReviewBridge::setQuery(const QString& query) {
    if (query == m_query)
        return;
    m_query = query;
    emit filterChanged();
    rebuild();
}

void ReviewBridge::setSelectedNote(int id) {
    if (id == m_selectedNote)
        return;
    m_selectedNote = id;
    emit currentChanged();
    updateCurrent();
}

void ReviewBridge::setDrawing(bool drawing) {
    if (drawing == m_drawing)
        return;
    m_drawing = drawing;
    updateCurrent(); // emits markChanged
    emit markChanged();
}

void ReviewBridge::setAuthor(const QString& author) {
    if (author == m_author)
        return;
    m_author = author;
    emit authorChanged();
}

QString ReviewBridge::authorName() const {
    const QString trimmed = m_author.trimmed();
    return trimmed.isEmpty() ? tr("Editor") : trimmed;
}

void ReviewBridge::rebuild() {
    QVariantList notes;
    int open = 0;
    int resolved = 0;
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const SequenceId current = m_controller ? m_controller->currentSequenceId() : SequenceId{};
    if (snap) {
        std::vector<const ReviewNote*> list;
        for (const ReviewNote& n : snap->reviewNotes) {
            if (!m_allSequences && n.sequence != current)
                continue;
            (n.status == ReviewStatus::Open ? open : resolved) += 1;
            if ((m_status == QStringLiteral("open") && n.status != ReviewStatus::Open) ||
                (m_status == QStringLiteral("resolved") && n.status != ReviewStatus::Resolved))
                continue;
            if (!m_query.trimmed().isEmpty()) {
                const QString q = m_query.trimmed();
                bool hit = text(n.text).contains(q, Qt::CaseInsensitive) || text(n.author).contains(q, Qt::CaseInsensitive);
                for (const ReviewReply& r : n.replies)
                    hit = hit || text(r.text).contains(q, Qt::CaseInsensitive);
                if (!hit)
                    continue;
            }
            list.push_back(&n);
        }
        std::stable_sort(list.begin(), list.end(), [&](const ReviewNote* a, const ReviewNote* b) {
            const bool ca = a->sequence == current;
            const bool cb = b->sequence == current;
            if (ca != cb)
                return ca; // the current sequence's first
            return std::tuple(a->sequence, a->start) < std::tuple(b->sequence, b->start);
        });
        for (const ReviewNote* n : list) {
            const Sequence* seq = snap->findSequence(n->sequence);
            const auto tc = [&](Ticks t) {
                return seq ? text(ticksToTimecode(t, seq->frameRate, seq->dropFrame)) : QString::number(t.value);
            };
            QVariantList replies;
            for (const ReviewReply& r : n->replies) {
                replies.append(QVariantMap{{QStringLiteral("author"), text(r.author)},
                                           {QStringLiteral("text"), text(r.text)},
                                           {QStringLiteral("createdText"), createdText(r.createdAt)}});
            }
            notes.append(QVariantMap{{QStringLiteral("id"), static_cast<qulonglong>(n->id.value)},
                                     {QStringLiteral("sequenceKey"), sequenceKey(n->sequence)},
                                     {QStringLiteral("sequenceName"), seq ? text(seq->name) : QString()},
                                     {QStringLiteral("startText"), tc(n->start)},
                                     {QStringLiteral("endText"), n->duration.value > 0 ? tc(n->end()) : QString()},
                                     {QStringLiteral("range"), n->duration.value > 0},
                                     {QStringLiteral("author"), text(n->author)},
                                     {QStringLiteral("text"), text(n->text)},
                                     {QStringLiteral("createdText"), createdText(n->createdAt)},
                                     {QStringLiteral("status"), text(std::string(toString(n->status)))},
                                     {QStringLiteral("resolved"), n->status == ReviewStatus::Resolved},
                                     {QStringLiteral("hasMark"), n->mark.has_value()},
                                     {QStringLiteral("mark"), n->mark ? markMap(*n->mark) : QVariantMap{}},
                                     {QStringLiteral("replies"), replies},
                                     {QStringLiteral("replyCount"), static_cast<int>(n->replies.size())}});
        }
    }
    if (notes != m_notes || open != m_openCount || resolved != m_resolvedCount) {
        m_notes = notes;
        m_openCount = open;
        m_resolvedCount = resolved;
        emit notesChanged();
    }
    updateCurrent();
}

void ReviewBridge::updateCurrent() {
    int current = 0;
    QVariantMap shown;
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (snap && seq) {
        const Ticks t = m_controller->playhead();
        const ReviewNote* selected = nullptr;
        const ReviewNote* atPlayhead = nullptr;
        for (const ReviewNote& n : snap->reviewNotes) {
            if (n.sequence != seq->id)
                continue;
            if (static_cast<int>(n.id.value) == m_selectedNote)
                selected = &n;
            if (!atPlayhead && covers(n, t, seq->frameRate))
                atPlayhead = &n;
        }
        if (atPlayhead)
            current = static_cast<int>(atPlayhead->id.value);
        // The monitor draws the selected note's mark while the playhead is on it, else the mark at the playhead.
        const ReviewNote* drawn = selected && covers(*selected, t, seq->frameRate) && selected->mark ? selected
                                  : atPlayhead && atPlayhead->mark                                    ? atPlayhead
                                                                                                      : nullptr;
        if (m_drawing && !m_pendingMark.isEmpty()) {
            shown = m_pendingMark;
            shown.insert(QStringLiteral("pending"), true);
            shown.insert(QStringLiteral("resolved"), false);
        } else if (drawn) {
            shown = markMap(*drawn->mark);
            shown.insert(QStringLiteral("pending"), false);
            shown.insert(QStringLiteral("resolved"), drawn->status == ReviewStatus::Resolved);
        }
    }
    if (current != m_currentNote) {
        m_currentNote = current;
        emit currentChanged();
    }
    if (shown != m_shownMark) {
        m_shownMark = shown;
        emit markChanged();
    }
}

QString ReviewBridge::run(const char* command, Json params) {
    if (!m_controller)
        return tr("No project");
    const command::CommandResult result = CommandJob::runNow(m_controller, nullptr, command, std::move(params));
    const QString error = commandErrorText(result);
    if (!error.isEmpty())
        setMessage(error);
    return error;
}

QString ReviewBridge::add(const QString& noteText, bool overInOut) {
    if (!m_controller)
        return tr("No project");
    const QString body = noteText.trimmed();
    if (body.isEmpty())
        return tr("Write the note first");
    Json params{{"text", body.toStdString()}, {"author", authorName().toStdString()}};
    const auto in = m_controller->markIn();
    const auto out = m_controller->markOut();
    if (overInOut && in && out && *out > *in) {
        params["start"] = in->value;
        params["end"] = out->value;
    } else {
        params["time"] = m_controller->playhead().value;
    }
    if (!m_pendingMark.isEmpty()) {
        params["mark"] = Json{{"x", m_pendingMark.value(QStringLiteral("x")).toDouble()},
                              {"y", m_pendingMark.value(QStringLiteral("y")).toDouble()},
                              {"width", m_pendingMark.value(QStringLiteral("width")).toDouble()},
                              {"height", m_pendingMark.value(QStringLiteral("height")).toDouble()}};
    }
    const QString error = run("review.add", std::move(params));
    if (error.isEmpty()) {
        m_pendingMark.clear();
        m_drawing = false;
        emit markChanged();
        setMessage(tr("Note added"));
    }
    rebuild();
    return error;
}

QString ReviewBridge::reply(int id, const QString& replyText) {
    if (replyText.trimmed().isEmpty())
        return tr("Write the reply first");
    return run("review.reply", Json{{"note", id}, {"text", replyText.trimmed().toStdString()}, {"author", authorName().toStdString()}});
}

QString ReviewBridge::resolve(int id, bool resolved) {
    return run("review.resolve", Json{{"note", id}, {"resolved", resolved}});
}

QString ReviewBridge::remove(int id) {
    const QString error = run("review.remove", Json{{"note", id}});
    if (error.isEmpty() && id == m_selectedNote)
        setSelectedNote(0);
    return error;
}

QString ReviewBridge::toMarkers(bool removeNotes) {
    if (!m_controller)
        return tr("No project");
    Json ids = Json::array();
    const SequenceId current = m_controller->currentSequenceId();
    for (const QVariant& v : m_notes) {
        const QVariantMap n = v.toMap();
        if (n.value(QStringLiteral("sequenceKey")).toString() == sequenceKey(current))
            ids.push_back(n.value(QStringLiteral("id")).toULongLong());
    }
    if (ids.empty())
        return tr("No notes of this sequence to turn into markers");
    const int count = static_cast<int>(ids.size());
    const QString error = run("review.toMarkers", Json{{"notes", std::move(ids)}, {"remove", removeNotes}});
    if (error.isEmpty())
        setMessage(tr("%n note(s) turned into markers", nullptr, count));
    return error;
}

bool ReviewBridge::seekToNote(int id) {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    if (!snap)
        return false;
    const auto it = std::find_if(snap->reviewNotes.begin(), snap->reviewNotes.end(),
                                 [&](const ReviewNote& n) { return static_cast<int>(n.id.value) == id; });
    if (it == snap->reviewNotes.end())
        return false;
    if (it->sequence != m_controller->currentSequenceId()) {
        for (std::size_t i = 0; i < snap->sequences.size(); ++i) {
            if (snap->sequences[i]->id == it->sequence)
                m_controller->setCurrentSequenceIndex(static_cast<int>(i));
        }
        if (m_controller->currentSequenceId() != it->sequence)
            return false;
    }
    const Ticks start = it->start;
    setSelectedNote(id);
    m_controller->setPlayhead(start);
    updateCurrent();
    return true;
}

void ReviewBridge::setPendingMark(double x, double y, double width, double height) {
    const auto unit = [](double v) { return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0; };
    x = unit(x);
    y = unit(y);
    width = std::min(unit(width), 1.0 - x);
    height = std::min(unit(height), 1.0 - y);
    m_pendingMark = {{QStringLiteral("x"), x}, {QStringLiteral("y"), y}, {QStringLiteral("width"), width}, {QStringLiteral("height"), height}};
    updateCurrent();
    emit markChanged();
}

void ReviewBridge::clearPendingMark() {
    if (m_pendingMark.isEmpty())
        return;
    m_pendingMark.clear();
    updateCurrent();
    emit markChanged();
}

QString ReviewBridge::exportPackage(const QUrl& folder, const QString& title, bool openOnly) {
    if (!m_controller)
        return tr("No project");
    const QString path = localPath(folder);
    if (path.isEmpty())
        return tr("Choose where to write the package");
    const SequencePtr seq = m_controller->currentSequence();
    Json params{{"path", path.toStdString()},
                {"title", (title.trimmed().isEmpty() ? (seq ? text(seq->name) : tr("Review notes")) : title.trimmed()).toStdString()}};
    if (openOnly)
        params["status"] = "open";
    m_package.clear();
    emit packageChanged();
    QPointer<ReviewBridge> self(this);
    const bool started = m_job.start(this, m_controller, services(), {"review.exportPackage", params, "Export Review Package"}, {},
                                     [self, path](const command::CommandResult& result) {
                                         if (!self)
                                             return;
                                         QStringList warnings;
                                         for (const std::string& w : result.warnings)
                                             warnings << text(w);
                                         QVariantMap out{{QStringLiteral("kind"), QStringLiteral("export")},
                                                         {QStringLiteral("ok"), result.ok()},
                                                         {QStringLiteral("path"), path},
                                                         {QStringLiteral("warnings"), warnings},
                                                         {QStringLiteral("error"), commandErrorText(result)}};
                                         if (result.ok()) {
                                             out.insert(QStringLiteral("notes"), result.data.value("notes", 0));
                                             out.insert(QStringLiteral("frames"), result.data.value("frames", 0));
                                             self->setMessage(tr("Wrote the review package (%n note(s))", nullptr,
                                                                 result.data.value("notes", 0)));
                                         } else {
                                             self->setMessage(commandErrorText(result));
                                         }
                                         self->m_package = out;
                                         emit self->packageChanged();
                                         emit self->busyChanged();
                                     });
    if (!started)
        return tr("A review package is being written");
    emit busyChanged();
    return {};
}

QString ReviewBridge::importPackage(const QUrl& folder) {
    if (!m_controller)
        return tr("No project");
    const QString path = localPath(folder);
    if (path.isEmpty())
        return tr("Choose a review package");
    const command::CommandResult result =
        CommandJob::runNow(m_controller, nullptr, "review.importPackage", Json{{"path", path.toStdString()}});
    QVariantMap out{{QStringLiteral("kind"), QStringLiteral("import")},
                    {QStringLiteral("ok"), result.ok()},
                    {QStringLiteral("path"), path},
                    {QStringLiteral("error"), commandErrorText(result)}};
    if (result.ok()) {
        QStringList conflicts;
        for (const Json& c : result.data.value("conflicts", Json::array()))
            conflicts << text(c.get<std::string>());
        out.insert(QStringLiteral("added"), result.data.value("added", 0));
        out.insert(QStringLiteral("updated"), result.data.value("updated", 0));
        out.insert(QStringLiteral("unchanged"), result.data.value("unchanged", 0));
        out.insert(QStringLiteral("conflicts"), conflicts);
        setMessage(tr("Imported the review package: %1 added, %2 updated")
                       .arg(result.data.value("added", 0))
                       .arg(result.data.value("updated", 0)));
    } else {
        setMessage(commandErrorText(result));
    }
    m_package = out;
    emit packageChanged();
    rebuild();
    return commandErrorText(result);
}

} // namespace wabcut::app
