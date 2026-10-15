#include "GlobalBridge.h"

#include "AiController.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "TranscriptBridge.h"

#include <wabcut/ai/HttpDownload.h>
#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/dub/MediaSpeechAudio.h>
#include <wabcut/ai/dub/Voices.h>
#include <wabcut/ai/translate/Languages.h>
#include <wabcut/ai/translate/LocalTranslation.h>
#include <wabcut/ai/translate/TranslateCommands.h>
#include <wabcut/command/ExecutionContext.h>

#include <QCoreApplication>
#include <QDesktopServices>
#include <QTextBoundaryFinder>
#include <QUrl>

#include <cmath>
#include <utility>

namespace wabcut::app {

using command::Json;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }
QString qs(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

std::filesystem::path projectPath(QQmlEngine* engine) {
    auto* file = engine ? engine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile") : nullptr;
    if (!file || file->filePath().isEmpty()) {
        return {};
    }
    return std::filesystem::path(file->filePath().toStdWString());
}

QString joined(const std::vector<std::string>& warnings) {
    QStringList lines;
    for (const std::string& w : warnings) {
        lines << qs(w);
    }
    return lines.join(QLatin1Char('\n'));
}

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

QVariantList captionTracksOf(ProjectController* controller) {
    QVariantList out;
    if (!controller) {
        return out;
    }
    const ProjectSnapshot s = controller->snapshot();
    const Sequence* seq = s ? s->findSequence(controller->currentSequenceId()) : nullptr;
    if (!seq) {
        return out;
    }
    for (const CaptionTrack& t : seq->captionTracks) {
        out.append(QVariantMap{{QStringLiteral("id"), QVariant::fromValue(static_cast<qint64>(t.id.value))},
                               {QStringLiteral("name"), qs(t.name)},
                               {QStringLiteral("language"), qs(t.language)},
                               {QStringLiteral("cues"), static_cast<int>(t.cues.size())}});
    }
    return out;
}

QString errorText(const std::optional<command::CommandError>& error, const QString& fallback) {
    if (!error) {
        return fallback;
    }
    if (error->code == command::ErrorCode::Cancelled) {
        return QObject::tr("Cancelled");
    }
    return qs(error->message);
}

/// A model made on the GUI thread, lent to a job (the platform hands models out on the GUI thread).
class BorrowedChat final : public ai::IChatModel {
public:
    explicit BorrowedChat(std::shared_ptr<ai::IChatModel> model) : m_model(std::move(model)) {}
    const ai::ModelSpec& spec() const override { return m_model->spec(); }
    std::expected<ai::ChatResponse, ai::AiError> chat(const ai::ChatRequest& r, const ai::ChatEventHandler& e, std::stop_token s) override {
        return m_model->chat(r, e, s);
    }

private:
    std::shared_ptr<ai::IChatModel> m_model;
};

class BorrowedSpeech final : public ai::ISpeechSynth {
public:
    explicit BorrowedSpeech(std::shared_ptr<ai::ISpeechSynth> synth) : m_synth(std::move(synth)) {}
    const ai::ModelSpec& spec() const override { return m_synth->spec(); }
    std::expected<ai::SpeechResult, ai::AiError> synthesize(const ai::SpeechRequest& r, std::stop_token s) override {
        return m_synth->synthesize(r, s);
    }
    std::expected<ai::ClonedVoice, ai::AiError> cloneVoice(std::span<const std::filesystem::path> samples, std::string_view name,
                                                           std::stop_token s) override {
        return m_synth->cloneVoice(samples, name, s);
    }
    std::expected<void, ai::AiError> deleteVoice(std::string_view voiceId, std::stop_token s) override {
        return m_synth->deleteVoice(voiceId, s);
    }

private:
    std::shared_ptr<ai::ISpeechSynth> m_synth;
};

/// Local translation: the pair's OPUS-MT model when it is installed (the dialog downloads it).
ai::translate::TranslateServices::LocalOpener localOpener() {
    return [](std::string_view source, std::string_view target) -> std::expected<std::unique_ptr<ai::ITranslator>, ai::AiError> {
        const ai::translate::LocalPair* pair = ai::translate::findLocalPair(source, target);
        if (pair == nullptr) {
            return std::unexpected(ai::AiError(ai::AiErrorCode::Unsupported, "no model on this computer translates \"" + std::string(source) +
                                                                                  "\" into \"" + std::string(target) + "\""));
        }
        const ai::ModelStore store(ai::ModelStore::defaultRoot());
        if (!ai::translate::isLocalModelInstalled(store, pair->model)) {
            return std::unexpected(ai::AiError(ai::AiErrorCode::ModelMissing, "the model " + std::string(pair->model) + " is not downloaded"));
        }
        ai::translate::LocalTranslatorOptions options;
        options.modelDirectory = ai::translate::localModelDirectory(store, pair->model);
        options.targetToken = std::string(pair->targetToken);
        return std::make_unique<ai::translate::LocalTranslator>(std::move(options));
    };
}

class QtLineBreaker final : public LineBreaker {
public:
    std::vector<std::size_t> breakOpportunities(std::string_view text, std::string_view) const override {
        const QString s = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
        // UTF-16 index -> UTF-8 byte offset.
        std::vector<std::size_t> bytes(static_cast<std::size_t>(s.size()) + 1, 0);
        std::size_t at = 0;
        for (qsizetype i = 0; i < s.size(); ++i) {
            bytes[static_cast<std::size_t>(i)] = at;
            const char16_t c = s.at(i).unicode();
            if (QChar::isHighSurrogate(c) && i + 1 < s.size()) {
                bytes[static_cast<std::size_t>(i + 1)] = at;
                at += 4;
                ++i;
            } else {
                at += c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
            }
        }
        bytes[static_cast<std::size_t>(s.size())] = at;
        std::vector<std::size_t> out;
        QTextBoundaryFinder finder(QTextBoundaryFinder::Line, s);
        for (qsizetype p = finder.toNextBoundary(); p > 0 && p < s.size(); p = finder.toNextBoundary()) {
            out.push_back(bytes[static_cast<std::size_t>(p)]);
        }
        return out;
    }
};

double cents(double dollars) { return std::round(dollars * 100.0) / 100.0; }

} // namespace

