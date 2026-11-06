#include "PacingBridge.h"

#include "BridgeIds.h"
#include "ProjectController.h"
#include "TranscriptBridge.h"

#include <wabcut/editor/Activity.h>
#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/EditOps.h>

#include <QCoreApplication>
#include <QMetaObject>
#include <QQmlEngine>
#include <QThreadPool>

#include <algorithm>
#include <cmath>

namespace wabcut::app {

namespace {

constexpr int kPauseMs = 300; ///< after an edit, how long the strip waits before it grades again

const char* reasonKey(std::uint8_t bit) {
    switch (bit) {
    case kPacingLongShot:
        return "longShot";
    case kPacingStill:
        return "still";
    case kPacingSilence:
        return "silence";
    case kPacingSlowSpeech:
        return "slowSpeech";
    default:
        return "";
    }
}

} // namespace

PacingBridge::PacingBridge(QObject* parent) : QObject(parent) {
    m_timer.setSingleShot(true);
    m_timer.setInterval(kPauseMs);
    connect(&m_timer, &QTimer::timeout, this, &PacingBridge::recompute);
}

PacingBridge::~PacingBridge() {
    cancelMeasuring();
}

PacingBridge* PacingBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new PacingBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setTranscripts(qmlEngine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript"));
    }
    return bridge;
}

void PacingBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::revisionChanged, this, &PacingBridge::schedule);
        connect(controller, &ProjectController::sequenceChanged, this, &PacingBridge::schedule);
    }
    schedule();
}

void PacingBridge::setTranscripts(TranscriptBridge* transcripts) {
    if (m_transcripts == transcripts)
        return;
    if (m_transcripts)
        disconnect(m_transcripts, nullptr, this, nullptr);
    m_transcripts = transcripts;
    if (transcripts) {
        // The words arrive (or change) after a transcription; the strip grades the speech again.
        connect(transcripts, &TranscriptBridge::transcriptChanged, this, &PacingBridge::schedule);
    }
}

void PacingBridge::setMeasure(Measure measure) {
    cancelMeasuring();
    m_measure = std::move(measure);
    m_failed.clear();
    schedule();
}

void PacingBridge::setActive(bool on) {
    if (on == m_active)
        return;
    m_active = on;
    emit activeChanged();
    if (on) {
        schedule();
    } else {
        m_timer.stop();
        cancelMeasuring();
        setStatus(false, 0.0, QString());
    }
}

void PacingBridge::schedule() {
    if (m_active)
        m_timer.start(); // restarts the pause: a burst of edits is graded once
}

void PacingBridge::refreshNow() {
    m_timer.stop();
    recompute();
}

std::string PacingBridge::keyOf(const ProjectState& state, MediaId media) const {
    const MediaItem* item = state.findMedia(media);
    return std::to_string(media.value) + "|" + (item ? item->locator.path : std::string());
}

const MediaActivity* PacingBridge::cachedActivity(const ProjectState& state, MediaId media) const {
    const auto it = m_activity.find(keyOf(state, media));
    return it != m_activity.end() ? it->second.get() : nullptr;
}

void PacingBridge::recompute() {
    if (!m_active || !m_controller)
        return;
    const ProjectSnapshot snapshot = m_controller->snapshot();
    const SequencePtr sequence = m_controller->currentSequence();
    if (!snapshot || !sequence) {
        m_stretches.clear();
        m_items.clear();
        emit stretchesChanged();
        return;
    }
    PacingInputs inputs = pacingInputsOf(*sequence);
    // The words, from the transcript the Transcript panel keeps up to date: the lowest audio track that has some (the same
    // dialogue often sits on a second track too, which would count every word twice).
    if (m_transcripts) {
        const auto words = m_transcripts->sequenceTranscript().words();
        std::uint16_t lowest = 0xFFFF;
        for (const auto& w : words)
            lowest = std::min(lowest, w.trackOrder);
        for (const auto& w : words) {
            if (w.trackOrder == lowest)
                inputs.words.push_back(w.timeline.start);
        }
        std::sort(inputs.words.begin(), inputs.words.end());
    }
    const ProjectState& state = *snapshot;
    std::vector<MediaId> missing = addPacingActivity(inputs, state, sequence->id, [&](MediaId id) { return cachedActivity(state, id); });
    // A file measured in vain stays unknown; it is not asked for again.
    std::erase_if(missing, [&](MediaId id) { return m_failed.contains(keyOf(state, id)); });
    const auto cells = pacingCells(inputs);
    m_stretches = pacingStretches(cells, inputs.end);
    m_end = inputs.end;
    m_items.clear();
    for (std::size_t i = 0; i < m_stretches.size(); ++i)
        m_items << describe(m_stretches[i], static_cast<int>(i));
    emit stretchesChanged();
    startMeasuring(snapshot, missing);
}

