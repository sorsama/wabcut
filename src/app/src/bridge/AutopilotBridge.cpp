#include "AutopilotBridge.h"

#include "AiController.h"
#include "BridgeIds.h"
#include "GenerateController.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "ShortsBridge.h"
#include "TranscriptBridge.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/Provider.h>
#include <wabcut/ai/analysis/AnalysisCommands.h>
#include <wabcut/ai/analysis/MediaAnalysis.h>
#include <wabcut/ai/autopilot/AutopilotCommands.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/search/MediaSearchAccess.h>
#include <wabcut/export/StillExport.h>

#include <QDesktopServices>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace wabcut::app {

using command::Json;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

QString joined(const std::vector<std::string>& warnings) {
    QStringList lines;
    for (const std::string& w : warnings) {
        lines << qs(w);
    }
    return lines.join(QLatin1Char('\n'));
}

std::filesystem::path projectPath(QQmlEngine* engine) {
    auto* file = engine ? engine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile") : nullptr;
    if (!file || file->filePath().isEmpty()) {
        return {};
    }
    return std::filesystem::path(file->filePath().toStdWString());
}

QString frameUrl(std::uint64_t media, std::int64_t ticks) {
    return QStringLiteral("image://wabcut-frames/%1/%2").arg(mediaKey(MediaId{media})).arg(ticks);
}

/// JSON values as QVariant (objects as maps, arrays as lists).
QVariant variantOf(const Json& j) {
    switch (j.type()) {
    case Json::value_t::object: {
        QVariantMap m;
        for (auto it = j.begin(); it != j.end(); ++it) {
            m.insert(QString::fromStdString(it.key()), variantOf(*it));
        }
        return m;
    }
    case Json::value_t::array: {
        QVariantList l;
        for (const Json& v : j) {
            l.append(variantOf(v));
        }
        return l;
    }
    case Json::value_t::string:
        return QString::fromStdString(j.get<std::string>());
    case Json::value_t::boolean:
        return j.get<bool>();
    case Json::value_t::number_integer:
        return QVariant::fromValue(j.get<qint64>());
    case Json::value_t::number_unsigned:
        return QVariant::fromValue(j.get<quint64>());
    case Json::value_t::number_float:
        return j.get<double>();
    default:
        return {};
    }
}

/// A model the app made on the GUI thread (consent, locks and the spend cap apply to its calls), lent to the worker.
class Borrowed final : public ai::IChatModel {
public:
    explicit Borrowed(std::shared_ptr<ai::IChatModel> model) : m_model(std::move(model)) {}
    const ai::ModelSpec& spec() const override { return m_model->spec(); }
    std::expected<ai::ChatResponse, ai::AiError> chat(const ai::ChatRequest& r, const ai::ChatEventHandler& e, std::stop_token s) override {
        return m_model->chat(r, e, s);
    }

private:
    std::shared_ptr<ai::IChatModel> m_model;
};

} // namespace

AutopilotBridge* AutopilotBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new AutopilotBridge;
    bridge->setEngine(qmlEngine);
    if (qmlEngine) {
        bridge->m_ai = qmlEngine->singletonInstance<::AiController*>("Wabcut", "AiController");
        bridge->stopJobsWith(bridge->m_ai);
        bridge->m_transcripts = qmlEngine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
        (void)qmlEngine->singletonInstance<ShortsBridge*>("Wabcut", "Shorts"); // registers image://wabcut-frames
    }
    return bridge;
}

AutopilotBridge::AutopilotBridge(QObject* parent) : CreatorBase(parent) {
    connect(this, &CreatorBase::resultChanged, this, &AutopilotBridge::refreshOutdated);
    connect(this, &CreatorBase::jobChanged, this, &AutopilotBridge::refreshOutdated);
}

// --- Out of date (fix round 1, ARGON-17) -------------------------------------------------------------------------------

void AutopilotBridge::watchRevision() {
    if (!m_controller || m_watched == m_controller) {
        return;
    }
    if (m_watched) {
        disconnect(m_watched, &ProjectController::revisionChanged, this, &AutopilotBridge::refreshOutdated);
    }
    m_watched = m_controller;
    connect(m_controller, &ProjectController::revisionChanged, this, &AutopilotBridge::refreshOutdated);
}