std::shared_ptr<const LineBreaker> qtLineBreaker() {
    static const auto breaker = std::make_shared<const QtLineBreaker>();
    return breaker;
}

// --- TranslateBridge ---------------------------------------------------------------------------------------------------

TranslateBridge* TranslateBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new TranslateBridge;
    bridge->setEngine(qmlEngine);
    if (qmlEngine) {
        bridge->m_ai = qmlEngine->singletonInstance<::AiController*>("Wabcut", "AiController");
        bridge->stopJobsWith(bridge->m_ai); // its cloud chat and model downloads (A5-P2-19)
        bridge->m_transcripts = qmlEngine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
    }
    return bridge;
}

TranslateBridge::TranslateBridge(QObject* parent) : CreatorBase(parent) {}

QVariantList TranslateBridge::captionTracks() const { return captionTracksOf(m_controller); }

bool TranslateBridge::cloudAvailable() const {
    if (!m_ai || m_ai->offlineMode() || (m_controller && m_controller->snapshot()->aiNoCloud)) {
        return false;
    }
    auto resolved = m_ai->platform().resolve(ai::Capability::Chat);
    return resolved && !resolved->empty();
}

QString TranslateBridge::cloudModelName() const {
    if (!cloudAvailable()) {
        return {};
    }
    auto resolved = m_ai->platform().resolve(ai::Capability::Chat);
    return resolved && !resolved->empty() ? qs(resolved->front().model->name) : QString();
}