QVariantMap PacingBridge::describe(const PacingStretch& s, int index) const {
    const auto seconds = [](double v) { return QString::number(static_cast<int>(std::lround(v))); };
    QStringList reasons;
    QStringList lines;
    const QString start = m_controller ? m_controller->formatTimecode(s.range.start) : QString();
    const QString end = m_controller ? m_controller->formatTimecode(s.range.end()) : QString();
    const QString headline = s.level >= 3 ? tr("Likely to drag") : s.level == 2 ? tr("Slow") : tr("Easing off");
    lines << tr("%1 · %2 to %3").arg(headline, start, end);
    QString tryThis;
    if (s.reasons & kPacingLongShot) {
        lines << tr("The same shot runs %1 s").arg(seconds(s.longestShot));
        tryThis = tr("Try a cut, a cutaway or B-roll here");
    }
    if (s.reasons & kPacingStill) {
        lines << tr("Little moves in the picture");
        if (tryThis.isEmpty())
            tryThis = tr("Try a cut, a cutaway or B-roll here");
    }
    if (s.reasons & kPacingSilence) {
        lines << tr("Silent for %1 s").arg(seconds(s.longestSilence));
        if (tryThis.isEmpty())
            tryThis = tr("Tighten the pause: trim it or speed it up");
    }
    if (s.reasons & kPacingSlowSpeech) {
        lines << tr("Slow speech: %1 words a minute").arg(seconds(s.slowestWpm));
        if (tryThis.isEmpty())
            tryThis = tr("Try a little speed (105 to 110 %) or trim the pauses");
    }
    if (!tryThis.isEmpty())
        lines << tryThis;
    for (const std::uint8_t bit : {kPacingLongShot, kPacingStill, kPacingSilence, kPacingSlowSpeech}) {
        if (s.reasons & bit)
            reasons << QString::fromLatin1(reasonKey(bit));
    }
    return {
        {QStringLiteral("index"), index},
        {QStringLiteral("startSec"), s.range.start.toSeconds()},
        {QStringLiteral("endSec"), s.range.end().toSeconds()},
        {QStringLiteral("level"), s.level},
        {QStringLiteral("score"), s.peak},
        {QStringLiteral("reasons"), reasons},
        {QStringLiteral("headline"), headline},
        {QStringLiteral("startText"), start},
        {QStringLiteral("endText"), end},
        {QStringLiteral("text"), lines.join(QLatin1Char('\n'))},
    };
}

QString PacingBridge::summary() const {
    if (m_items.isEmpty())
        return m_busy ? tr("Grading the pacing…") : tr("No stretch that is likely to drag");
    int drags = 0;
    for (const PacingStretch& s : m_stretches)
        drags += s.level >= 3 ? 1 : 0;
    return tr("%n stretch(es) may lose a viewer, %1 of them likely to drag", "", static_cast<int>(m_stretches.size())).arg(drags);
}

int PacingBridge::stretchAt(double seconds) const {
    for (std::size_t i = 0; i < m_stretches.size(); ++i) {
        const TimeRange& r = m_stretches[i].range;
        if (seconds >= r.start.toSeconds() && seconds < r.end().toSeconds())
            return static_cast<int>(i);
    }
    return -1;
}

QVariantMap PacingBridge::stretch(int index) const {
    return index >= 0 && index < m_items.size() ? m_items[index].toMap() : QVariantMap();
}

bool PacingBridge::markStretch(int index) {
    if (!m_controller || index < 0 || index >= static_cast<int>(m_stretches.size()))
        return false;
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq)
        return false;
    const TimeRange range = m_stretches[static_cast<std::size_t>(index)].range;
    const SequenceId id = seq->id;
    const Ticks last = range.end() - framesToTicks(1, seq->frameRate);
    const bool ok = m_controller->applyEditFn("Mark Pacing Stretch", [&](const ProjectState& state) -> edit::EditOutcome {
        auto in = edit::markIn(state, id, range.start);
        if (!in)
            return in;
        auto out = edit::markOut(in->state, id, std::max(range.start, last));
        if (out)
            out->changes = diff(state, out->state);
        return out;
    });
    if (ok)
        m_controller->setPlayhead(range.start);
    return ok;
}

bool PacingBridge::selectStretch(int index) {
    if (!m_controller || index < 0 || index >= static_cast<int>(m_stretches.size()))
        return false;
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq)
        return false;
    const TimeRange range = m_stretches[static_cast<std::size_t>(index)].range;
    std::vector<ClipId> clips;
    seq->forEachTrack([&](const Track& track) {
        for (const Clip& clip : track.clips) {
            if (clip.timelineRange.overlaps(range))
                clips.push_back(clip.id);
        }
    });
    if (clips.empty())
        return false;
    m_controller->setSelection(std::move(clips));
    m_controller->setPlayhead(range.start);
    return true;
}