void AutopilotBridge::refreshOutdated() {
    // A staged B-roll / pacing preview made at an older revision than the session's (the timeline was edited while the
    // job ran, or since), or one Apply refused for that reason. Not while a job runs: it ends with a new preview.
    // Fix round 3 (ARGON-17): the publishing kit too (its chapters and thumbnails are the timeline's), when it is the
    // last job.
    const std::uint64_t now = m_controller ? m_controller->session().revision() : 0;
    const bool stale = m_rerunKind == QLatin1String("kit") ? m_controller && !m_kit.isEmpty() && now != m_kitRevision
                                                            : staged() && m_controller && now != m_stagedRevision;
    const bool shown = !busy() && !m_rerunKind.isEmpty() && (m_outdated || stale);
    if (shown != m_outdatedShown) {
        m_outdatedShown = shown;
        emit outdatedChanged();
    }
}

QString AutopilotBridge::refuseIfOutdated() {
    if (staged() && m_controller && m_controller->session().revision() != m_stagedRevision) {
        // The staged edit was made for the timeline before the change: dropped (never committed over the user's edit);
        // the suggestions stay listed and Run again makes them afresh.
        m_outdated = true;
        if (!m_stagedKind.isEmpty()) {
            m_rerunKind = m_stagedKind; // Run again makes this one again (a kit may have been the last job meanwhile)
        }
        m_stagedKind.clear();
        CreatorBase::discard();
        refreshOutdated();
        return tr("The timeline changed since this preview: Run again makes it again for the timeline as it is now");
    }
    return {};
}

QString AutopilotBridge::rerun() {
    if (busy()) {
        return tr("Busy");
    }
    if (m_rerunKind == QLatin1String("broll")) {
        return stageBroll(m_rerunParams, tr("Finding B-roll for what is said"), true);
    }
    if (m_rerunKind == QLatin1String("pacing")) {
        return stagePacing(m_rerunParams, tr("Measuring the reference's rhythm"), true);
    }
    if (m_rerunKind == QLatin1String("kit")) {
        return makeKit(m_kitRequest); // a kit written with your AI asks for consent again when it needs it
    }
    return tr("Nothing to run again");
}

bool AutopilotBridge::cloudAvailable() const {
    if (!m_ai) {
        return false;
    }
    // QA-07: offline mode and the no-cloud lock stop cloud models only (resolve() leaves them out); the local assistant,
    // Ollama or a custom provider on localhost keeps working.
    auto resolved = m_ai->platform().resolve(ai::Capability::Chat);
    if (!resolved || resolved->empty()) {
        return false;
    }
    const bool blocked = m_ai->offlineMode() || m_ai->projectLock() || (m_controller && m_controller->snapshot()->aiNoCloud);
    return !blocked || m_ai->runsLocally(resolved->front().model->id);
}

QUrl AutopilotBridge::kitFolder() const {
    QString folder = QSettings().value(QStringLiteral("autopilot/kitFolder")).toString();
    if (folder.isEmpty() || !QDir(folder).exists()) {
        folder = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    }
    return QUrl::fromLocalFile(folder);
}

std::shared_ptr<command::CommandServices> AutopilotBridge::autopilotServices(std::shared_ptr<ai::IChatModel> chat) {
    if (m_services) {
        return m_services;
    }
    const std::filesystem::path file = projectPath(m_engine);
    std::vector<std::shared_ptr<command::CommandServices>> parts{creatorServices(m_engine, m_transcripts, file)};
    // The reference's cuts: shot detection, cached per source file like Scene Edit Detection's.
    ai::analysis::MediaAnalyzerOptions analysis;
    analysis.cacheRoot = ai::AnalysisCache::defaultRoot();
    parts.push_back(std::make_shared<ai::analysis::AnalysisServices>(std::make_shared<ai::analysis::MediaAnalysisProvider>(analysis, file)));
    // Thumbnail scores: the media's frames, decoded small.
    auto access = std::make_shared<ai::search::MediaSearchAccess>(ai::search::MediaSearchAccessOptions{file, {}});
    ai::autopilot::FramePicture frames = [access](const MediaItem& media, Ticks time) -> std::optional<ai::search::Image> {
        const Ticks times[] = {time};
        auto pictures = access->frames(media, times, 320, {});
        if (!pictures || pictures->empty() || !pictures->front().valid()) {
            return std::nullopt;
        }
        return std::move(pictures->front());
    };
    ai::autopilot::AutopilotServices::ChatOpener opener;
    if (chat) {
        opener = [chat]() -> std::expected<std::unique_ptr<ai::IChatModel>, ai::AiError> { return std::make_unique<Borrowed>(chat); };
    }
    parts.push_back(std::make_shared<ai::autopilot::AutopilotServices>(std::move(frames), std::move(opener)));
    // The thumbnail files: rendered as Export Frame renders them.
    parts.push_back(exporter::stillExportServices());
    return std::make_shared<command::ServiceBundle>(std::move(parts));
}