QVariantList TranslateBridge::languages(const QString& source) const {
    QVariantList out;
    const ai::ModelStore store(ai::ModelStore::defaultRoot());
    const std::string from = ai::translate::normalizeLanguage(source.toStdString());
    for (const ai::translate::LanguageRules& rules : ai::translate::knownLanguages()) {
        if (rules.code == from) {
            continue;
        }
        QVariantMap m{{QStringLiteral("code"), qs(rules.code)},
                      {QStringLiteral("name"), qs(rules.name)},
                      {QStringLiteral("nativeName"), qs(rules.nativeName)},
                      {QStringLiteral("readingSpeed"), rules.charsPerSecond}};
        const ai::translate::LocalPair* pair = ai::translate::findLocalPair(from, rules.code);
        m[QStringLiteral("local")] = pair != nullptr;
        if (pair) {
            m[QStringLiteral("localModel")] = qs(pair->model);
            m[QStringLiteral("installed")] = ai::translate::isLocalModelInstalled(store, pair->model);
            m[QStringLiteral("downloadMB")] = static_cast<int>((ai::translate::localModelDownloadBytes(store, pair->model) + 500'000) / 1'000'000);
        }
        out.append(m);
    }
    return out;
}

Json TranslateBridge::paramsOf(const QVariantMap& options) const {
    Json params = Json::object();
    if (options.contains(QStringLiteral("captionTrack"))) {
        params["captionTrack"] = options.value(QStringLiteral("captionTrack")).toLongLong();
    }
    Json languages = Json::array();
    for (const QVariant& l : options.value(QStringLiteral("languages")).toList()) {
        languages.push_back(l.toString().toStdString());
    }
    params["languages"] = std::move(languages);
    params["provider"] = options.value(QStringLiteral("provider"), QStringLiteral("auto")).toString().toStdString();
    if (const QString tone = options.value(QStringLiteral("tone")).toString(); !tone.isEmpty()) {
        params["tone"] = tone.toStdString();
    }
    if (const QString notes = options.value(QStringLiteral("notes")).toString().trimmed(); !notes.isEmpty()) {
        params["notes"] = notes.left(500).toStdString();
    }
    if (options.contains(QStringLiteral("readingSpeed")) && options.value(QStringLiteral("readingSpeed")).toDouble() > 0) {
        params["readingSpeed"] = options.value(QStringLiteral("readingSpeed")).toDouble();
    }
    // The glossary: "term = translation" lines (the dialog's text box) or a list of {source, target}.
    Json glossary = Json::array();
    const QVariant g = options.value(QStringLiteral("glossary"));
    if (g.typeId() == QMetaType::QString) {
        for (const QString& line : g.toString().split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const qsizetype eq = line.indexOf(QLatin1Char('='));
            const QString source = (eq < 0 ? line : line.left(eq)).trimmed();
            const QString target = (eq < 0 ? line : line.mid(eq + 1)).trimmed();
            if (!source.isEmpty() && !target.isEmpty()) {
                glossary.push_back(Json{{"source", source.left(200).toStdString()}, {"target", target.left(200).toStdString()}});
            }
        }
    } else {
        for (const QVariant& term : g.toList()) {
            const QVariantMap t = term.toMap();
            glossary.push_back(Json{{"source", t.value(QStringLiteral("source")).toString().toStdString()},
                                    {"target", t.value(QStringLiteral("target")).toString().toStdString()}});
        }
    }
    if (!glossary.empty()) {
        params["glossary"] = std::move(glossary);
    }
    return params;
}

std::shared_ptr<command::CommandServices> TranslateBridge::jobServices(bool allowOnce) {
    if (m_services) {
        return m_services; // tests
    }
    std::shared_ptr<ai::IChatModel> chat;
    if (cloudAvailable()) {
        auto resolved = m_ai->platform().resolve(ai::Capability::Chat);
        if (resolved && !resolved->empty()) {
            if (auto model = m_ai->platform().chat(resolved->front().model->id, ai::CallOptions{allowOnce, {}})) {
                chat = std::shared_ptr<ai::IChatModel>(std::move(*model));
            }
        }
    }
    ai::translate::TranslateServices::ChatOpener opener;
    if (chat) {
        opener = [chat]() -> std::expected<std::unique_ptr<ai::IChatModel>, ai::AiError> { return std::make_unique<BorrowedChat>(chat); };
    }
    return std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
        creatorServices(m_engine, m_transcripts, projectPath(m_engine)), std::make_shared<ai::translate::TranslateServices>(localOpener(), opener),
        std::make_shared<command::CaptionServices>(qtLineBreaker())});
}

