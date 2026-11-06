// RenderReplaceBridge (see the header): Render and Replace for nested sequences, Restore Unrendered.

#include "RenderReplaceBridge.h"

#include "ExportController.h"
#include "ExportInternal.h"
#include "ProjectController.h"

#include <wabcut/editor/MediaImport.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/RenderReplace.h>
#include <wabcut/model/Timecode.h>

#include <QDir>
#include <QFileInfo>
#include <QQmlEngine>
#include <QSettings>

#include <algorithm>
#include <cmath>

namespace wabcut::app {

namespace {

constexpr const char* kPresetKey = "renderReplace/preset";
constexpr const char* kHandleKey = "renderReplace/handleSeconds";
constexpr double kMaxHandleSeconds = 60.0;

const QStringList& commandIds() {
    static const QStringList ids{QStringLiteral("clip.renderAndReplace"), QStringLiteral("clip.restoreUnrendered")};
    return ids;
}

/// The default format: an edit-friendly intermediate.
constexpr const char* kDefaultPreset = "prores-422-hq";
constexpr const char* kAlphaPreset = "prores-4444-alpha";
constexpr const char* kSoundPreset = "audio-wav";

Ticks handleTicks(double seconds) {
    const double clamped = std::clamp(std::isfinite(seconds) ? seconds : 0.0, 0.0, kMaxHandleSeconds);
    return Ticks{static_cast<std::int64_t>(std::llround(clamped * static_cast<double>(kTicksPerSecond)))};
}

QString timecodeText(const Sequence& sequence, Ticks t) {
    return QString::fromStdString(ticksToTimecode(t, sequence.frameRate, sequence.dropFrame));
}

std::filesystem::path pathOf(const QString& text) {
    return std::filesystem::path(text.toStdU16String());
}

QString textOf(const std::filesystem::path& path) {
    return QDir::toNativeSeparators(QString::fromStdU16String(path.u16string()));
}

/// The sequences `id` plays, itself included, as the project has them now.
void collectSources(const ProjectState& state, SequenceId id, std::vector<std::pair<SequenceId, SequencePtr>>& out) {
    for (const auto& [seen, ptr] : out) {
        if (seen == id)
            return;
    }
    const SequencePtr seq = state.sequencePtr(id);
    if (!seq)
        return;
    out.emplace_back(id, seq);
    for (const SequenceId inner : nestedSequencesOf(*seq))
        collectSources(state, inner, out);
}

} // namespace

RenderReplaceBridge::RenderReplaceBridge(QObject* parent) : QObject(parent) {
    QSettings settings;
    m_presetId = settings.value(QLatin1String(kPresetKey), QLatin1String(kDefaultPreset)).toString();
    m_handleSeconds = std::clamp(settings.value(QLatin1String(kHandleKey), 1.0).toDouble(), 0.0, kMaxHandleSeconds);
    m_poll.setInterval(200);
    connect(&m_poll, &QTimer::timeout, this, &RenderReplaceBridge::pollProgress);
}

RenderReplaceBridge::~RenderReplaceBridge() = default;

RenderReplaceBridge* RenderReplaceBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new RenderReplaceBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setExport(qmlEngine->singletonInstance<ExportController*>("Wabcut", "ExportController"));
    }
    return bridge;
}

void RenderReplaceBridge::setController(ProjectController* controller) {
    m_controller = controller;
}

void RenderReplaceBridge::setExport(ExportController* exporter) {
    if (m_export == exporter)
        return;
    if (m_export)
        disconnect(m_export, nullptr, this, nullptr);
    m_export = exporter;
    if (exporter)
        connect(exporter, &ExportController::jobFinished, this, &RenderReplaceBridge::onJobFinished);
}

QVariantList RenderReplaceBridge::presets() const {
    return {
        QVariantMap{{QStringLiteral("id"), QStringLiteral("prores-422-hq")},
                    {QStringLiteral("name"), tr("ProRes 422 HQ")},
                    {QStringLiteral("note"), tr("Smooth to edit, large files. No transparency.")},
                    {QStringLiteral("alpha"), false}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("dnxhr-hq")},
                    {QStringLiteral("name"), tr("DNxHR HQ")},
                    {QStringLiteral("note"), tr("Smooth to edit, large files. No transparency.")},
                    {QStringLiteral("alpha"), false}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("match-source")},
                    {QStringLiteral("name"), tr("H.264, high quality")},
                    {QStringLiteral("note"), tr("Small files, heavier to scrub. No transparency.")},
                    {QStringLiteral("alpha"), false}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("prores-4444-alpha")},
                    {QStringLiteral("name"), tr("ProRes 4444")},
                    {QStringLiteral("note"), tr("Keeps transparency: for a nest with gaps, titles or graphics on its lowest track.")},
                    {QStringLiteral("alpha"), true}},
    };
}