void AutopilotBridge::discard() {
    CreatorBase::discard();
    m_rerunKind.clear();
    m_outdated = false;
    m_broll.clear();
    m_kit.clear();
    m_pacing.clear();
    m_stagedKind.clear();
    m_pacingReference = Json::object();
    emit resultChanged();
}

// --- B-roll ------------------------------------------------------------------------------------------------------------

QString AutopilotBridge::previewBroll(const QVariantMap& options) {
    if (!m_controller) {
        return tr("No project");
    }
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq) {
        return tr("Open a sequence first");
    }
    Json params{{"sequence", seq->id.value}};
    for (const char* key : {"perMinute", "minSeconds", "maxSeconds"}) {
        if (options.contains(QLatin1String(key))) {
            params[key] = options.value(QLatin1String(key)).toDouble();
        }
    }
    if (options.contains(QStringLiteral("visual"))) {
        params["visual"] = options.value(QStringLiteral("visual")).toBool();
    }
    m_rerunKind = QStringLiteral("broll");
    m_rerunParams = params;
    return stageBroll(params, tr("Finding B-roll for what is said"), true);
}

Json AutopilotBridge::chosenPlacements() const {
    Json out = Json::array();
    for (const QVariant& v : m_broll.value(QStringLiteral("suggestions")).toList()) {
        const QVariantMap s = v.toMap();
        if (!s.value(QStringLiteral("chosen")).toBool()) {
            continue;
        }
        Json p{{"media", s.value(QStringLiteral("media")).toULongLong()},
               {"atTicks", s.value(QStringLiteral("atTicks")).toLongLong()},
               {"sourceInTicks", s.value(QStringLiteral("sourceInTicks")).toLongLong()},
               {"durationTicks", s.value(QStringLiteral("durationTicks")).toLongLong()}};
        if (const int line = s.value(QStringLiteral("line")).toInt(); line > 0) {
            p["line"] = line;
        }
        out.push_back(std::move(p));
    }
    return out;
}

QString AutopilotBridge::stageBroll(const Json& params, const QString& phase, bool fresh) {
    if (!m_controller) {
        return tr("No project");
    }
    const ProjectSnapshot base = m_controller->snapshot();
    CreatorBase::discard();
    if (fresh) {
        m_broll.clear();
    }
    m_stagedKind.clear();
    m_outdated = false;
    watchRevision();
    m_sequence = SequenceId{params.value("sequence", std::uint64_t{0})};
    const std::uint64_t revision = m_controller->session().revision();
    QPointer<AutopilotBridge> self(this);
    const bool started = runJob(phase, base, "autopilot.broll", params, true,
                                [self, base, revision, fresh](Run&& run) {
                                    if (run.error) {
                                        self->m_message = run.error->code == command::ErrorCode::Cancelled ? tr("Cancelled") : qs(run.error->message);
                                        emit self->resultChanged();
                                        return;
                                    }
                                    self->setBrollReport(run.data, *base, fresh);
                                    if (run.preview && run.data.value("placed", 0) > 0) {
                                        self->m_staged = run.preview;
                                        self->m_stagedBase = base;
                                        self->m_stagedRevision = revision;
                                        self->m_stagedKind = QStringLiteral("broll");
                                        self->showGhost(self->m_sequence);
                                    }
                                    self->m_message = joined(run.warnings);
                                    emit self->resultChanged();
                                },
                                autopilotServices());
    return started ? QString() : tr("Busy");
}

void AutopilotBridge::setBrollReport(const Json& data, const ProjectState& state, bool fresh) {
    if (!fresh && !m_broll.isEmpty()) {
        // The suggestions stay as the first preview listed them (with their chosen flags); the track may change.
        m_broll.insert(QStringLiteral("placed"), QVariant::fromValue(data.value("placed", std::uint64_t{0})));
        m_broll.insert(QStringLiteral("track"), data.contains("track") ? variantOf(data["track"]) : QVariant(QVariantMap{}));
        return;
    }
    QVariantMap report = variantOf(data).toMap();
    QVariantList suggestions;
    for (const Json& s : data.value("suggestions", Json::array())) {
        QVariantMap m = variantOf(s).toMap();
        m.insert(QStringLiteral("chosen"), true);
        const std::uint64_t media = s.value("media", std::uint64_t{0});
        const MediaItem* item = state.findMedia(MediaId{media});
        if (item && item->info.hasVideo()) {
            m.insert(QStringLiteral("thumb"),
                     frameUrl(media, s.value("sourceInTicks", std::int64_t{0}) + s.value("durationTicks", std::int64_t{0}) / 2));
        }
        suggestions.append(m);
    }
    report.insert(QStringLiteral("suggestions"), suggestions);
    m_broll = report;
}