QString TranslateBridge::preview(const QVariantMap& options) {
    if (!m_controller) {
        return tr("No project");
    }
    if (options.value(QStringLiteral("languages")).toList().isEmpty()) {
        return tr("Choose at least one language");
    }
    const ProjectSnapshot base = m_controller->snapshot();
    Json params = paramsOf(options);
    discard();
    m_report.clear();
    const std::uint64_t revision = m_controller->session().revision();
    QPointer<TranslateBridge> self(this);
    const bool started = runJob(
        tr("Translating captions"), base, "captions.translate", std::move(params), true,
        [self, base, revision](Run&& run) {
            if (!run.error && run.preview) {
                self->m_staged = run.preview;
                self->m_stagedBase = base;
                self->m_stagedRevision = revision;
                self->m_report = variantOf(run.data).toMap();
                self->m_message = joined(run.warnings);
            } else {
                self->m_message = errorText(run.error, tr("Nothing was translated"));
            }
            emit self->resultChanged();
        },
        jobServices(options.value(QStringLiteral("allowOnce")).toBool()));
    return started ? QString() : tr("Busy");
}

QString TranslateBridge::estimate(const QVariantMap& options) {
    if (!m_controller) {
        return tr("No project");
    }
    Json params = paramsOf(options);
    params["estimate"] = true;
    params["provider"] = "cloud";
    QPointer<TranslateBridge> self(this);
    const bool started = runJob(
        tr("Pricing the translation"), m_controller->snapshot(), "captions.translate", std::move(params), false,
        [self](Run&& run) {
            if (!run.error) {
                self->m_report = variantOf(run.data).toMap();
                self->m_message.clear();
            } else {
                self->m_message = errorText(run.error, {});
            }
            emit self->resultChanged();
        },
        jobServices(false));
    return started ? QString() : tr("Busy");
}

QString TranslateBridge::download(const QString& source, const QString& target) {
    const ai::translate::LocalPair* pair = ai::translate::findLocalPair(source.toStdString(), target.toStdString());
    if (!pair) {
        return tr("No model on this computer translates this pair");
    }
    if (!m_ai) {
        return tr("Downloads are unavailable");
    }
    if (m_ai->offlineMode()) {
        return tr("Offline mode is on: downloads are paused");
    }
    ai::IHttpTransport* transport = &m_ai->transport();
    const std::string model(pair->model);
    setJob(tr("Downloading the %1 model").arg(qs(model)), 0.0);
    QPointer<TranslateBridge> self(this);
    const bool started = m_job.start(this, m_synchronous, [self, transport, model](const std::atomic<bool>& cancelled) -> BridgeJob::Finish {
        ai::ModelStore store(ai::ModelStore::defaultRoot());
        ai::HttpDownloadTransport downloads(*transport);
        std::stop_source stop;
        const std::uint64_t total = std::max<std::uint64_t>(1, ai::translate::localModelDownloadBytes(store, model));
        std::uint64_t before = 0;
        auto ensured = ai::translate::ensureLocalModel(
            store, model, &downloads,
            [&](const ai::DownloadProgress& p) {
                if (cancelled.load()) {
                    stop.request_stop();
                }
                const double fraction = std::min(1.0, static_cast<double>(before + p.received) / static_cast<double>(total));
                if (p.total > 0 && p.received >= p.total) {
                    before += p.total;
                }
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(), [self, fraction] { if (self) self->setJob(self->phase(), fraction); }, Qt::QueuedConnection);
            },
            stop.get_token());
        const QString message = ensured ? QString() : QString::fromStdString(ensured.error().message);
        return [self, message]() {
            if (!self) {
                return;
            }
            self->setJob({}, 0.0);
            self->m_message = message;
            emit self->resultChanged();
        };
    });
    return started ? QString() : tr("Busy");
}

QVariantMap TranslateBridge::consentPrompt(int cues) const {
    if (!cloudAvailable()) {
        return {};
    }
    auto resolved = m_ai->platform().resolve(ai::Capability::Chat);
    if (!resolved || resolved->empty()) {
        return {};
    }
    return m_ai->consentPrompt(qs(resolved->front().model->id), QStringLiteral("chat"),
                               tr("the text of %n caption cue(s), with redacted words as [bleep]", nullptr, cues));
}

QString TranslateBridge::apply() { return commitStaged(tr("Translate captions")); }

