#include "MarkersBridge.h"

#include "BridgeIds.h"
#include "ProjectController.h"

#include <wabcut/command/Engine.h>
#include <wabcut/command/SourceServices.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/Timecode.h>

#include <QFile>
#include <QFileInfo>
#include <QQmlEngine>

#include <algorithm>

namespace wabcut::app {

MarkersBridge::MarkersBridge(QObject* parent) : QObject(parent) {
    // What onPlayhead() compares with: the markers under the playhead as of the last listing.
    connect(this, &MarkersBridge::markersChanged, this, [this] { m_atPlayhead = markersAtPlayhead(); });
}

MarkersBridge* MarkersBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new MarkersBridge;
    if (qmlEngine)
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    return bridge;
}

void MarkersBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::revisionChanged, this, &MarkersBridge::onRevision);
        connect(controller, &ProjectController::sequenceChanged, this, &MarkersBridge::markersChanged);
        connect(controller, &ProjectController::playheadChanged, this, &MarkersBridge::onPlayhead);
        connect(controller, &ProjectController::selectionChanged, this, [this] {
            if (m_clipScope)
                emit markersChanged();
        });
    }
    emit markersChanged();
}

void MarkersBridge::onRevision() {
    if (!m_controller)
        return;
    // P.1: every edit used to re-list every marker (the panel's whole model) whatever it changed.
    const TimelineChange& change = m_controller->lastChange();
    bool relevant = change.structural || !change.changes || change.markersChanged;
    if (!relevant && m_clipScope) {
        // The clip's name and its media's markers (setMediaMarkers changes the media item).
        const Scoped s = scoped();
        const ClipId clips[] = {m_controller->primaryClip()};
        const MediaId media[] = {s.media};
        relevant = change.touchesAny(clips, media);
    }
    if (const SequenceChange* sc = !relevant ? change.sequenceChange() : nullptr; sc && sc->settingsChanged)
        relevant = true; // the frame rate or drop-frame display: the timecodes
    if (relevant)
        emit markersChanged();
}

void MarkersBridge::onPlayhead() {
    // Only `atPlayhead` depends on the playhead: playback no longer re-lists the markers every frame.
    if (markersAtPlayhead() != m_atPlayhead)
        emit markersChanged();
}

std::vector<int> MarkersBridge::markersAtPlayhead() const {
    std::vector<int> out;
    if (!m_controller || m_clipScope)
        return out;
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq)
        return out;
    const Ticks playhead = m_controller->playhead();
    for (std::size_t i = 0; i < seq->markers.size(); ++i) {
        const Marker& m = seq->markers[i];
        if (m.start <= playhead && playhead < std::max(m.end(), m.start + Ticks{1}))
            out.push_back(static_cast<int>(i));
    }
    return out;
}

void MarkersBridge::setMessage(const QString& message) {
    m_message = message;
    emit messageChanged();
}

// ---- Scope and listing ------------------------------------------------------------------------------------------------

MarkersBridge::Scoped MarkersBridge::scoped() const {
    Scoped out;
    if (!m_controller)
        return out;
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq)
        return out;
    if (!m_clipScope) {
        out.markers = &seq->markers; // the snapshot keeps the sequence alive while this call runs
        return out;
    }
    const ClipId primary = m_controller->primaryClip();
    const auto loc = primary ? seq->findClip(primary) : std::nullopt;
    if (!loc)
        return out;
    const MediaItem* media = m_controller->snapshot()->findMedia(loc->clip().media);
    if (!media)
        return out;
    out.markers = &media->markers;
    out.media = media->id;
    out.clip = primary;
    return out;
}

void MarkersBridge::setScope(const QString& scope) {
    const bool clip = scope == QLatin1String("clip");
    if (clip == m_clipScope)
        return;
    m_clipScope = clip;
    emit markersChanged();
}

void MarkersBridge::setColorMask(int mask) {
    mask &= 0xFF;
    if (mask == m_colorMask)
        return;
    m_colorMask = mask;
    emit markersChanged();
}

void MarkersBridge::setSearch(const QString& text) {
    if (text == m_search)
        return;
    m_search = text;
    emit markersChanged();
}

int MarkersBridge::totalCount() const {
    const Scoped s = scoped();
    return s.markers ? static_cast<int>(s.markers->size()) : 0;
}

QString MarkersBridge::clipName() const {
    if (!m_controller || !m_clipScope)
        return {};
    const SequencePtr seq = m_controller->currentSequence();
    const auto loc = seq && m_controller->primaryClip() ? seq->findClip(m_controller->primaryClip()) : std::nullopt;
    return loc ? QString::fromStdString(loc->clip().name()) : QString();
}

QVariantList MarkersBridge::items() const {
    return list(true);
}