QString AutopilotBridge::setBrollChosen(int index, bool chosen) {
    QVariantList list = m_broll.value(QStringLiteral("suggestions")).toList();
    if (index < 1 || index > list.size()) {
        return tr("No such suggestion");
    }
    if (m_broll.value(QStringLiteral("done")).toBool()) {
        return tr("This B-roll is already in the sequence");
    }
    QVariantMap s = list[index - 1].toMap();
    s.insert(QStringLiteral("chosen"), chosen);
    list[index - 1] = s;
    m_broll.insert(QStringLiteral("suggestions"), list);
    const Json placements = chosenPlacements();
    if (placements.empty()) {
        CreatorBase::discard();
        m_stagedKind.clear();
        m_broll.insert(QStringLiteral("placed"), 0);
        emit resultChanged();
        return {};
    }
    return stageBroll(Json{{"sequence", m_sequence.value}, {"placements", placements}}, tr("Updating the B-roll"), false);
}

QString AutopilotBridge::applyBroll() {
    if (m_stagedKind != QLatin1String("broll") || !staged()) {
        return tr("Preview the B-roll first");
    }
    if (QString error = refuseIfOutdated(); !error.isEmpty()) {
        return error;
    }
    if (QString error = commitStaged(tr("Auto B-roll")); !error.isEmpty()) {
        return error;
    }
    m_stagedKind.clear();
    m_broll.insert(QStringLiteral("done"), true);
    const int placed = m_broll.value(QStringLiteral("placed")).toInt();
    m_message = placed == 1 ? tr("1 B-roll clip added: Undo removes it in one step")
                            : tr("%1 B-roll clips added: Undo removes them in one step").arg(placed);
    emit resultChanged();
    return {};
}

QString AutopilotBridge::generateFor(int line) {
    if (!m_controller || !m_controller->currentSequence()) {
        return tr("Open a sequence first");
    }
    for (const QVariant& v : m_broll.value(QStringLiteral("unmatched")).toList()) {
        const QVariantMap u = v.toMap();
        if (u.value(QStringLiteral("line")).toInt() != line) {
            continue;
        }
        const SequencePtr seq = m_controller->currentSequence();
        QVariantMap proposal{{QStringLiteral("operation"), QStringLiteral("broll")},
                             {QStringLiteral("prompt"), u.value(QStringLiteral("prompt"))},
                             {QStringLiteral("seconds"), std::clamp(static_cast<int>(std::lround(u.value(QStringLiteral("duration")).toDouble())), 2, 10)},
                             {QStringLiteral("aspect"), seq->height > seq->width ? QStringLiteral("9:16") : QStringLiteral("16:9")},
                             {QStringLiteral("at"), u.value(QStringLiteral("at")).toDouble()},
                             {QStringLiteral("clip"), QString()},
                             {QStringLiteral("head"), false},
                             {QStringLiteral("loop"), false},
                             {QStringLiteral("sequence"), QString::number(seq->id.value)}};
        emit generateRequested(proposal);
        return {};
    }
    return tr("No such line");
}

void AutopilotBridge::proposeGeneration(const QVariantMap& proposal) {
    auto* generate = m_engine ? m_engine->singletonInstance<GenerateController*>("Wabcut", "GenerateController") : nullptr;
    if (generate && m_controller) {
        generate->propose(proposal, m_controller->snapshot());
    }
}

// --- Publishing kit ----------------------------------------------------------------------------------------------------