std::shared_ptr<command::CommandServices> TranslateBridge::agentServices() {
    std::shared_ptr<ai::IChatModel> chat;
    if (cloudAvailable()) {
        auto resolved = m_ai->platform().resolve(ai::Capability::Chat);
        if (resolved && !resolved->empty()) {
            if (auto model = m_ai->platform().chat(resolved->front().model->id, ai::CallOptions{false, {}})) {
                chat = std::shared_ptr<ai::IChatModel>(std::move(*model));
            }
        }
    }
    ai::translate::TranslateServices::ChatOpener opener;
    if (chat) {
        opener = [chat]() -> std::expected<std::unique_ptr<ai::IChatModel>, ai::AiError> { return std::make_unique<BorrowedChat>(chat); };
    }
    return std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
        std::make_shared<ai::translate::TranslateServices>(localOpener(), opener), std::make_shared<command::CaptionServices>(qtLineBreaker())});
}

// --- DubBridge ---------------------------------------------------------------------------------------------------------

namespace {

/// The assistant's host: dub.create / narration.create prefill the Dub dialog; nothing is generated or spent.
class ProposalDubServices final : public ai::dub::DubServices {
public:
    QPointer<DubBridge> owner;

    bool proposes() const override { return true; }

    command::CommandOutput dub(const ai::dub::DubPlan& plan, command::ExecutionContext& context) override {
        if (!owner) {
            command::fail(command::ErrorCode::InvalidRequest, "Dub is unavailable");
        }
        const ai::dub::DubRequest& r = plan.request;
        QVariantMap options{{QStringLiteral("kind"), r.kind == ai::dub::DubKind::Dub ? QStringLiteral("dub") : QStringLiteral("narration")},
                            {QStringLiteral("language"), qs(plan.language)},
                            {QStringLiteral("model"), qs(plan.model.id)},
                            {QStringLiteral("voice"), plan.clone ? QString() : qs(plan.voice)},
                            {QStringLiteral("style"), qs(r.style)},
                            {QStringLiteral("maxStretch"), r.maxStretch},
                            {QStringLiteral("duck"), plan.duckDb},
                            {QStringLiteral("name"), qs(plan.trackName)}};
        if (r.kind == ai::dub::DubKind::Dub) {
            options[QStringLiteral("captionTrack")] = static_cast<qint64>(r.captionTrack.value);
        } else {
            options[QStringLiteral("text")] = qs(r.text);
        }
        if (r.cloneVoice) {
            options[QStringLiteral("cloneVoice")] = true; // the user still ticks the rights box in the dialog
            options[QStringLiteral("speaker")] = static_cast<qint64>(r.cloneSpeaker.value);
        }
        QPointer<DubBridge> target = owner;
        QMetaObject::invokeMethod(
            target, [target, options] { if (target) target->propose(options); }, Qt::QueuedConnection);
        command::CommandOutput out;
        out.state = context.state();
        out.data = ai::dub::planToJson(plan);
        out.data["status"] = "approval-required";
        out.data["message"] = "Review the prefilled Dub dialog: voice, cost, cap and consent. No speech was generated or paid for.";
        return out;
    }
};

} // namespace

DubBridge* DubBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new DubBridge;
    bridge->setEngine(qmlEngine);
    if (qmlEngine) {
        bridge->m_ai = qmlEngine->singletonInstance<::AiController*>("Wabcut", "AiController");
        bridge->stopJobsWith(bridge->m_ai); // its voice provider (A5-P2-19)
        bridge->m_transcripts = qmlEngine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
        if (bridge->m_controller) {
            QObject::connect(bridge->m_controller, &ProjectController::revisionChanged, bridge, &DubBridge::openPending);
        }
    }
    return bridge;
}

DubBridge::DubBridge(QObject* parent) : CreatorBase(parent) {}

bool DubBridge::hasCaptionTrack(qint64 id) const {
    for (const QVariant& t : captionTracks()) {
        if (t.toMap().value(QStringLiteral("id")).toLongLong() == id) {
            return true;
        }
    }
    return false;
}

void DubBridge::openPending() {
    if (m_pending.isEmpty() || !hasCaptionTrack(m_pending.value(QStringLiteral("captionTrack")).toLongLong())) {
        return;
    }
    m_proposal = std::exchange(m_pending, {});
    emit proposalChanged();
    emit proposalRequested();
}

QVariantList DubBridge::captionTracks() const { return captionTracksOf(m_controller); }

QVariantList DubBridge::speakers() const {
    QVariantList out;
    if (!m_controller) {
        return out;
    }
    for (const Speaker& s : m_controller->snapshot()->speakers) {
        out.append(QVariantMap{{QStringLiteral("id"), static_cast<qint64>(s.id.value)}, {QStringLiteral("name"), qs(s.name)}});
    }
    return out;
}