QVariantMap MarkersBridge::marker(int index) const {
    for (const QVariant& item : list(false)) {
        if (item.toMap().value(QStringLiteral("index")).toInt() == index)
            return item.toMap();
    }
    return {};
}

QVariantList MarkersBridge::list(bool filtered) const {
    QVariantList out;
    const Scoped s = scoped();
    if (!s.markers || !m_controller)
        return out;
    const ProjectSnapshot snap = m_controller->snapshot();
    const MediaItem* media = s.media ? snap->findMedia(s.media) : nullptr;
    const Rate sourceRate = media ? command::sourceTimecodeRate(*snap, *media) : Rate{};
    const auto text = [&](Ticks t) {
        return media ? QString::fromStdString(ticksToTimecode(t, sourceRate, false)) : m_controller->formatTimecode(t);
    };
    const Ticks playhead = m_controller->playhead();
    int index = 0;
    for (const Marker& m : *s.markers) {
        const int i = index++;
        if (filtered && !(m_colorMask & (1 << (((m.colorIndex % 8) + 8) % 8))))
            continue;
        const QString name = QString::fromStdString(m.name);
        const QString comment = QString::fromStdString(m.comment);
        if (filtered && !m_search.isEmpty() && !name.contains(m_search, Qt::CaseInsensitive)
            && !comment.contains(m_search, Qt::CaseInsensitive))
            continue;
        out << QVariantMap{
            {QStringLiteral("index"), i},
            {QStringLiteral("name"), name},
            {QStringLiteral("comment"), comment},
            {QStringLiteral("colorIndex"), m.colorIndex},
            {QStringLiteral("type"), QString::fromLatin1(markerTypeName(m.type).data(),
                                                         static_cast<qsizetype>(markerTypeName(m.type).size()))},
            {QStringLiteral("url"), QString::fromStdString(m.url)},
            {QStringLiteral("target"), QString::fromStdString(m.target)},
            {QStringLiteral("inText"), text(m.start)},
            {QStringLiteral("outText"), text(m.end())},
            {QStringLiteral("durationText"), text(m.duration)},
            {QStringLiteral("ranged"), m.duration > Ticks{}},
            {QStringLiteral("atPlayhead"), !media && m.start <= playhead && playhead < std::max(m.end(), m.start + Ticks{1})},
        };
    }
    return out;
}

// ---- Editing ---------------------------------------------------------------------------------------------------------

bool MarkersBridge::replaceMarkers(const char* label, std::vector<Marker> markers) {
    if (!m_controller)
        return false;
    const Scoped s = scoped();
    const bool ok = m_clipScope
        ? s.media && m_controller->applyEditFn(label, [&](const ProjectState& st) {
              return edit::setMediaMarkers(st, s.media, markers);
          })
        : m_controller->applyEditFn(label, [&](const ProjectState& st) {
              return edit::setSequenceMarkers(st, m_controller->currentSequenceId(), markers);
          });
    if (!ok)
        setMessage(m_controller->lastError());
    return ok;
}

QString MarkersBridge::add(bool timelineFocused) {
    if (!m_controller || !m_controller->currentSequence())
        return {};
    const SequencePtr seq = m_controller->currentSequence();
    const Ticks playhead = m_controller->playhead();
    // Premiere: with a clip selected in the Timeline, M marks the clip.
    if (timelineFocused) {
        for (const ClipId id : m_controller->selection()) {
            const auto loc = seq->findClip(id);
            if (loc && loc->track->kind == TrackKind::Video && loc->clip().start() <= playhead && playhead < loc->clip().end())
                return addClipMarker(clipKey(id)) ? QStringLiteral("clip") : QString();
        }
    }
    Marker marker;
    marker.start = playhead;
    const bool ok = m_controller->applyEditFn("Add Marker", [&](const ProjectState& st) {
        return edit::addMarker(st, seq->id, marker);
    });
    if (!ok)
        setMessage(m_controller->lastError());
    return ok ? QStringLiteral("sequence") : QString();
}

bool MarkersBridge::addClipMarker(const QString& clipId) {
    const auto id = parseClipKey(clipId);
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto loc = id && seq ? seq->findClip(*id) : std::nullopt;
    if (!loc)
        return false;
    const Clip& clip = loc->clip();
    const MediaItem* media = m_controller->snapshot()->findMedia(clip.media);
    if (!media)
        return false;
    const Ticks t = std::clamp(m_controller->playhead(), clip.start(), clip.end() - Ticks{1});
    const Rate rate = command::sourceTimecodeRate(*m_controller->snapshot(), *media);
    Marker marker;
    marker.start = snapToFrame(clip.sourceTimeAt(t), rate);
    std::vector<Marker> markers = media->markers;
    markers.push_back(marker);
    const MediaId mediaId = media->id;
    const bool ok = m_controller->applyEditFn("Add Clip Marker", [&](const ProjectState& st) {
        return edit::setMediaMarkers(st, mediaId, markers);
    });
    if (!ok)
        setMessage(m_controller->lastError());
    return ok;
}