void RenderReplaceBridge::setPresetId(const QString& id) {
    if (id.isEmpty() || id == m_presetId)
        return;
    m_presetId = id;
    QSettings().setValue(QLatin1String(kPresetKey), id);
    emit settingsChanged();
}

void RenderReplaceBridge::setHandleSeconds(double seconds) {
    const double clamped = std::clamp(std::isfinite(seconds) ? seconds : 0.0, 0.0, kMaxHandleSeconds);
    if (clamped == m_handleSeconds)
        return;
    m_handleSeconds = clamped;
    QSettings().setValue(QLatin1String(kHandleKey), clamped);
    emit settingsChanged();
}

void RenderReplaceBridge::setStatus(const QString& text) {
    m_status = text;
    emit changed();
}

QString RenderReplaceBridge::defaultFolder() const {
    if (!m_controller)
        return {};
    const std::filesystem::path file = m_controller->projectFile();
    if (!file.empty()) {
        const QString dir = textOf(file.parent_path());
        const QString stem = QString::fromStdU16String(file.stem().u16string());
        return QDir::toNativeSeparators(QDir(dir).filePath(stem + QStringLiteral(" Rendered Media")));
    }
    const QString base = m_export ? m_export->defaultFolderFor(QString()) : QDir::homePath();
    return QDir::toNativeSeparators(QDir(base).filePath(QStringLiteral("Rendered Media")));
}

QVariantMap RenderReplaceBridge::inspect(double handleSeconds) const {
    QVariantMap out{{QStringLiteral("ok"), false},
                    {QStringLiteral("reason"), QString()},
                    {QStringLiteral("units"), QVariantList()},
                    {QStringLiteral("preset"), m_presetId},
                    {QStringLiteral("folder"), defaultFolder()}};
    if (!m_controller) {
        out[QStringLiteral("reason")] = tr("There is no project open");
        return out;
    }
    const ProjectSnapshot snapshot = m_controller->snapshot();
    const auto plan = edit::planRenderAndReplace(*snapshot, m_controller->currentSequenceId(), m_controller->selection(),
                                                 handleTicks(handleSeconds));
    if (!plan) {
        out[QStringLiteral("reason")] = QString::fromStdString(plan.error().message);
        return out;
    }
    QVariantList units;
    bool transparent = false;
    for (const edit::NestRenderUnit& unit : *plan) {
        const Sequence* nested = snapshot->findSequence(unit.nested);
        if (!nested)
            continue;
        const bool mayBeTransparent = unit.video && edit::nestMayBeTransparent(*snapshot, unit.nested, unit.range);
        transparent = transparent || mayBeTransparent;
        units.append(QVariantMap{
            {QStringLiteral("name"), QString::fromStdString(nested->name)},
            {QStringLiteral("clips"), static_cast<int>(unit.clips.size())},
            {QStringLiteral("kind"), unit.video && unit.audio ? tr("Picture and sound") : unit.video ? tr("Picture") : tr("Sound")},
            {QStringLiteral("usedText"), tr("%1 to %2").arg(timecodeText(*nested, unit.used.start), timecodeText(*nested, unit.used.end()))},
            {QStringLiteral("rangeText"), tr("%1 to %2").arg(timecodeText(*nested, unit.range.start), timecodeText(*nested, unit.range.end()))},
            {QStringLiteral("transparent"), mayBeTransparent}});
    }
    out[QStringLiteral("ok")] = !units.isEmpty();
    out[QStringLiteral("units")] = units;
    if (transparent)
        out[QStringLiteral("preset")] = QLatin1String(kAlphaPreset);
    return out;
}