bool DubBridge::cloudAvailable() const {
    if (m_services) {
        return true; // tests
    }
    return m_ai && !m_ai->offlineMode() && !(m_controller && m_controller->snapshot()->aiNoCloud);
}

QVariantList DubBridge::voices(const QString& model) const {
    QVariantList out;
    const ai::ModelSpec* spec = ai::ModelRegistry::builtin().model(model.toStdString());
    if (!spec) {
        return out;
    }
    std::string api = spec->api;
    if (api.empty()) {
        if (const ai::ProviderSpec* p = ai::ModelRegistry::builtin().provider(spec->provider)) {
            api = p->api;
        }
    }
    for (const ai::dub::VoiceInfo& v : ai::dub::builtinVoices(api)) {
        out.append(QVariantMap{{QStringLiteral("id"), qs(v.id)}, {QStringLiteral("name"), qs(v.name)}, {QStringLiteral("description"), qs(v.description)}});
    }
    return out;
}

Json DubBridge::paramsOf(const QVariantMap& options, bool estimate) const {
    Json p = Json::object();
    const bool narration = options.value(QStringLiteral("kind")).toString() == QLatin1String("narration");
    if (narration) {
        p["text"] = options.value(QStringLiteral("text")).toString().toStdString();
        p["onBeats"] = options.value(QStringLiteral("onBeats"), true).toBool();
        if (options.contains(QStringLiteral("at"))) {
            p["at"] = static_cast<std::int64_t>(std::llround(options.value(QStringLiteral("at")).toDouble() * static_cast<double>(kTicksPerSecond)));
        }
    } else if (options.contains(QStringLiteral("captionTrack"))) {
        p["captionTrack"] = options.value(QStringLiteral("captionTrack")).toLongLong();
    }
    for (const char* key : {"language", "model", "voice", "style", "name"}) {
        if (const QString v = options.value(QString::fromLatin1(key)).toString().trimmed(); !v.isEmpty()) {
            p[key] = v.toStdString();
        }
    }
    for (const char* key : {"maxStretch", "duck"}) {
        if (options.contains(QString::fromLatin1(key))) {
            p[key] = options.value(QString::fromLatin1(key)).toDouble();
        }
    }
    if (options.value(QStringLiteral("cloneVoice")).toBool()) {
        p["cloneVoice"] = true;
        p["rightsConfirmed"] = options.value(QStringLiteral("rightsConfirmed")).toBool();
        if (options.contains(QStringLiteral("speaker"))) {
            p["speaker"] = options.value(QStringLiteral("speaker")).toLongLong();
        }
    }
    if (estimate) {
        p["estimate"] = true;
    } else if (options.value(QStringLiteral("maxCost")).toDouble() > 0) {
        p["maxCost"] = options.value(QStringLiteral("maxCost")).toDouble();
    }
    return p;
}

std::shared_ptr<command::CommandServices> DubBridge::jobServices(bool allowOnce) {
    if (m_services) {
        return m_services; // tests
    }
    // The speech is opened here, on the GUI thread, for the model the run uses; the job borrows it.
    // run() opens the chosen model's guarded speech on the GUI thread (with the consent answer and the run's cap) into
    // this map; the job borrows it.
    (void)allowOnce;
    auto synths = std::make_shared<std::map<std::string, std::shared_ptr<ai::ISpeechSynth>>>();
    auto opener = [synths](const ai::ModelSpec& model, std::optional<ai::MicroUsd>) -> std::expected<std::unique_ptr<ai::ISpeechSynth>, ai::AiError> {
        const auto it = synths->find(model.id);
        if (it == synths->end() || !it->second) {
            return std::unexpected(ai::AiError(ai::AiErrorCode::NoCredentials, "set up " + model.name + " in Preferences > AI first"));
        }
        return std::make_unique<BorrowedSpeech>(it->second);
    };
    m_openedSpeech = synths;
    const std::filesystem::path cache = ai::userCacheDir();
    return std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
        creatorServices(m_engine, m_transcripts, projectPath(m_engine)),
        std::make_shared<ai::dub::DirectDubServices>(opener, ai::dub::makeMediaSpeechAudio(cache / "tts"), cache / "generated" / "dub")});
}