std::optional<Ticks> MarkersBridge::parseTime(const QVariant& value) const {
    if (!m_controller)
        return std::nullopt;
    const QString text = value.toString().trimmed();
    if (m_clipScope) {
        const Scoped s = scoped();
        const ProjectSnapshot snap = m_controller->snapshot();
        const MediaItem* media = s.media ? snap->findMedia(s.media) : nullptr;
        if (!media)
            return std::nullopt;
        const auto t = parseTimecode(text.toStdString(), command::sourceTimecodeRate(*snap, *media), false);
        return t ? std::optional<Ticks>(*t) : std::nullopt;
    }
    return m_controller->parseTimecodeText(text);
}

bool MarkersBridge::update(int index, const QVariantMap& fields) {
    const Scoped s = scoped();
    if (!s.markers || index < 0 || index >= static_cast<int>(s.markers->size()))
        return false;
    std::vector<Marker> markers = *s.markers;
    Marker& m = markers[static_cast<std::size_t>(index)];
    if (fields.contains(QStringLiteral("name")))
        m.name = fields.value(QStringLiteral("name")).toString().toStdString();
    if (fields.contains(QStringLiteral("comment")))
        m.comment = fields.value(QStringLiteral("comment")).toString().toStdString();
    if (fields.contains(QStringLiteral("colorIndex")))
        m.colorIndex = std::clamp(fields.value(QStringLiteral("colorIndex")).toInt(), 0, 15);
    if (fields.contains(QStringLiteral("type"))) {
        const QByteArray type = fields.value(QStringLiteral("type")).toString().toUtf8();
        m.type = markerTypeFromName(std::string_view(type.constData(), static_cast<std::size_t>(type.size())))
                     .value_or(MarkerType::Comment);
    }
    if (fields.contains(QStringLiteral("url")))
        m.url = fields.value(QStringLiteral("url")).toString().trimmed().toStdString();
    if (fields.contains(QStringLiteral("target")))
        m.target = fields.value(QStringLiteral("target")).toString().toStdString();
    if (fields.contains(QStringLiteral("start"))) {
        const auto t = parseTime(fields.value(QStringLiteral("start")));
        if (!t) {
            setMessage(tr("Not a timecode: %1").arg(fields.value(QStringLiteral("start")).toString()));
            return false;
        }
        m.start = *t;
    }
    if (fields.contains(QStringLiteral("duration"))) {
        const auto t = parseTime(fields.value(QStringLiteral("duration")));
        if (!t) {
            setMessage(tr("Not a timecode: %1").arg(fields.value(QStringLiteral("duration")).toString()));
            return false;
        }
        m.duration = *t;
    }
    if (const auto problem = markerProblem(m)) {
        setMessage(QString::fromStdString(*problem));
        return false;
    }
    if (!m_clipScope) {
        const Marker changed = m;
        const bool ok = m_controller->applyEditFn("Edit Marker", [&](const ProjectState& st) {
            return edit::updateMarker(st, m_controller->currentSequenceId(), static_cast<std::size_t>(index), changed);
        });
        if (!ok)
            setMessage(m_controller->lastError());
        return ok;
    }
    return replaceMarkers("Edit Clip Marker", std::move(markers));
}

bool MarkersBridge::moveTo(int index, const QString& timeKey) {
    bool ok = false;
    const qint64 ticks = timeKey.toLongLong(&ok);
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!ok || !seq || index < 0 || index >= static_cast<int>(seq->markers.size()))
        return false;
    Marker m = seq->markers[static_cast<std::size_t>(index)];
    m.start = Ticks{std::max<qint64>(0, ticks)};
    return m_controller->applyEditFn("Move Marker", [&](const ProjectState& st) {
        return edit::updateMarker(st, seq->id, static_cast<std::size_t>(index), m);
    });
}

bool MarkersBridge::setEnd(int index, const QString& timeKey) {
    bool ok = false;
    const qint64 ticks = timeKey.toLongLong(&ok);
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!ok || !seq || index < 0 || index >= static_cast<int>(seq->markers.size()))
        return false;
    Marker m = seq->markers[static_cast<std::size_t>(index)];
    m.duration = Ticks{std::max<qint64>(0, ticks - m.start.value)};
    return m_controller->applyEditFn("Marker Duration", [&](const ProjectState& st) {
        return edit::updateMarker(st, seq->id, static_cast<std::size_t>(index), m);
    });
}

bool MarkersBridge::remove(int index) {
    const Scoped s = scoped();
    if (!s.markers || index < 0 || index >= static_cast<int>(s.markers->size()))
        return false;
    std::vector<Marker> markers = *s.markers;
    markers.erase(markers.begin() + index);
    return replaceMarkers(m_clipScope ? "Remove Clip Marker" : "Remove Marker", std::move(markers));
}