bool RenderReplaceBridge::start(const QString& presetId, double handleSeconds, const QString& folder) {
    const auto refuse = [&](const QString& text) {
        setStatus(text);
        emit notice(text, true);
        return false;
    };
    if (!m_controller || !m_export)
        return refuse(tr("Rendering needs the export queue"));
    const ProjectSnapshot snapshot = m_controller->snapshot();
    const SequenceId outer = m_controller->currentSequenceId();
    const auto plan = edit::planRenderAndReplace(*snapshot, outer, m_controller->selection(), handleTicks(handleSeconds));
    if (!plan)
        return refuse(QString::fromStdString(plan.error().message));
    // The same clip is not rendered twice at once.
    for (const edit::NestRenderUnit& unit : *plan) {
        for (const auto& [id, pending] : m_pending) {
            for (const ClipId clip : unit.clips) {
                if (std::ranges::find(pending.clips, clip) != pending.clips.end())
                    return refuse(tr("These clips are being rendered already"));
            }
        }
    }
    setPresetId(presetId);
    setHandleSeconds(handleSeconds);
    const QUrl chosen(folder);
    const std::filesystem::path target =
        pathOf(folder.isEmpty() ? defaultFolder() : chosen.isLocalFile() ? chosen.toLocalFile() : folder);
    std::vector<std::pair<QString, Pending>> queued;
    for (const edit::NestRenderUnit& unit : *plan) {
        const Sequence* nested = snapshot->findSequence(unit.nested);
        if (!nested)
            continue;
        QVariantMap overrides;
        QString preset = presetId;
        if (!unit.video) {
            preset = QLatin1String(kSoundPreset); // sound only: a WAV, whatever picture format was chosen
        } else if (!unit.audio) {
            overrides.insert(QStringLiteral("audio"), false);
        }
        const QString name = QString::fromStdString(nested->name);
        const QString base = exportdetail::safeFileName(name + QStringLiteral(" (rendered)"));
        auto job = m_export->queueSequenceRange(unit.nested, unit.range, preset, overrides, target, base);
        if (!job) {
            for (const auto& [jobId, pending] : queued)
                m_export->cancel(jobId); // nothing of a refused batch stays queued
            return refuse(tr("%1 cannot be rendered: %2").arg(name, ExportController::plainError(job.error())));
        }
        Pending pending;
        pending.outer = outer;
        pending.nested = unit.nested;
        pending.clips = unit.clips;
        pending.origin = unit.range.start;
        pending.output = job->output;
        pending.name = name;
        collectSources(*snapshot, unit.nested, pending.sources);
        queued.emplace_back(job->jobId, std::move(pending));
    }
    const QString first = queued.empty() ? QString() : queued.front().second.name;
    const int count = static_cast<int>(queued.size());
    for (auto& [jobId, pending] : queued)
        m_pending.emplace(jobId, std::move(pending));
    m_progress = 0.0;
    m_poll.start();
    setStatus(count == 1 ? tr("Rendering %1…").arg(first) : tr("Rendering %n nest(s)…", "", count));
    return count > 0;
}

void RenderReplaceBridge::cancel() {
    if (!m_export)
        return;
    const std::vector<QString> jobs = [&] {
        std::vector<QString> ids;
        for (const auto& [jobId, pending] : m_pending)
            ids.push_back(jobId);
        return ids;
    }();
    for (const QString& jobId : jobs)
        m_export->cancel(jobId); // each reports through jobFinished
}

void RenderReplaceBridge::clearStatus() {
    if (!m_pending.empty() || m_status.isEmpty())
        return;
    setStatus({});
}

QString RenderReplaceBridge::localPath(const QUrl& url) const {
    return url.isLocalFile() ? QDir::toNativeSeparators(url.toLocalFile()) : QDir::toNativeSeparators(url.toString());
}

void RenderReplaceBridge::pollProgress() {
    if (m_pending.empty()) {
        m_poll.stop();
        return;
    }
    double sum = 0.0;
    for (const auto& [jobId, pending] : m_pending) {
        const ExportJobModel::Row* row = m_export ? m_export->jobs()->find(jobId) : nullptr;
        sum += row ? std::clamp(row->progress, 0.0, 1.0) : 0.0;
    }
    const double next = sum / static_cast<double>(m_pending.size());
    if (next != m_progress) {
        m_progress = next;
        emit changed();
    }
}

void RenderReplaceBridge::onJobFinished(const QString& jobId, bool succeeded, const QString& message) {
    const auto it = m_pending.find(jobId);
    if (it == m_pending.end())
        return;
    const Pending unit = std::move(it->second);
    m_pending.erase(it);
    if (m_pending.empty()) {
        m_poll.stop();
        m_progress = 0.0;
    }
    if (!succeeded) {
        const QString text = tr("Rendering %1 stopped (%2): its clips still play the nest.").arg(unit.name, message);
        setStatus(text);
        emit notice(text, true);
        return;
    }
    finishUnit(unit);
}