QString DubBridge::estimate(const QVariantMap& options) {
    if (!m_controller) {
        return tr("No project");
    }
    const bool narration = options.value(QStringLiteral("kind")).toString() == QLatin1String("narration");
    QPointer<DubBridge> self(this);
    const bool started = runJob(
        tr("Pricing the voice-over"), m_controller->snapshot(), narration ? "narration.create" : "dub.create", paramsOf(options, true), false,
        [self](Run&& run) {
            if (!run.error) {
                self->m_plan = variantOf(run.data).toMap();
                self->m_message = joined(run.warnings);
            } else {
                self->m_plan.clear();
                self->m_message = errorText(run.error, {});
            }
            emit self->resultChanged();
        },
        m_services ? m_services : creatorServices(m_engine, m_transcripts, projectPath(m_engine)));
    return started ? QString() : tr("Busy");
}

QString DubBridge::run(const QVariantMap& options) {
    if (!m_controller) {
        return tr("No project");
    }
    if (!cloudAvailable()) {
        return tr("Voice-over needs your cloud voice provider, and this project is no-cloud or offline");
    }
    if (options.value(QStringLiteral("maxCost")).toDouble() <= 0) {
        return tr("Set a spending cap for this run");
    }
    if (options.value(QStringLiteral("cloneVoice")).toBool() && !options.value(QStringLiteral("rightsConfirmed")).toBool()) {
        return tr("Confirm that you have the speaker's permission to clone their voice");
    }
    const bool narration = options.value(QStringLiteral("kind")).toString() == QLatin1String("narration");
    std::shared_ptr<command::CommandServices> services = jobServices(options.value(QStringLiteral("allowOnce")).toBool());
    if (!m_services && m_ai) {
        // Open the chosen model's guarded speech now (GUI thread), with the run's cap.
        std::string model = options.value(QStringLiteral("model")).toString().toStdString();
        if (model.empty()) {
            model = "eleven_v3";
        }
        const auto cap = static_cast<ai::MicroUsd>(std::llround(options.value(QStringLiteral("maxCost")).toDouble() * 1e6));
        auto synth = m_ai->platform().speech(model, ai::CallOptions{options.value(QStringLiteral("allowOnce")).toBool(), cap});
        if (!synth) {
            return qs(synth.error().message);
        }
        if (m_openedSpeech) {
            (*m_openedSpeech)[model] = std::shared_ptr<ai::ISpeechSynth>(std::move(*synth));
        }
    }
    const ProjectSnapshot base = m_controller->snapshot();
    discard();
    m_report.clear();
    const std::uint64_t revision = m_controller->session().revision();
    QPointer<DubBridge> self(this);
    const bool started = runJob(
        narration ? tr("Recording the narration") : tr("Dubbing"), base, narration ? "narration.create" : "dub.create",
        paramsOf(options, false), true,
        [self, base, revision](Run&& run) {
            if (!run.error && run.preview) {
                self->m_staged = run.preview;
                self->m_stagedBase = base;
                self->m_stagedRevision = revision;
                self->m_report = variantOf(run.data).toMap();
                self->m_message = joined(run.warnings);
                if (self->m_controller) {
                    self->showGhost(self->m_controller->currentSequenceId());
                }
            } else {
                self->m_message = errorText(run.error, tr("No speech was made"));
            }
            emit self->resultChanged();
        },
        services);
    return started ? QString() : tr("Busy");
}