QVariantMap AutopilotBridge::kitQuote() const {
    QVariantMap q{{QStringLiteral("available"), cloudAvailable()}};
    if (!cloudAvailable() || !m_controller || !m_controller->currentSequence()) {
        return q;
    }
    auto resolved = m_ai->platform().resolve(ai::Capability::Chat);
    if (!resolved || resolved->empty()) {
        return q;
    }
    const std::string modelId = resolved->front().model->id;
    const ProjectSnapshot state = m_controller->snapshot();
    const SequencePtr seq = m_controller->currentSequence();
    std::shared_ptr<ai::edit::TranscriptServices> transcripts = m_transcripts ? m_transcripts->services() : nullptr;
    ai::edit::SequenceTranscript words;
    if (transcripts && transcripts->transcripts) {
        words = ai::edit::SequenceTranscript::build(*state, seq->id, *transcripts->transcripts);
    }
    std::vector<ai::autopilot::KitChapter> chapters;
    for (const QVariant& v : m_kit.value(QStringLiteral("chapters")).toList()) {
        const QVariantMap c = v.toMap();
        chapters.push_back(ai::autopilot::KitChapter{Ticks{static_cast<std::int64_t>(std::llround(c.value(QStringLiteral("seconds")).toDouble() * static_cast<double>(kTicksPerSecond)))},
                                                     c.value(QStringLiteral("title")).toString().toStdString()});
    }
    const std::string text = ai::autopilot::kitTranscriptText(words);
    q.insert(QStringLiteral("model"), qs(modelId));
    q.insert(QStringLiteral("characters"), static_cast<int>(text.size()));
    if (text.empty()) {
        q.insert(QStringLiteral("available"), false);
        q.insert(QStringLiteral("reason"), tr("Transcribe the sequence first"));
        return q;
    }
    const ai::ChatRequest request = ai::autopilot::kitChatRequest(text, chapters, seq->name);
    if (auto p = m_ai->platform().preflightChat(modelId, request)) {
        q.insert(QStringLiteral("provider"), qs(p->candidate.provider ? p->candidate.provider->name : std::string()));
        q.insert(QStringLiteral("cost"), p->cost.priced ? tr("about %1, at most %2").arg(qs(ai::formatUsd(p->cost.expected)), qs(ai::formatUsd(p->cost.ceiling)))
                                                        : tr("this model's price is not known"));
        q.insert(QStringLiteral("priced"), p->cost.priced);
    } else {
        q.insert(QStringLiteral("available"), false);
        q.insert(QStringLiteral("reason"), qs(p.error().message));
        return q;
    }
    const int characters = static_cast<int>(text.size());
    const QVariantMap prompt = m_ai->consentPrompt(
        qs(modelId), QStringLiteral("chat"),
        characters == 1
            ? tr("the sequence's transcript (1 character) and its chapter titles, as text; never the video")
            : tr("the sequence's transcript (%1 characters) and its chapter titles, as text; never the video")
                  .arg(characters));
    q.insert(QStringLiteral("needed"), prompt.value(QStringLiteral("needed")).toBool());
    q.insert(QStringLiteral("prompt"), prompt);
    return q;
}

QString AutopilotBridge::makeKit(const QVariantMap& options) {
    if (!m_controller) {
        return tr("No project");
    }
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq) {
        return tr("Open a sequence first");
    }
    Json params{{"sequence", seq->id.value},
                {"thumbnails", std::clamp(options.value(QStringLiteral("thumbnails"), 4).toInt(), 0, 12)},
                {"chapters", options.value(QStringLiteral("chapters"), QStringLiteral("auto")).toString().toStdString()}};
    m_kitOptions = params;
    std::shared_ptr<ai::IChatModel> chat;
    if (options.value(QStringLiteral("cloud")).toBool()) {
        if (!m_services) {
            if (!cloudAvailable()) {
                return tr("No cloud chat model is set up, AI is offline, or this project is no-cloud");
            }
            // Never a quiet fallback: the sheet shows the cost and asks first; without an answer nothing is sent.
            const bool allowOnce = options.value(QStringLiteral("allowOnce")).toBool();
            if (!allowOnce && kitQuote().value(QStringLiteral("needed")).toBool()) {
                return tr("Writing with your AI needs your consent first: nothing was sent");
            }
            auto resolved = m_ai->platform().resolve(ai::Capability::Chat);
            if (!resolved || resolved->empty()) {
                return tr("No cloud chat model is set up");
            }
            auto model = m_ai->platform().chat(resolved->front().model->id, ai::CallOptions{allowOnce, {}});
            if (!model) {
                return qs(model.error().message);
            }
            chat = std::shared_ptr<ai::IChatModel>(std::move(*model));
        }
        params["cloud"] = true;
    }
    if (busy()) {
        return tr("Busy");
    }
    // Fix round 3 (ARGON-17): Run again makes the kit again with these options (never with a consent given once).
    m_kitRequest = options;
    m_kitRequest.remove(QStringLiteral("allowOnce"));
    m_rerunKind = QStringLiteral("kit");
    m_outdated = false;
    watchRevision();
    const std::uint64_t revision = m_controller->session().revision();
    const ProjectSnapshot base = m_controller->snapshot();
    QPointer<AutopilotBridge> self(this);
    const bool started = runJob(tr("Preparing the publishing kit"), base, "autopilot.publishKit", params, true,
                                [self, base, revision](Run&& run) {
                                    if (run.error) {
                                        self->m_message = run.error->code == command::ErrorCode::Cancelled ? tr("Cancelled") : qs(run.error->message);
                                        emit self->resultChanged();
                                        return;
                                    }
                                    QVariantMap kit = variantOf(run.data).toMap();
                                    QVariantList thumbs;
                                    for (const Json& t : run.data.value("thumbnails", Json::array())) {
                                        QVariantMap m = variantOf(t).toMap();
                                        if (t.contains("media")) {
                                            m.insert(QStringLiteral("thumb"), frameUrl(t["media"].get<std::uint64_t>(), t.value("sourceTicks", std::int64_t{0})));
                                        }
                                        thumbs.append(m);
                                    }
                                    kit.insert(QStringLiteral("thumbnails"), thumbs);
                                    self->m_kit = kit;
                                    self->m_kitRevision = revision;
                                    self->m_message = joined(run.warnings);
                                    emit self->resultChanged();
                                },
                                autopilotServices(chat));
    return started ? QString() : tr("Busy");
}