bool MarkersBridge::clearAtPlayhead() {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return false;
    const Ticks playhead = m_controller->playhead();
    for (std::size_t i = 0; i < seq->markers.size(); ++i) {
        if (seq->markers[i].start == playhead) {
            return m_controller->applyEditFn("Clear Selected Marker", [&](const ProjectState& st) {
                return edit::removeMarker(st, seq->id, i);
            });
        }
    }
    setMessage(tr("No marker at the playhead"));
    return false;
}

bool MarkersBridge::clearAll() {
    const Scoped s = scoped();
    if (!s.markers || s.markers->empty())
        return false;
    return replaceMarkers(m_clipScope ? "Clear All Clip Markers" : "Clear All Markers", {});
}

bool MarkersBridge::goToNext() {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return false;
    const Ticks playhead = m_controller->playhead();
    for (const Marker& m : seq->markers) {
        if (m.start > playhead) {
            m_controller->setPlayhead(m.start);
            return true;
        }
    }
    return false;
}

bool MarkersBridge::goToPrevious() {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return false;
    const Ticks playhead = m_controller->playhead();
    for (auto it = seq->markers.rbegin(); it != seq->markers.rend(); ++it) {
        if (it->start < playhead) {
            m_controller->setPlayhead(it->start);
            return true;
        }
    }
    return false;
}

bool MarkersBridge::seek(int index) {
    const Scoped s = scoped();
    if (!s.markers || index < 0 || index >= static_cast<int>(s.markers->size()))
        return false;
    const Marker& m = (*s.markers)[static_cast<std::size_t>(index)];
    if (!m_clipScope) {
        m_controller->setPlayhead(m.start);
        return true;
    }
    const SequencePtr seq = m_controller->currentSequence();
    const auto loc = seq ? seq->findClip(s.clip) : std::nullopt;
    const auto t = loc ? loc->clip().timelineTimeOfCurveTime(m.start) : std::nullopt;
    if (!t)
        return false;
    m_controller->setPlayhead(*t);
    return true;
}

// ---- Export ----------------------------------------------------------------------------------------------------------

QVariantMap MarkersBridge::exportText(const QString& format) const {
    if (!m_controller || !m_controller->currentSequenceId().isValid())
        return {};
    command::EnginePolicy policy;
    policy.allowed = command::CapabilitySet::of({command::Capability::Read});
    command::CommandEngine engine(m_controller->session(), command::builtinCommands(), policy);
    command::CommandRequest request;
    request.command = "sequence.exportMarkers";
    request.params = command::Json{{"format", format.toStdString()}, {"sequence", m_controller->currentSequenceId().value}};
    const command::CommandResult r = engine.execute(request);
    if (!r.ok())
        return {{QStringLiteral("error"), QString::fromStdString(r.error->message)}};
    QStringList warnings;
    for (const auto& w : r.data.value("warnings", command::Json::array()))
        warnings << QString::fromStdString(w.get<std::string>());
    return {{QStringLiteral("text"), QString::fromStdString(r.data.value("text", std::string()))},
            {QStringLiteral("count"), r.data.value("count", 0)},
            {QStringLiteral("warnings"), warnings}};
}

QString MarkersBridge::exportToFile(const QString& format, const QUrl& file) {
    const QVariantMap result = exportText(format);
    if (result.contains(QStringLiteral("error"))) {
        setMessage(result.value(QStringLiteral("error")).toString());
        return {};
    }
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    QFile out(path);
    if (path.isEmpty() || !out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setMessage(tr("Could not write %1").arg(path));
        return {};
    }
    out.write(result.value(QStringLiteral("text")).toString().toUtf8());
    out.close();
    const QStringList warnings = result.value(QStringLiteral("warnings")).toStringList();
    setMessage(warnings.isEmpty() ? tr("Markers exported: %1").arg(QFileInfo(path).fileName())
                                  : tr("Markers exported with warnings: %1").arg(warnings.join(QStringLiteral("; "))));
    return path;
}

bool MarkersBridge::runCommand(const QString& commandId) {
    if (commandId == QLatin1String("marker.goToNext")) {
        goToNext();
        return true;
    }
    if (commandId == QLatin1String("marker.goToPrevious")) {
        goToPrevious();
        return true;
    }
    if (commandId == QLatin1String("marker.clearSelected")) {
        clearAtPlayhead();
        return true;
    }
    if (commandId == QLatin1String("marker.clearAll")) {
        const bool clip = m_clipScope;
        m_clipScope = false; // the key clears the sequence's markers
        clearAll();
        m_clipScope = clip;
        return true;
    }
    return false;
}

} // namespace wabcut::app