QString DubBridge::previewVoice(const QVariantMap& options) {
    std::string model = options.value(QStringLiteral("model")).toString().toStdString();
    if (model.empty()) {
        model = "eleven_v3";
    }
    const ai::ModelSpec* spec = ai::ModelRegistry::builtin().model(model);
    if (!spec) {
        return tr("Unknown voice model");
    }
    QString text = options.value(QStringLiteral("text")).toString().trimmed();
    if (text.isEmpty()) {
        const QVariantList script = m_plan.value(QStringLiteral("script")).toList();
        text = script.isEmpty() ? tr("Hello, this is the voice of your video.") : script.front().toMap().value(QStringLiteral("text")).toString();
    }
    text = text.left(200);
    ai::SpeechRequest request;
    request.text = text.toStdString();
    request.voiceId = options.value(QStringLiteral("voice")).toString().toStdString();
    request.language = options.value(QStringLiteral("language")).toString().toStdString();
    request.style = options.value(QStringLiteral("style")).toString().toStdString();
    std::shared_ptr<ai::ISpeechSynth> synth = m_previewSynth; // tests
    if (!synth) {
        if (!m_ai || !cloudAvailable()) {
            return tr("Voice previews need your cloud voice provider");
        }
        // A sample costs a few cents at most: capped at $0.05.
        auto opened = m_ai->platform().speech(model, ai::CallOptions{options.value(QStringLiteral("allowOnce")).toBool(), ai::MicroUsd{50'000}});
        if (!opened) {
            return qs(opened.error().message);
        }
        synth = std::shared_ptr<ai::ISpeechSynth>(std::move(*opened));
    }
    if (request.voiceId.empty()) {
        request.voiceId = ai::dub::defaultVoice(spec->api);
    }
    const std::filesystem::path dir = ai::userCacheDir() / "tts";
    setJob(tr("Speaking a sample"), -1.0);
    QPointer<DubBridge> self(this);
    const bool open = m_openSamples;
    const bool started = m_job.start(this, m_synchronous, [self, synth, request, dir, open](const std::atomic<bool>&) -> BridgeJob::Finish {
        auto spoken = synth->synthesize(request, {});
        QString message;
        QString file;
        if (!spoken) {
            message = QString::fromStdString(spoken.error().message);
        } else {
            auto audio = ai::dub::makeMediaSpeechAudio(dir);
            auto decoded = audio->decode(*spoken, 48000);
            if (!decoded) {
                message = QString::fromStdString(decoded.error().message);
            } else {
                const auto path = dir / ("voice-sample-" + request.voiceId + ".wav");
                auto written = audio->write(path, *decoded, 48000);
                message = written ? QString() : QString::fromStdString(written.error().message);
                file = written ? QString::fromStdWString(path.wstring()) : QString();
            }
        }
        return [self, message, file, open]() {
            if (!self) {
                return;
            }
            self->setJob({}, 0.0);
            self->m_message = message;
            self->m_lastSample = file;
            if (!file.isEmpty() && open) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(file));
            }
            emit self->resultChanged();
        };
    });
    return started ? QString() : tr("Busy");
}

QVariantMap DubBridge::consentPrompt(const QString& model, int characters, bool clone) const {
    if (!m_ai) {
        return {};
    }
    const QString id = model.isEmpty() ? QStringLiteral("eleven_v3") : model;
    if (!clone) {
        return m_ai->consentPrompt(id, QStringLiteral("speak"),
                                   tr("%n character(s) of text to speak (no audio or video of the project)", nullptr, characters));
    }
    // Cloning uploads a person's voice (review wave 5, A5-P2-23 / S5-P2-9): its own consent, which a remembered
    // "Always allow" for speech does not cover, and a prompt that says what goes. Asked while either is missing.
    QVariantMap prompt = m_ai->consentPrompt(
        id, QStringLiteral("cloneVoice"),
        tr("a recording of the speaker's voice (up to 90 s of their speech in this project) to make a voice from, "
           "deleted after the run, and %n character(s) of text to speak", nullptr, characters));
    const bool speech = m_ai->consentPrompt(id, QStringLiteral("speak"), QString()).value(QStringLiteral("needed")).toBool();
    prompt[QStringLiteral("needed")] = prompt.value(QStringLiteral("needed")).toBool() || speech;
    return prompt;
}

QString DubBridge::apply() { return commitStaged(tr("Dub")); }

void DubBridge::clearProposal() {
    m_proposal.clear();
    emit proposalChanged();
}

void DubBridge::propose(const QVariantMap& options) {
    if (options.contains(QStringLiteral("captionTrack")) && !hasCaptionTrack(options.value(QStringLiteral("captionTrack")).toLongLong())) {
        m_pending = options; // the assistant's own translation: the dialog opens once it is applied
        return;
    }
    m_pending.clear();
    m_proposal = options;
    emit proposalChanged();
    emit proposalRequested();
}

std::shared_ptr<command::CommandServices> DubBridge::proposalServices() {
    auto services = std::make_shared<ProposalDubServices>();
    services->owner = this;
    return services;
}

} // namespace wabcut::app