void RenderReplaceBridge::finishUnit(const Pending& unit) {
    const auto fail = [&](const QString& text) {
        setStatus(text);
        emit notice(text, true);
    };
    if (!m_controller)
        return;
    const ProjectSnapshot snapshot = m_controller->snapshot();
    // The file shows the nest as it was when the render began: when the nest (or anything in it) was edited meanwhile, the
    // clips keep playing the live nest - the file is out of date. It stays where it was made.
    for (const auto& [id, was] : unit.sources) {
        if (snapshot->sequencePtr(id) != was) {
            fail(tr("%1 changed while it was rendering, so its clips were not replaced. The file is in %2.")
                     .arg(unit.name, textOf(unit.output.parent_path())));
            return;
        }
    }
    auto item = editor::describeMediaFile(unit.output, m_controller->projectFile());
    if (!item) {
        fail(tr("The file of %1 cannot be read: its clips still play the nest.").arg(unit.name));
        return;
    }
    item->name = unit.name.toStdString() + " (rendered)";
    const bool ok = m_controller->applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Render and Replace"),
                                              [&](const ProjectState& state) {
                                                  return edit::renderAndReplace(state, unit.outer, unit.clips, *item, unit.origin);
                                              });
    if (!ok) {
        fail(tr("%1 was rendered, but its clips could not be replaced: %2").arg(unit.name, m_controller->lastError()));
        return;
    }
    const QString text = tr("%1 is a file now. Clip > Restore Unrendered brings the nest back.").arg(unit.name);
    setStatus(text);
    emit notice(text, false);
}

bool RenderReplaceBridge::restore() {
    if (!m_controller)
        return false;
    const SequenceId outer = m_controller->currentSequenceId();
    const std::vector<ClipId> clips = m_controller->selection();
    const bool ok = m_controller->applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Restore Unrendered"),
                                              [&](const ProjectState& state) { return edit::restoreUnrendered(state, outer, clips); });
    const QString text = ok ? tr("The nest plays again.") : m_controller->lastError();
    setStatus(text);
    emit notice(text, !ok);
    return ok;
}

bool RenderReplaceBridge::selectionProblem(QString* reason) const {
    if (!m_controller) {
        *reason = tr("There is no project open");
        return true;
    }
    if (m_controller->selection().empty()) {
        *reason = tr("Select a clip first");
        return true;
    }
    return false;
}

bool RenderReplaceBridge::handlesCommand(const QString& commandId) {
    return commandIds().contains(commandId);
}

QVariantMap RenderReplaceBridge::commandAvailability(const QString& commandId) const {
    const auto state = [](bool enabled, const QString& reason = {}) {
        return QVariantMap{{QStringLiteral("handled"), true}, {QStringLiteral("enabled"), enabled}, {QStringLiteral("reason"), reason}};
    };
    if (!handlesCommand(commandId))
        return {{QStringLiteral("handled"), false}};
    QString reason;
    if (selectionProblem(&reason))
        return state(false, reason);
    if (commandId == QLatin1String("clip.restoreUnrendered")) {
        const SequencePtr seq = m_controller->currentSequence();
        const bool any = seq && std::ranges::any_of(m_controller->selection(), [&](ClipId id) {
            const auto found = seq->findClip(id);
            return found && edit::isRenderedNest(found->clip());
        });
        return any ? state(true) : state(false, tr("Select a clip that was rendered and replaced first"));
    }
    if (running())
        return state(false, tr("A render is running"));
    const auto plan = edit::planRenderAndReplace(*m_controller->snapshot(), m_controller->currentSequenceId(),
                                                 m_controller->selection(), Ticks{});
    return plan ? state(true) : state(false, QString::fromStdString(plan.error().message));
}

bool RenderReplaceBridge::runCommand(const QString& commandId) {
    if (!handlesCommand(commandId))
        return false;
    const QVariantMap available = commandAvailability(commandId);
    if (!available.value(QStringLiteral("enabled")).toBool()) {
        emit notice(available.value(QStringLiteral("reason")).toString(), true);
        return true;
    }
    if (commandId == QLatin1String("clip.restoreUnrendered"))
        restore();
    else
        emit dialogRequested();
    return true;
}

} // namespace wabcut::app