QString AutopilotBridge::writeKit(const QUrl& folder, const QVariantMap& edits) {
    if (!m_controller || m_kit.isEmpty()) {
        return tr("Prepare the kit first");
    }
    const QString path = folder.isLocalFile() ? folder.toLocalFile() : folder.toString();
    if (path.isEmpty() || !QDir(path).exists()) {
        return tr("Choose a folder to write the kit into");
    }
    if (m_controller->session().revision() != m_kitRevision) {
        // Fix round 3 (ARGON-17): the description's chapters and the thumbnails were made for the timeline before the
        // change; nothing is written, and Run again makes the kit for the timeline as it is now.
        m_outdated = true;
        m_rerunKind = QStringLiteral("kit");
        refreshOutdated();
        return tr("The timeline changed since this preview: Run again makes it again for the timeline as it is now");
    }
    QSettings().setValue(QStringLiteral("autopilot/kitFolder"), path);
    Json params = m_kitOptions;
    params["folder"] = QDir::toNativeSeparators(path).toStdString();
    const QString title = edits.value(QStringLiteral("title"), m_kit.value(QStringLiteral("title"))).toString().trimmed();
    if (!title.isEmpty()) {
        params["title"] = title.left(200).toStdString();
    }
    params["description"] = edits.value(QStringLiteral("description"), m_kit.value(QStringLiteral("description"))).toString().left(5000).toStdString();
    Json tags = Json::array();
    const QVariant tagValue = edits.value(QStringLiteral("tags"), m_kit.value(QStringLiteral("tags")));
    const QStringList tagList = tagValue.typeId() == QMetaType::QString ? tagValue.toString().split(QLatin1Char(','), Qt::SkipEmptyParts)
                                                                         : tagValue.toStringList();
    for (const QString& t : tagList) {
        if (const QString tag = t.trimmed(); !tag.isEmpty()) {
            tags.push_back(tag.left(100).toStdString());
        }
    }
    params["tags"] = std::move(tags);
    const ProjectSnapshot base = m_controller->snapshot();
    QPointer<AutopilotBridge> self(this);
    const bool started = runJob(tr("Writing the publishing kit"), base, "autopilot.publishKit", params, false,
                                [self](Run&& run) {
                                    if (run.error) {
                                        self->m_message = run.error->code == command::ErrorCode::Cancelled ? tr("Cancelled") : qs(run.error->message);
                                        emit self->resultChanged();
                                        return;
                                    }
                                    const QString written = qs(run.data.value("folder", std::string()));
                                    const int files = static_cast<int>(run.data.value("files", Json::array()).size());
                                    self->m_kitWritten = written;
                                    self->m_kit.insert(QStringLiteral("written"), true);
                                    self->m_kit.insert(QStringLiteral("folder"), written);
                                    self->m_kit.insert(QStringLiteral("files"), variantOf(run.data.value("files", Json::array())));
                                    QVariantList thumbs = self->m_kit.value(QStringLiteral("thumbnails")).toList();
                                    const Json& done = run.data.value("thumbnails", Json::array());
                                    for (qsizetype i = 0; i < thumbs.size() && static_cast<std::size_t>(i) < done.size(); ++i) {
                                        if (done[static_cast<std::size_t>(i)].contains("file")) {
                                            QVariantMap t = thumbs[i].toMap();
                                            t.insert(QStringLiteral("file"), qs(done[static_cast<std::size_t>(i)]["file"].get<std::string>()));
                                            t.insert(QStringLiteral("thumb"),
                                                     QUrl::fromLocalFile(written + QLatin1Char('/') + t.value(QStringLiteral("file")).toString()).toString());
                                            thumbs[i] = t;
                                        }
                                    }
                                    self->m_kit.insert(QStringLiteral("thumbnails"), thumbs);
                                    QString message = files == 1
                                                          ? tr("Wrote 1 file to %1").arg(QDir::toNativeSeparators(written))
                                                          : tr("Wrote %1 files to %2").arg(files).arg(QDir::toNativeSeparators(written));
                                    if (!run.warnings.empty()) {
                                        message += QLatin1Char('\n') + joined(run.warnings);
                                    }
                                    self->m_message = message;
                                    emit self->resultChanged();
                                },
                                autopilotServices());
    return started ? QString() : tr("Busy");
}