bool PacingBridge::addMarker(int index) {
    if (!m_controller || index < 0 || index >= static_cast<int>(m_stretches.size()))
        return false;
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq)
        return false;
    const QVariantMap item = stretch(index);
    Marker marker;
    marker.start = snapToFrame(m_stretches[static_cast<std::size_t>(index)].range.start, seq->frameRate);
    marker.duration = m_stretches[static_cast<std::size_t>(index)].range.duration;
    marker.name = item.value(QStringLiteral("headline")).toString().toStdString();
    marker.comment = item.value(QStringLiteral("text")).toString().toStdString();
    if (marker.comment.size() > kMaxMarkerTextBytes)
        marker.comment.resize(kMaxMarkerTextBytes);
    marker.colorIndex = m_stretches[static_cast<std::size_t>(index)].level >= 3 ? 1 : 3; // red, orange
    const SequenceId id = seq->id;
    return m_controller->applyEditFn("Add Pacing Marker",
                                     [&](const ProjectState& state) { return edit::addMarker(state, id, marker); });
}

// ---- Measuring ------------------------------------------------------------------------------------------------------

void PacingBridge::cancelMeasuring() {
    if (m_cancel)
        m_cancel->store(true);
    m_cancel.reset();
    ++m_job; // answers of the pass that was cancelled are ignored
    m_measuring = false;
}

void PacingBridge::setStatus(bool busy, double progress, const QString& status) {
    if (busy == m_busy && progress == m_progress && status == m_status)
        return;
    m_busy = busy;
    m_progress = progress;
    m_status = status;
    emit statusChanged();
}

void PacingBridge::startMeasuring(const ProjectSnapshot& snapshot, const std::vector<MediaId>& missing) {
    if (missing.empty()) {
        if (!m_measuring) {
            const int failed = static_cast<int>(m_failed.size());
            setStatus(false, 1.0, failed > 0 ? tr("%n file(s) could not be measured", "", failed) : QString());
        }
        return;
    }
    if (m_measuring)
        return; // the running pass asks again when it is done
    if (!m_measure) {
        const std::filesystem::path projectFile = m_controller ? m_controller->projectFile() : std::filesystem::path();
        m_measure = editor::activityMeasure(projectFile);
    }
    m_measuring = true;
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    const std::uint64_t job = ++m_job;
    const int total = static_cast<int>(missing.size());
    setStatus(true, 0.0, tr("Measuring footage: %1 of %2").arg(1).arg(total));
    // Keys are made here (the worker must not read the bridge): media id and file.
    std::vector<std::pair<MediaId, std::string>> work;
    for (const MediaId id : missing)
        work.emplace_back(id, keyOf(*snapshot, id));
    QPointer<PacingBridge> self(this);
    const Measure measure = m_measure;
    const std::shared_ptr<std::atomic<bool>> cancel = m_cancel;
    QThreadPool::globalInstance()->start([self, snapshot, work, measure, cancel, job, total] {
        int done = 0;
        for (const auto& item : work) {
            if (cancel->load())
                break;
            const MediaId media = item.first;
            const std::string key = item.second;
            const auto stop = [&cancel](double) { return !cancel->load(); };
            auto result = measure(*snapshot, media, stop);
            if (cancel->load())
                break;
            const bool failed = !result.has_value();
            std::shared_ptr<const MediaActivity> activity;
            if (result)
                activity = std::make_shared<const MediaActivity>(std::move(*result));
            ++done;
            if (self) {
                QMetaObject::invokeMethod(
                    self.data(), [self, job, key, activity, failed, done, total] {
                        if (self)
                            self->measured(job, key, activity, failed, done, total);
                    },
                    Qt::QueuedConnection);
            }
        }
        if (self) {
            QMetaObject::invokeMethod(
                self.data(), [self, job] {
                    if (self)
                        self->finished(job);
                },
                Qt::QueuedConnection);
        }
    });
}

void PacingBridge::measured(std::uint64_t job, const std::string& key, std::shared_ptr<const MediaActivity> activity, bool failed, int done,
                            int total) {
    if (job != m_job)
        return;
    if (failed || !activity)
        m_failed.insert(key);
    else
        m_activity[key] = std::move(activity);
    setStatus(true, static_cast<double>(done) / std::max(1, total), tr("Measuring footage: %1 of %2").arg(std::min(done + 1, total)).arg(total));
    recompute(); // the strip fills in as each file is done
}

void PacingBridge::finished(std::uint64_t job) {
    if (job != m_job)
        return;
    m_measuring = false;
    const int failed = static_cast<int>(m_failed.size());
    setStatus(false, 1.0, failed > 0 ? tr("%n file(s) could not be measured", "", failed) : QString());
    recompute(); // anything the edit added meanwhile
}

} // namespace wabcut::app