void AutopilotBridge::revealKit() const {
    if (!m_kitWritten.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_kitWritten));
    }
}

// --- Pacing ------------------------------------------------------------------------------------------------------------

QVariantList AutopilotBridge::referenceVideos() const {
    QVariantList out;
    if (!m_controller) {
        return out;
    }
    const ProjectSnapshot state = m_controller->snapshot();
    std::vector<const MediaItem*> videos;
    for (const MediaPtr& m : state->media) {
        if (m && m->info.hasVideo() && !m->isGenerator() && !m->info.isStillImage && m->info.duration >= Ticks::fromSeconds(5)) {
            videos.push_back(m.get());
        }
    }
    std::ranges::sort(videos, [](const MediaItem* a, const MediaItem* b) { return QString::fromStdString(a->name).localeAwareCompare(QString::fromStdString(b->name)) < 0; });
    for (const MediaItem* m : videos) {
        const int total = static_cast<int>(m->info.duration.toSeconds());
        out.append(QVariantMap{{QStringLiteral("key"), mediaKey(m->id)},
                               {QStringLiteral("name"), qs(m->name)},
                               {QStringLiteral("duration"), m->info.duration.toSeconds()},
                               {QStringLiteral("label"), QStringLiteral("%1 (%2:%3)").arg(qs(m->name)).arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'))}});
    }
    return out;
}

Json AutopilotBridge::pacingParams(const QString& apply) const {
    Json params = m_pacingReference;
    params["sequence"] = m_sequence.value;
    params["apply"] = apply.toStdString();
    for (auto it = m_pacingOptions.begin(); it != m_pacingOptions.end(); ++it) {
        params[it.key()] = *it;
    }
    Json only = Json::array();
    for (const QVariant& v : m_pacing.value(QStringLiteral("suggestions")).toList()) {
        const QVariantMap s = v.toMap();
        if (s.value(QStringLiteral("chosen")).toBool()) {
            only.push_back(s.value(QStringLiteral("index")).toInt());
        }
    }
    params["only"] = std::move(only);
    return params;
}

QString AutopilotBridge::analysePacing(const QString& reference, const QVariantMap& options) {
    if (!m_controller) {
        return tr("No project");
    }
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq) {
        return tr("Open a sequence first");
    }
    const auto media = detail::parseId<MediaId>(QLatin1Char('m'), reference);
    if (!media || !m_controller->snapshot()->findMedia(*media)) {
        return tr("Choose a reference video");
    }
    m_pacingOptions = Json::object();
    if (options.contains(QStringLiteral("strength"))) {
        m_pacingOptions["strength"] = std::clamp(options.value(QStringLiteral("strength")).toDouble(), 0.0, 1.0);
    }
    if (options.contains(QStringLiteral("tolerance"))) {
        m_pacingOptions["tolerance"] = std::clamp(options.value(QStringLiteral("tolerance")).toDouble(), 1.0, 5.0);
    }
    m_sequence = seq->id;
    Json params{{"sequence", seq->id.value}, {"reference", media->value}, {"apply", "markers"}};
    for (auto it = m_pacingOptions.begin(); it != m_pacingOptions.end(); ++it) {
        params[it.key()] = *it;
    }
    m_rerunKind = QStringLiteral("pacing");
    m_rerunParams = params;
    return stagePacing(params, tr("Measuring the reference's rhythm"), true);
}

QString AutopilotBridge::stagePacing(const Json& params, const QString& phase, bool fresh, const QString& commitAs) {
    if (!m_controller) {
        return tr("No project");
    }
    const ProjectSnapshot base = m_controller->snapshot();
    CreatorBase::discard();
    m_stagedKind.clear();
    m_outdated = false;
    watchRevision();
    if (fresh) {
        m_pacing.clear();
        m_pacingReference = Json::object();
    }
    const std::uint64_t revision = m_controller->session().revision();
    QPointer<AutopilotBridge> self(this);
    const bool started = runJob(
        phase, base, "autopilot.pacing", params, true,
        [self, base, revision, fresh, commitAs](Run&& run) {
            if (run.error) {
                self->m_message = run.error->code == command::ErrorCode::Cancelled ? tr("Cancelled") : qs(run.error->message);
                emit self->resultChanged();
                return;
            }
            QVariantMap report = variantOf(run.data).toMap();
            if (fresh) {
                const Json& ref = run.data.value("reference", Json::object());
                self->m_pacingReference = Json{{"referenceCuts", ref.value("cutTimes", Json::array())}, {"referenceLength", ref.value("length", 0.0)}};
            } else {
                // Later runs pass the cuts: keep the reference's own name.
                report.insert(QStringLiteral("reference"), self->m_pacing.value(QStringLiteral("reference")));
            }
            self->m_pacing = report;
            self->m_message = joined(run.warnings);
            if (run.preview && run.data.value("applied", 0) > 0) {
                self->m_staged = run.preview;
                self->m_stagedBase = base;
                self->m_stagedRevision = revision;
                self->m_stagedKind = QStringLiteral("pacing");
                if (!commitAs.isEmpty()) {
                    QString error = self->refuseIfOutdated();
                    if (error.isEmpty()) {
                        error = self->commitStaged(commitAs);
                    }
                    self->m_stagedKind.clear();
                    const int cuts = static_cast<int>(run.data.value("applied", 0));
                    self->m_message = error.isEmpty()
                                          ? (cuts == 1 ? tr("1 cut added: Undo removes it in one step")
                                                       : tr("%1 cuts added: Undo removes them in one step").arg(cuts))
                                          : error;
                    self->m_pacing.insert(QStringLiteral("done"), error.isEmpty());
                } else {
                    self->showGhost(self->m_sequence);
                }
            }
            emit self->resultChanged();
        },
        autopilotServices());
    return started ? QString() : tr("Busy");
}

QString AutopilotBridge::setPacingChosen(int index, bool chosen) {
    QVariantList list = m_pacing.value(QStringLiteral("suggestions")).toList();
    if (index < 1 || index > list.size()) {
        return tr("No such suggestion");
    }
    if (m_pacing.value(QStringLiteral("done")).toBool()) {
        return tr("These cuts are already in the sequence");
    }
    QVariantMap s = list[index - 1].toMap();
    s.insert(QStringLiteral("chosen"), chosen);
    list[index - 1] = s;
    m_pacing.insert(QStringLiteral("suggestions"), list);
    if (pacingParams(QStringLiteral("markers"))["only"].empty()) {
        CreatorBase::discard();
        m_stagedKind.clear();
        emit resultChanged();
        return {};
    }
    return stagePacing(pacingParams(QStringLiteral("markers")), tr("Updating the suggestions"), false);
}

QString AutopilotBridge::applyPacing(const QString& mode) {
    if (m_pacing.isEmpty() || m_pacingReference.empty()) {
        return tr("Analyse the pacing first");
    }
    if (pacingParams(mode)["only"].empty()) {
        return tr("Choose at least one suggestion");
    }
    if (mode == QLatin1String("markers")) {
        if (m_stagedKind != QLatin1String("pacing") || !staged()) {
            return tr("Analyse the pacing first");
        }
        if (QString error = refuseIfOutdated(); !error.isEmpty()) {
            return error;
        }
        if (QString error = commitStaged(tr("Pacing Match")); !error.isEmpty()) {
            return error;
        }
        m_stagedKind.clear();
        m_pacing.insert(QStringLiteral("done"), true);
        m_message = tr("Markers added where the edit could cut: Undo removes them in one step");
        emit resultChanged();
        return {};
    }
    if (mode != QLatin1String("cuts")) {
        return tr("Unknown way to apply");
    }
    return stagePacing(pacingParams(QStringLiteral("cuts")), tr("Cutting"), false, tr("Pacing Match"));
}

} // namespace wabcut::app
