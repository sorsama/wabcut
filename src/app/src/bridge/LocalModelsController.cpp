#include "LocalModelsController.h"

#include "AiController.h"

#include <wabcut/ai/HttpDownload.h>
#include <wabcut/ai/ModelDownloads.h>
#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/Provider.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/ai/llm/LocalRuntime.h>
#include <wabcut/ai/llm/Tiers.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMetaObject>
#include <QQmlEngine>
#include <QSettings>
#include <QThreadPool>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <set>
#include <stop_token>

using namespace wabcut::ai;
namespace lib = wabcut::ai::library;

namespace wabcut::app {

namespace {

constexpr char kFolderKey[] = "ai/localModelFolder";
constexpr char kWhisperKey[] = "ai/localWhisperModel";
constexpr char kLaunchGroup[] = "ai/localLaunch";

QString qs(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
std::string str(const QString& s) { return s.toStdString(); }

QString sizeText(std::uint64_t bytes) {
    if (bytes == 0)
        return {};
    if (bytes < 1'000'000'000ULL)
        return QCoreApplication::translate("LocalModels", "%1 MB").arg(std::max<std::uint64_t>(1, bytes / 1'000'000ULL));
    return QCoreApplication::translate("LocalModels", "%1 GB").arg(static_cast<double>(bytes) / 1e9, 0, 'f', 1);
}

QString countText(std::int64_t n) {
    if (n >= 1'000'000)
        return QStringLiteral("%1M").arg(static_cast<double>(n) / 1e6, 0, 'f', 1);
    if (n >= 1'000)
        return QStringLiteral("%1K").arg(static_cast<double>(n) / 1e3, 0, 'f', 1);
    return QString::number(n);
}

QString paramsText(double b) {
    if (b <= 0.0)
        return {};
    if (b < 1.0)
        return QStringLiteral("%1M").arg(std::lround(b * 1000.0));
    return QStringLiteral("%1B").arg(b, 0, 'f', b < 10.0 ? 1 : 0);
}

QString taskName(lib::Task t) {
    switch (t) {
    case lib::Task::Chat: return QCoreApplication::translate("LocalModels", "Assistant");
    case lib::Task::Coding: return QCoreApplication::translate("LocalModels", "Coding");
    case lib::Task::Reasoning: return QCoreApplication::translate("LocalModels", "Reasoning");
    case lib::Task::Vision: return QCoreApplication::translate("LocalModels", "Vision");
    case lib::Task::Transcription: return QCoreApplication::translate("LocalModels", "Transcription");
    case lib::Task::Translation: return QCoreApplication::translate("LocalModels", "Translation");
    }
    return {};
}

QVariantList option(std::initializer_list<std::pair<const char*, QString>> items) {
    QVariantList out;
    for (const auto& [id, name] : items)
        out.push_back(QVariantMap{{QStringLiteral("id"), QString::fromLatin1(id)}, {QStringLiteral("name"), name}});
    return out;
}

/// "https://huggingface.co/<owner>/<name>/resolve/..." -> "<owner>/<name>".
std::string repoOfUrl(std::string_view url) {
    constexpr std::string_view prefix = "https://huggingface.co/";
    if (!url.starts_with(prefix))
        return {};
    url.remove_prefix(prefix.size());
    const auto resolve = url.find("/resolve/");
    return resolve == std::string_view::npos ? std::string() : std::string(url.substr(0, resolve));
}

std::filesystem::path fromQt(const QString& s) { return std::filesystem::path(s.toStdU16String()); }
QString toQt(const std::filesystem::path& p) { return QString::fromStdU16String(p.u16string()); }

/// Free bytes on the drive of `folder` (its nearest existing parent).
std::optional<std::uint64_t> freeBytes(const std::filesystem::path& folder) {
    std::error_code ec;
    std::filesystem::path p = folder;
    while (!p.empty() && !std::filesystem::exists(p, ec)) {
        if (p.parent_path() == p)
            break;
        p = p.parent_path();
    }
    const auto info = std::filesystem::space(p, ec);
    if (ec)
        return std::nullopt;
    return info.available;
}

const ModelInfo* builtin(std::string_view id, ModelKind kind) {
    for (const ModelInfo& m : builtinModels()) {
        if (m.id == id && m.kind == kind)
            return &m;
    }
    return nullptr;
}

/// The curated launch settings, "context,gpuLayers,threads" in QSettings ai/localLaunch/<id>.
llm::LaunchOverrides storedLaunch(const QString& id) {
    const QStringList parts = QSettings().value(QLatin1String(kLaunchGroup) + QLatin1Char('/') + id).toString().split(QLatin1Char(','));
    llm::LaunchOverrides o;
    if (parts.size() == 3) {
        o.context = parts[0].toInt();
        o.gpuLayers = parts[1].toInt();
        o.threads = parts[2].toInt();
    }
    return o;
}

std::uint64_t installedBytes(const ModelStore& store, const ModelInfo& m) { return store.isInstalled(m) ? m.size : 0; }

} // namespace

// ---- Transports ---------------------------------------------------------------------------------------------------------

namespace {

/// Adds the Hugging Face access token to requests for https://huggingface.co itself (gated repositories) and to nothing
/// else: redirect hops to Hugging Face's file CDN, other hosts and plain http never carry it.
class HubAuthTransport final : public IHttpTransport {
public:
    void setInner(IHttpTransport* inner) {
        std::lock_guard lock(m_mutex);
        m_inner = inner;
    }
    void setToken(std::string token) {
        std::lock_guard lock(m_mutex);
        m_token = Secret(std::move(token));
    }
    std::expected<HttpResponse, AiError> send(const HttpRequest& request, const HttpStreamHandlers& handlers,
                                              std::stop_token stop) override {
        HttpRequest r = request;
        IHttpTransport* inner = nullptr;
        {
            std::lock_guard lock(m_mutex);
            inner = m_inner;
            const auto url = parseHttpUrl(r.url);
            if (url && url->scheme == "https" && url->host == "huggingface.co" && !url->port && !m_token.empty() &&
                !findHeader(r.headers, "authorization"))
                r.headers.push_back({"Authorization", "Bearer " + std::string(m_token.reveal())});
        }
        if (!inner)
            return std::unexpected(AiError(AiErrorCode::Network, "no network"));
        return inner->send(r, handlers, stop);
    }

private:
    std::mutex m_mutex;
    IHttpTransport* m_inner = nullptr;
    Secret m_token;
};

} // namespace

struct LocalModelsController::Hub {
    HubAuthTransport auth;
    HttpDownloadTransport downloads{auth};
    QThreadPool pool;
    std::stop_source stop;
};

/// What a finished download does.
struct LocalModelsController::Pending {
    std::optional<lib::InstalledModel> install; ///< a library model: written to the index and registered
    QString kind;                               ///< "library", "curated", "whisper"
    bool use = false;                           ///< then use it (assistant / transcription)
};

// ---- Start-up -----------------------------------------------------------------------------------------------------------

void loadLocalModelLibrary(AiController& ai) {
    QSettings settings;
    if (const QString folder = settings.value(QLatin1String(kFolderKey)).toString(); !folder.isEmpty())
        ModelStore::setDefaultRoot(fromQt(folder));
    settings.beginGroup(QLatin1String(kLaunchGroup));
    for (const QString& id : settings.childKeys())
        llm::setLaunchOverrides(str(id), storedLaunch(id));
    settings.endGroup();
    lib::LibraryIndex index(ModelStore::defaultRoot());
    const bool loaded = index.load().has_value();
    // QA-10: models another storage folder held (the folder changed) leave the model store's and the routing's lists:
    // nothing here backs them any more.
    std::set<std::string, std::less<>> present;
    if (loaded) {
        for (const lib::InstalledModel& m : index.models()) {
            for (const ModelInfo& info : lib::modelInfosOf(m))
                present.insert(info.id);
        }
    }
    for (const ModelInfo& info : registeredModels()) {
        if (!present.contains(info.id))
            unregisterModel(info.id);
    }
    for (const QString& id : ai.libraryModelIds()) {
        if (!present.contains(str(id)))
            (void)ai.unregisterLocalModel(id);
    }
    if (!loaded)
        return; // a damaged index: the library page says so when it opens
    for (const lib::InstalledModel& m : index.models()) {
        for (ModelInfo& info : lib::modelInfosOf(m))
            (void)registerModel(std::move(info));
        llm::setLaunchOverrides(m.id, {m.context, m.gpuLayers, m.threads});
        (void)ai.registerLocalModel(lib::registrySpecOf(m));
    }
}

// ---- Construction ---------------------------------------------------------------------------------------------------------

LocalModelsController::LocalModelsController(QObject* parent) : QObject(parent), m_hub(std::make_unique<Hub>()) {
    m_hub->pool.setMaxThreadCount(2);
}

LocalModelsController::~LocalModelsController() {
    *m_alive = false;
    m_hub->stop.request_stop();
    m_queue.reset(); // pauses the running download (its partial file stays) and joins the worker
    m_hub->pool.waitForDone();
}

LocalModelsController* LocalModelsController::create(QQmlEngine* engine, QJSEngine*) {
    auto* self = new LocalModelsController;
    if (engine)
        self->setAiController(engine->singletonInstance<AiController*>("Wabcut", "AiController"));
    return self;
}

void LocalModelsController::setAiController(AiController* ai) {
    if (m_ai)
        disconnect(m_ai, nullptr, this, nullptr);
    m_ai = ai;
    if (ai) {
        connect(ai, &AiController::privacyChanged, this, &LocalModelsController::offlineChanged);
        connect(ai, &AiController::modelsChanged, this, [this] { rebuildLibrary(); });
        // The platform and its network stack go away after this: stop the queue and the searches first.
        connect(ai, &AiController::shuttingDown, this, [this] {
            m_hub->stop.request_stop();
            m_queue.reset();
            m_hub->pool.waitForDone();
            m_hub->auth.setInner(nullptr);
        });
    }
    emit offlineChanged();
}

void LocalModelsController::setTransportForTesting(IHttpTransport* transport) { m_testTransport = transport; }
void LocalModelsController::setProbeForTesting(std::optional<llm::SystemProbe> p) {
    m_testProbe = std::move(p);
    m_probe.reset();
}
void LocalModelsController::setFreeSpaceForTesting(std::optional<std::uint64_t> bytes) { m_testFreeSpace = bytes; }

IHttpTransport* LocalModelsController::transport() {
    IHttpTransport* inner = m_testTransport ? m_testTransport : (m_ai ? &m_ai->transport() : nullptr);
    m_hub->auth.setInner(inner);
    return inner ? &m_hub->auth : nullptr;
}

DownloadQueue& LocalModelsController::queue() {
    if (!m_queue) {
        (void)transport(); // the network stack exists before the worker uses it (created on the GUI thread)
        DownloadQueue::Options options;
        QPointer<LocalModelsController> self(this);
        options.onChanged = [self] {
            QMetaObject::invokeMethod(self, [self] {
                if (self)
                    self->updateDownloads();
            }, Qt::QueuedConnection);
        };
        options.onFinished = [self](const DownloadState& state) {
            QMetaObject::invokeMethod(self, [self, state] {
                if (self)
                    self->finished(state);
            }, Qt::QueuedConnection);
        };
        if (m_testFreeSpace) {
            const std::uint64_t free = *m_testFreeSpace;
            options.freeSpace = [free](const std::filesystem::path&) { return std::optional<std::uint64_t>(free); };
        }
        m_queue = std::make_unique<DownloadQueue>(m_hub->downloads, std::move(options));
    }
    return *m_queue;
}

llm::SystemProbe LocalModelsController::probe() const {
    if (m_testProbe)
        return *m_testProbe;
    if (!m_probe)
        m_probe = llm::probeSystem();
    return *m_probe;
}

bool LocalModelsController::offline() const { return m_ai && (m_ai->offlineMode() || m_ai->projectLock()); }

void LocalModelsController::readHubToken() {
    m_hasToken = false;
    m_hub->auth.setToken({});
    if (m_testTransport || !m_ai)
        return;
    const ProviderSpec* spec = m_ai->platform().providers().models().provider("huggingface");
    ProviderSpec fallback;
    if (!spec) {
        fallback.id = "huggingface"; // the key ring entry the Hugging Face provider uses
        fallback.envVar = "WABCUT_HUGGINGFACE_API_KEY";
        spec = &fallback;
    }
    auto key = m_ai->platform().keys().get(*spec);
    if (key && !key->empty()) {
        m_hasToken = true;
        m_hub->auth.setToken(std::string(key->reveal()));
    }
}

// ---- Static helpers -------------------------------------------------------------------------------------------------------

QString LocalModelsController::transcriptionModelId() {
    const QString chosen = QSettings().value(QLatin1String(kWhisperKey)).toString();
    if (!chosen.isEmpty()) {
        if (const ModelInfo* m = findModel(str(chosen)); m && m->kind == ModelKind::Whisper)
            return chosen;
    }
    return automaticWhisperModel();
}

QString LocalModelsController::automaticWhisperModel() {
    ModelStore store(ModelStore::defaultRoot());
    const ModelInfo* turbo = findModel("large-v3-turbo-q5_0");
    if (turbo && store.isInstalled(*turbo))
        return QStringLiteral("large-v3-turbo-q5_0");
    const ModelInfo* base = findModel("base");
    if (base && store.isInstalled(*base))
        return QStringLiteral("base");
    // Probing the GPUs is not free and the transcript panel reads this from bindings: the answer is kept for a while.
    static std::mutex mutex;
    static std::optional<bool> gpuCapable;
    static std::chrono::steady_clock::time_point probedAt;
    std::lock_guard lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    if (!gpuCapable || now - probedAt > std::chrono::minutes(5)) {
        const llm::SystemProbe p = llm::probeSystem();
        const llm::GpuInfo* gpu = p.bestGpu();
        gpuCapable = gpu && !gpu->integrated && gpu->freeBytes >= (std::uint64_t{2} << 30);
        probedAt = now;
    }
    return *gpuCapable ? QStringLiteral("large-v3-turbo-q5_0") : QStringLiteral("base");
}

// ---- Options ----------------------------------------------------------------------------------------------------------

QVariantList LocalModelsController::sortOptions() const {
    return option({{"recommended", tr("Recommended")},
                   {"benchmark", tr("Benchmark score")},
                   {"downloads", tr("Most downloaded")},
                   {"likes", tr("Most liked")},
                   {"trending", tr("Trending")},
                   {"newest", tr("Newest")},
                   {"updated", tr("Recently updated")},
                   {"smallest", tr("Smallest")}});
}
QVariantList LocalModelsController::taskOptions() const {
    return option({{"", tr("All")},
                   {"chat", tr("Assistant")},
                   {"coding", tr("Coding")},
                   {"reasoning", tr("Reasoning")},
                   {"vision", tr("Vision")},
                   {"translation", tr("Translation")}});
}
QVariantList LocalModelsController::sizeOptions() const {
    return option({{"any", tr("Any size")},
                   {"small", tr("Under 3 GB")},
                   {"medium", tr("3 to 8 GB")},
                   {"large", tr("8 to 20 GB")},
                   {"huge", tr("Over 20 GB")}});
}
QVariantList LocalModelsController::paramOptions() const {
    return option({{"any", tr("Any")},
                   {"tiny", tr("Under 3B")},
                   {"small", tr("3B to 9B")},
                   {"medium", tr("9B to 20B")},
                   {"large", tr("20B to 40B")},
                   {"huge", tr("Over 40B")}});
}
QVariantList LocalModelsController::quantOptions() const {
    return option({{"", tr("Any")},
                   {"Q4_K_M", QStringLiteral("Q4_K_M")},
                   {"Q5_K_M", QStringLiteral("Q5_K_M")},
                   {"Q6_K", QStringLiteral("Q6_K")},
                   {"Q8_0", QStringLiteral("Q8_0")},
                   {"IQ4_XS", QStringLiteral("IQ4_XS")},
                   {"Q4_0", QStringLiteral("Q4_0")},
                   {"F16", QStringLiteral("F16")}});
}
QVariantList LocalModelsController::publisherOptions() const {
    return option({{"any", tr("Anyone")}, {"trusted", tr("Official or well-known")}, {"official", tr("Official only")}});
}
QVariantList LocalModelsController::languageOptions() const {
    return option({{"", tr("Any language")},
                   {"en", tr("English")},
                   {"th", tr("Thai")},
                   {"zh", tr("Chinese")},
                   {"ja", tr("Japanese")},
                   {"ko", tr("Korean")},
                   {"es", tr("Spanish")},
                   {"fr", tr("French")},
                   {"de", tr("German")},
                   {"pt", tr("Portuguese")},
                   {"ru", tr("Russian")},
                   {"vi", tr("Vietnamese")},
                   {"id", tr("Indonesian")}});
}

QString LocalModelsController::sortKey() const { return qs(lib::toString(m_sort)); }

void LocalModelsController::setQuery(const QString& query) {
    if (query == m_query)
        return;
    m_query = query;
    emit queryChanged();
}

void LocalModelsController::setSortKey(const QString& key) {
    const auto parsed = lib::sortKeyFromString(str(key));
    if (!parsed || *parsed == m_sort)
        return;
    const auto serverSide = [](lib::SortKey k) {
        return k == lib::SortKey::Downloads || k == lib::SortKey::Likes || k == lib::SortKey::Trending ||
               k == lib::SortKey::Newest || k == lib::SortKey::Updated;
    };
    const bool refetch = m_searched && (serverSide(*parsed) || serverSide(m_sort));
    m_sort = *parsed;
    emit queryChanged();
    if (refetch)
        search(); // the Hub orders these: another page of results
    else
        rebuildResults();
}

QVariantMap LocalModelsController::filters() const {
    return {{QStringLiteral("task"), m_filter.task ? qs(lib::toString(*m_filter.task)) : QString()},
            {QStringLiteral("fitsOnly"), m_filter.fitsOnly},
            {QStringLiteral("size"), qs(m_filter.size)},
            {QStringLiteral("params"), qs(m_filter.params)},
            {QStringLiteral("quant"), qs(m_filter.quant)},
            {QStringLiteral("permissiveOnly"), m_filter.permissiveOnly},
            {QStringLiteral("language"), qs(m_filter.language)},
            {QStringLiteral("publisher"), qs(m_filter.publisher)},
            {QStringLiteral("installedOnly"), m_filter.installedOnly}};
}

void LocalModelsController::setFilter(const QString& key, const QVariant& value) {
    const std::string text = str(value.toString());
    if (key == QLatin1String("task"))
        m_filter.task = lib::taskFromString(text);
    else if (key == QLatin1String("fitsOnly"))
        m_filter.fitsOnly = value.toBool();
    else if (key == QLatin1String("size"))
        m_filter.size = text.empty() ? "any" : text;
    else if (key == QLatin1String("params"))
        m_filter.params = text.empty() ? "any" : text;
    else if (key == QLatin1String("quant"))
        m_filter.quant = text;
    else if (key == QLatin1String("permissiveOnly"))
        m_filter.permissiveOnly = value.toBool();
    else if (key == QLatin1String("language"))
        m_filter.language = text;
    else if (key == QLatin1String("publisher"))
        m_filter.publisher = text.empty() ? "any" : text;
    else if (key == QLatin1String("installedOnly"))
        m_filter.installedOnly = value.toBool();
    else
        return;
    emit queryChanged();
    rebuildResults();
}

void LocalModelsController::resetFilters() {
    m_filter = lib::Filter{};
    emit queryChanged();
    rebuildResults();
}

// ---- Searching ----------------------------------------------------------------------------------------------------------

void LocalModelsController::search() {
    lib::HubQuery q;
    q.search = str(m_query.trimmed());
    q.sort = m_sort;
    q.limit = 50;
    m_nextPage.clear();
    runSearch(qs(lib::hubSearchUrl(q)), false);
}

void LocalModelsController::loadMore() {
    if (!m_nextPage.isEmpty() && !m_searching)
        runSearch(m_nextPage, true);
}

void LocalModelsController::runSearch(const QString& url, bool append) {
    if (offline()) {
        m_searchMessage = tr("Offline mode or the project's no-cloud lock is on: Hugging Face cannot be searched. The recommended and "
                             "downloaded models still work.");
        m_searching = false;
        emit resultsChanged();
        return;
    }
    IHttpTransport* t = transport();
    if (!t) {
        m_searchMessage = tr("The network is not available.");
        emit resultsChanged();
        return;
    }
    readHubToken();
    const int generation = ++*m_generation;
    m_searching = true;
    m_searchMessage.clear();
    emit resultsChanged();
    QPointer<LocalModelsController> self(this);
    const std::string requestUrl = str(url);
    const std::stop_token stop = m_hub->stop.get_token();
    auto counter = m_generation;
    m_hub->pool.start([self, t, requestUrl, stop, generation, counter, append] {
        HttpRequest request;
        request.method = "GET";
        request.url = requestUrl;
        request.headers = {{"Accept", "application/json"}};
        request.timeout = std::chrono::seconds(30);
        request.idleTimeout = std::chrono::seconds(20);
        std::vector<lib::Entry> entries;
        QString error;
        QString next;
        try {
            auto response = t->send(request, {}, stop);
            if (!response) {
                error = response.error().code == AiErrorCode::Offline ? QStringLiteral("offline") : qs(response.error().message);
            } else if (!response->ok()) {
                error = QStringLiteral("HTTP %1").arg(response->status);
            } else if (auto models = lib::parseHubSearch(response->body); !models) {
                error = qs(models.error().message);
            } else {
                for (lib::HubModel& m : *models) {
                    if (lib::isRunnableLlm(m))
                        entries.push_back(lib::makeEntry(std::move(m), lib::builtinBenchmarks()));
                }
                if (auto more = lib::nextPageUrl(response->headers))
                    next = qs(*more);
            }
        } catch (...) { // QThreadPool does not catch
            error = QStringLiteral("search failed");
        }
        if (stop.stop_requested())
            return;
        QMetaObject::invokeMethod(self, [self, found = std::move(entries), error, next, generation, counter, append]() mutable {
            if (!self || generation != counter->load())
                return; // a newer search replaced this one
            self->m_searching = false;
            self->m_searched = true;
            if (!error.isEmpty()) {
                self->m_searchMessage = error == QLatin1String("offline")
                    ? tr("Offline mode or the project's no-cloud lock is on: Hugging Face cannot be searched.")
                    : error.startsWith(QLatin1String("HTTP 429"))
                    ? tr("Hugging Face asks to slow down. Try again in a minute.")
                    : tr("Could not search Hugging Face: %1").arg(error);
                if (!append)
                    self->m_entries.clear();
                self->m_nextPage.clear();
            } else {
                if (!append)
                    self->m_entries.clear();
                std::set<std::string> seen;
                for (const lib::Entry& e : self->m_entries)
                    seen.insert(e.hub.id);
                for (lib::Entry& e : found) {
                    if (seen.insert(e.hub.id).second)
                        self->m_entries.push_back(std::move(e));
                }
                self->m_nextPage = next;
                self->m_searchMessage.clear();
            }
            self->rebuildResults();
        }, Qt::QueuedConnection);
    });
}

QString LocalModelsController::fitKey(lib::Fit fit) const {
    switch (fit) {
    case lib::Fit::Gpu: return QStringLiteral("gpu");
    case lib::Fit::Cpu: return QStringLiteral("cpu");
    case lib::Fit::No: return QStringLiteral("no");
    case lib::Fit::Unknown: return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

QVariantMap LocalModelsController::entryRow(const lib::Entry& e) const {
    QStringList taskNames;
    QStringList tasks;
    for (const lib::Task t : e.tasks) {
        tasks << qs(lib::toString(t));
        taskNames << taskName(t);
    }
    QVariantMap row{
        {QStringLiteral("id"), qs(e.hub.id)},
        {QStringLiteral("kind"), QStringLiteral("hub")},
        {QStringLiteral("repo"), qs(e.hub.id)},
        {QStringLiteral("name"), e.family ? qs(e.family->name) : qs(e.hub.id.substr(e.hub.id.find('/') + 1))},
        {QStringLiteral("publisher"), qs(e.hub.author)},
        {QStringLiteral("publisherKind"), qs(lib::toString(e.publisher))},
        {QStringLiteral("params"), paramsText(e.hub.paramsB)},
        {QStringLiteral("paramsB"), e.hub.paramsB},
        {QStringLiteral("activeParams"), paramsText(e.hub.activeParamsB)},
        {QStringLiteral("size"), e.sizeEstimated && e.sizeBytes > 0 ? tr("about %1 at 4-bit").arg(sizeText(e.sizeBytes)) : sizeText(e.sizeBytes)},
        {QStringLiteral("sizeBytes"), static_cast<double>(e.sizeBytes)},
        {QStringLiteral("sizeEstimated"), e.sizeEstimated},
        {QStringLiteral("quant"), qs(e.quant)},
        {QStringLiteral("license"), qs(e.hub.license)},
        {QStringLiteral("licenseName"), qs(e.hub.licenseName)},
        {QStringLiteral("permissive"), e.permissive},
        {QStringLiteral("tasks"), tasks},
        {QStringLiteral("taskNames"), taskNames.join(QStringLiteral(" · "))},
        {QStringLiteral("score"), e.score ? *e.score : -1.0},
        {QStringLiteral("scoreText"), e.score ? QString::number(std::lround(*e.score)) : QString()},
        {QStringLiteral("scoreFrom"), e.family ? qs(e.family->name) : e.ownEvals ? tr("the model's own card") : QString()},
        {QStringLiteral("ownEvals"), e.ownEvals},
        {QStringLiteral("downloads"), static_cast<double>(e.hub.downloads)},
        {QStringLiteral("downloadsText"), countText(e.hub.downloads)},
        {QStringLiteral("likes"), static_cast<double>(e.hub.likes)},
        {QStringLiteral("likesText"), countText(e.hub.likes)},
        {QStringLiteral("updated"), qs(e.hub.lastModified.substr(0, 10))},
        {QStringLiteral("created"), qs(e.hub.createdAt.substr(0, 10))},
        {QStringLiteral("gated"), e.hub.gated},
        {QStringLiteral("vision"), e.vision},
        {QStringLiteral("fit"), fitKey(e.fit)},
        {QStringLiteral("installed"), e.installed},
        {QStringLiteral("curated"), e.curated},
        {QStringLiteral("recommended"), false},
    };
    return row;
}

void LocalModelsController::rebuildResults() {
    const llm::SystemProbe p = probe();
    lib::LibraryIndex index(ModelStore::defaultRoot());
    (void)index.load();
    std::set<std::string> installedRepos;
    for (const auto& m : index.models())
        installedRepos.insert(m.repo);
    std::set<std::string> curatedRepos;
    for (const ModelInfo& m : builtinModels()) {
        if (m.kind == ModelKind::Llm)
            curatedRepos.insert(repoOfUrl(m.url));
    }
    std::vector<lib::Entry> shown;
    for (lib::Entry e : m_entries) {
        e.installed = installedRepos.contains(e.hub.id);
        e.curated = curatedRepos.contains(e.hub.id);
        const std::uint64_t bytes = e.sizeBytes > 0 ? e.sizeBytes
                                                    : static_cast<std::uint64_t>(std::max(0.0, e.hub.paramsB) * 1e9 * 4.8 / 8.0);
        if (bytes > 0) {
            const llm::Admission a = llm::estimateFit(bytes, e.hub.paramsB, p);
            e.fit = !a.fits ? lib::Fit::No : a.gpu ? lib::Fit::Gpu : lib::Fit::Cpu;
        }
        if (lib::matches(e, m_filter))
            shown.push_back(std::move(e));
    }
    lib::sortEntries(shown, m_sort);
    m_results.clear();
    for (const lib::Entry& e : shown)
        m_results.push_back(entryRow(e));
    m_hidden = static_cast<int>(m_entries.size() - shown.size());
    emit resultsChanged();
}

// ---- Model cards ---------------------------------------------------------------------------------------------------------

QVariantMap LocalModelsController::detailFor(const lib::HubModel& model, const lib::BenchmarkFamily* family,
                                             const QString& kind) const {
    const lib::BenchmarkTable& table = lib::builtinBenchmarks();
    lib::Entry entry = lib::makeEntry(model, table);
    if (family)
        entry.family = family;
    if (family && !entry.score)
        entry.score = lib::compositeScore(*family, table);
    QVariantMap d = entryRow(entry);
    d.insert(QStringLiteral("kind"), kind);
    d.insert(QStringLiteral("loading"), false);
    d.insert(QStringLiteral("error"), QString());
    d.insert(QStringLiteral("hubUrl"), qs(lib::hubPageUrl(model.id)));
    d.insert(QStringLiteral("revision"), qs(model.revision.substr(0, 12)));
    d.insert(QStringLiteral("contextLength"), static_cast<double>(model.contextLength));
    d.insert(QStringLiteral("architecture"), qs(model.architecture));
    d.insert(QStringLiteral("baseModel"), model.baseModels.empty() ? QString() : qs(model.baseModels.front()));
    d.insert(QStringLiteral("relation"), qs(model.baseRelation));
    d.insert(QStringLiteral("licenseNeedsAcceptance"), !entry.permissive);

    const llm::SystemProbe p = probe();
    lib::LibraryIndex index(ModelStore::defaultRoot());
    (void)index.load();
    const auto variants = lib::variantsOf(model.files);
    const auto projectors = lib::projectorsOf(model.files);
    const lib::HubFile* projector = lib::defaultProjector(projectors);
    const lib::Variant* chosen = lib::defaultVariant(variants, m_filter.quant);
    QVariantList variantRows;
    for (const lib::Variant& v : variants) {
        const llm::Admission a = llm::estimateFit(v.bytes + (projector ? projector->size : 0), model.paramsB, p);
        const QString id = qs(lib::libraryModelId(model.id, v.quant));
        variantRows.push_back(QVariantMap{
            {QStringLiteral("quant"), qs(v.quant)},
            {QStringLiteral("id"), id},
            {QStringLiteral("size"), sizeText(v.bytes)},
            {QStringLiteral("sizeBytes"), static_cast<double>(v.bytes)},
            {QStringLiteral("bits"), v.bits},
            {QStringLiteral("files"), static_cast<int>(v.files.size())},
            {QStringLiteral("fit"), fitKey(!a.fits ? lib::Fit::No : a.gpu ? lib::Fit::Gpu : lib::Fit::Cpu)},
            {QStringLiteral("fitText"), qs(a.reason)},
            {QStringLiteral("ram"), sizeText(a.ramNeeded)},
            {QStringLiteral("vram"), a.gpu ? sizeText(a.vramNeeded) : QString()},
            {QStringLiteral("isDefault"), chosen == &v},
            {QStringLiteral("installed"), index.find(str(id)) != nullptr},
        });
    }
    d.insert(QStringLiteral("variants"), variantRows);
    d.insert(QStringLiteral("defaultQuant"), chosen ? qs(chosen->quant) : QString());
    if (chosen) {
        const llm::Admission a = llm::estimateFit(chosen->bytes + (projector ? projector->size : 0), model.paramsB, p);
        d.insert(QStringLiteral("fit"), fitKey(!a.fits ? lib::Fit::No : a.gpu ? lib::Fit::Gpu : lib::Fit::Cpu));
        d.insert(QStringLiteral("fitText"), qs(a.reason));
        d.insert(QStringLiteral("size"), sizeText(chosen->bytes));
    }
    d.insert(QStringLiteral("projector"),
             projector ? QVariant(QVariantMap{{QStringLiteral("path"), qs(projector->path)}, {QStringLiteral("size"), sizeText(projector->size)}})
                       : QVariant());

    QVariantList scores;
    const lib::BenchmarkFamily* f = entry.family;
    if (f) {
        for (const lib::Score& s : f->scores) {
            const lib::Metric* m = table.metric(s.metric);
            scores.push_back(QVariantMap{{QStringLiteral("metric"), qs(s.metric)},
                                         {QStringLiteral("name"), m ? qs(m->name) : qs(s.metric)},
                                         {QStringLiteral("description"), m ? qs(m->description) : QString()},
                                         {QStringLiteral("value"), s.value},
                                         {QStringLiteral("mode"), qs(s.mode)},
                                         {QStringLiteral("note"), qs(s.note)},
                                         {QStringLiteral("source"), qs(s.source.empty() ? f->source : s.source)}});
        }
        d.insert(QStringLiteral("scoreSource"), qs(f->source));
        d.insert(QStringLiteral("scoreDate"), qs(f->checked));
        d.insert(QStringLiteral("scoreNote"), qs(f->note));
        d.insert(QStringLiteral("scoreFamily"), qs(f->name));
    } else {
        for (const lib::EvalResult& e : model.evals) {
            scores.push_back(QVariantMap{{QStringLiteral("metric"), qs(e.metric)},
                                         {QStringLiteral("name"), qs(e.metric)},
                                         {QStringLiteral("description"), qs(e.dataset)},
                                         {QStringLiteral("value"), e.value},
                                         {QStringLiteral("mode"), QString()},
                                         {QStringLiteral("note"), QString()},
                                         {QStringLiteral("source"), qs(lib::hubPageUrl(model.id))}});
        }
        d.insert(QStringLiteral("scoreSource"), model.evals.empty() ? QString() : qs(lib::hubPageUrl(model.id)));
        d.insert(QStringLiteral("scoreDate"), QString());
        d.insert(QStringLiteral("scoreNote"), model.evals.empty() ? QString() : tr("Results the publisher put in the model's card."));
        d.insert(QStringLiteral("scoreFamily"), QString());
    }
    d.insert(QStringLiteral("scores"), scores);
    return d;
}

void LocalModelsController::openModel(const QString& repo) {
    const std::string id = str(repo.trimmed());
    m_detailModel.reset();
    if (!lib::isValidRepoId(id)) {
        m_detail = {{QStringLiteral("repo"), repo}, {QStringLiteral("name"), repo}, {QStringLiteral("loading"), false},
                    {QStringLiteral("error"), tr("That is not a Hugging Face model id (owner/name).")}};
        emit detailChanged();
        return;
    }
    m_detail = {{QStringLiteral("id"), repo},
                {QStringLiteral("repo"), repo},
                {QStringLiteral("kind"), QStringLiteral("hub")},
                {QStringLiteral("name"), qs(id.substr(id.find('/') + 1))},
                {QStringLiteral("publisher"), qs(id.substr(0, id.find('/')))},
                {QStringLiteral("loading"), true},
                {QStringLiteral("error"), QString()},
                {QStringLiteral("hubUrl"), qs(lib::hubPageUrl(id))}};
    for (const lib::Entry& e : m_entries) {
        if (e.hub.id == id) {
            QVariantMap row = entryRow(e);
            row.insert(QStringLiteral("loading"), true);
            row.insert(QStringLiteral("error"), QString());
            row.insert(QStringLiteral("hubUrl"), qs(lib::hubPageUrl(id)));
            m_detail = row;
        }
    }
    emit detailChanged();
    if (offline()) {
        m_detail.insert(QStringLiteral("loading"), false);
        m_detail.insert(QStringLiteral("error"), tr("Offline mode or the project's no-cloud lock is on: the model's files cannot be listed."));
        emit detailChanged();
        return;
    }
    IHttpTransport* t = transport();
    if (!t)
        return;
    readHubToken();
    const int generation = ++*m_detailGeneration;
    QPointer<LocalModelsController> self(this);
    const std::stop_token stop = m_hub->stop.get_token();
    auto counter = m_detailGeneration;
    m_hub->pool.start([self, t, id, stop, generation, counter] {
        std::optional<lib::HubModel> model;
        QString error;
        try {
            HttpRequest request;
            request.method = "GET";
            request.url = lib::hubModelUrl(id);
            request.headers = {{"Accept", "application/json"}};
            request.timeout = std::chrono::seconds(30);
            request.idleTimeout = std::chrono::seconds(20);
            auto response = t->send(request, {}, stop);
            if (!response) {
                error = qs(response.error().message);
            } else if (response->status == 401 || response->status == 403) {
                error = QStringLiteral("gated");
            } else if (response->status == 404) {
                error = QStringLiteral("missing");
            } else if (!response->ok()) {
                error = QStringLiteral("HTTP %1").arg(response->status);
            } else if (auto parsed = lib::parseHubModel(response->body); !parsed) {
                error = qs(parsed.error().message);
            } else {
                model = std::move(*parsed);
                // Files without their LFS digest in the details: the tree at the same revision has them.
                if (lib::variantsOf(model->files).empty() && !model->revision.empty()) {
                    HttpRequest tree = request;
                    tree.url = lib::hubTreeUrl(id, model->revision);
                    if (auto r = t->send(tree, {}, stop); r && r->ok()) {
                        if (auto files = lib::parseHubTree(r->body))
                            model->files = std::move(*files);
                    }
                }
            }
        } catch (...) {
            error = QStringLiteral("failed");
        }
        if (stop.stop_requested())
            return;
        QMetaObject::invokeMethod(self, [self, loaded = std::move(model), error, generation, counter]() mutable {
            if (!self || generation != counter->load())
                return;
            if (!loaded) {
                self->m_detail.insert(QStringLiteral("loading"), false);
                self->m_detail.insert(QStringLiteral("error"),
                                      error == QLatin1String("gated") ? tr("This model is gated. Accept its terms on its Hugging Face page, then add "
                                                                           "your Hugging Face access token in Settings > AI > Providers.")
                                      : error == QLatin1String("missing") ? tr("Hugging Face does not know this model.")
                                                                          : tr("Could not read the model from Hugging Face: %1").arg(error));
                emit self->detailChanged();
                return;
            }
            self->m_detailModel = std::move(*loaded);
            self->m_detail = self->detailFor(*self->m_detailModel, nullptr, QStringLiteral("hub"));
            if (self->m_detail.value(QStringLiteral("variants")).toList().isEmpty())
                self->m_detail.insert(QStringLiteral("error"), tr("This repository has no GGUF file with a checksum to download."));
            emit self->detailChanged();
        }, Qt::QueuedConnection);
    });
}

void LocalModelsController::openCurated(const QString& id) {
    const ModelInfo* m = builtin(str(id), ModelKind::Llm);
    if (!m)
        return;
    lib::HubModel model;
    model.id = repoOfUrl(m->url);
    model.author = model.id.substr(0, model.id.find('/'));
    const auto resolve = m->url.find("/resolve/");
    model.revision = m->url.substr(resolve + 9, 40);
    model.license = m->license;
    model.licenseName = lib::licenseDisplayName(m->license);
    model.files = {{m->fileName, m->size, m->sha256}};
    model.tags = {"gguf", "conversational"};
    model.pipelineTag = "text-generation";
    const lib::BenchmarkFamily* family = lib::familyForRepo(model.id, lib::builtinBenchmarks());
    model.paramsB = family ? family->paramsB : lib::paramsFromName(model.id);
    m_detailModel.reset(); // curated downloads go through downloadCurated()
    m_detail = detailFor(model, family, QStringLiteral("curated"));
    m_detail.insert(QStringLiteral("id"), id);
    m_detail.insert(QStringLiteral("note"), qs(m->description));
    m_detail.insert(QStringLiteral("card"), qs(m->licenseCard));
    m_detail.insert(QStringLiteral("installed"), ModelStore(ModelStore::defaultRoot()).isInstalled(*m));
    emit detailChanged();
}

void LocalModelsController::openInstalled(const QString& id) {
    lib::LibraryIndex index(ModelStore::defaultRoot());
    (void)index.load();
    const lib::InstalledModel* m = index.find(str(id));
    if (!m)
        return openCurated(id);
    lib::HubModel model;
    model.id = m->repo;
    model.author = m->publisher;
    model.revision = m->revision;
    model.license = m->license;
    model.licenseName = m->licenseName;
    model.paramsB = m->paramsB;
    model.contextLength = m->contextLength;
    model.tags = {"gguf", "conversational"};
    model.pipelineTag = m->vision ? "image-text-to-text" : "text-generation";
    for (const lib::InstalledFile& f : m->files) {
        const auto slash = f.storeName.find('/', std::string("library/").size() + m->repo.size());
        model.files.push_back({f.storeName.substr(slash + 1), f.size, f.sha256});
    }
    if (m->projector) {
        const auto slash = m->projector->storeName.find('/', std::string("library/").size() + m->repo.size());
        model.files.push_back({m->projector->storeName.substr(slash + 1), m->projector->size, m->projector->sha256});
    }
    m_detailModel.reset();
    m_detail = detailFor(model, m->family.empty() ? nullptr : lib::builtinBenchmarks().family(m->family), QStringLiteral("installed"));
    m_detail.insert(QStringLiteral("id"), id);
    m_detail.insert(QStringLiteral("name"), qs(m->name));
    m_detail.insert(QStringLiteral("installed"), true);
    m_detail.insert(QStringLiteral("licenseNeedsAcceptance"), false);
    emit detailChanged();
}

void LocalModelsController::closeModel() {
    ++*m_detailGeneration;
    m_detailModel.reset();
    m_detail.clear();
    emit detailChanged();
}

// ---- Downloads ------------------------------------------------------------------------------------------------------------

void LocalModelsController::addRuntime(std::vector<ModelInfo>& files, std::uint64_t weightsBytes, double paramsB) const {
    const llm::SystemProbe p = probe();
    if (p.os != "windows" || p.arch != "x64")
        return; // llama-server is managed on Windows x64 only for now
    // The build the plan picks now, and the small CPU build too: when the GPU has less free memory at the next start,
    // the assistant falls back to the CPU without another download.
    const llm::Admission a = llm::estimateFit(weightsBytes, paramsB, p);
    const std::string cpu = "llama-server-win-cpu-x64";
    for (const std::string& runtime : {a.fits && !a.runtimeId.empty() ? a.runtimeId : cpu, cpu}) {
        const ModelInfo* r = findModel(runtime);
        if (r && std::none_of(files.begin(), files.end(), [&](const ModelInfo& f) { return f.id == r->id; }))
            files.push_back(*r);
    }
}

QString LocalModelsController::enqueue(const QString& id, const QString& name, std::vector<ModelInfo> files, bool use) {
    DownloadRequest request{str(id), str(name), ModelStore::defaultRoot(), std::move(files)};
    if (!queue().enqueue(std::move(request)))
        return tr("%1 is already downloading.").arg(name);
    auto pending = std::make_unique<Pending>();
    pending->use = use;
    m_pending[str(id)] = std::move(pending);
    updateDownloads();
    return {};
}

QString LocalModelsController::download(const QString& repo, const QString& quant, bool acceptLicense, bool withVision, bool use) {
    if (offline())
        return tr("Offline mode or the project's no-cloud lock is on: downloads are blocked.");
    if (!m_detailModel || m_detailModel->id != str(repo))
        return tr("Open the model first.");
    const auto variants = lib::variantsOf(m_detailModel->files);
    const lib::Variant* variant = nullptr;
    for (const lib::Variant& v : variants) {
        if (qs(v.quant) == quant)
            variant = &v;
    }
    if (!variant)
        return tr("Choose a quantization.");
    const auto projectors = lib::projectorsOf(m_detailModel->files);
    const lib::HubFile* projector = withVision ? lib::defaultProjector(projectors) : nullptr;
    auto plan = lib::planInstall(*m_detailModel, *variant, projector, acceptLicense, lib::builtinBenchmarks());
    if (!plan) {
        if (!lib::isPermissive(m_detailModel->license) && !acceptLicense)
            return tr("Accept the model's licence (%1) first.").arg(qs(m_detailModel->licenseName));
        return qs(plan.error().message);
    }
    if (m_detailModel->gated && !m_hasToken)
        return tr("This model is gated. Accept its terms on its Hugging Face page, then add your Hugging Face access token in "
                  "Settings > AI > Providers.");
    std::vector<ModelInfo> files = lib::modelInfosOf(*plan);
    addRuntime(files, lib::bytesOf(*plan), plan->paramsB);
    const std::string id = plan->id;
    const QString error = enqueue(qs(id), qs(plan->name) + QLatin1Char(' ') + qs(plan->quant), std::move(files), use);
    if (error.isEmpty()) {
        Pending& pending = *m_pending[id];
        pending.kind = QStringLiteral("library");
        pending.install = std::move(*plan);
    }
    return error;
}

QString LocalModelsController::downloadCurated(const QString& id, bool use) {
    const ModelInfo* m = builtin(str(id), ModelKind::Llm);
    if (!m)
        return tr("Unknown model.");
    if (offline())
        return tr("Offline mode or the project's no-cloud lock is on: downloads are blocked.");
    std::vector<ModelInfo> files{*m};
    const lib::BenchmarkFamily* family = lib::familyForRepo(repoOfUrl(m->url), lib::builtinBenchmarks());
    addRuntime(files, m->size, family ? family->paramsB : 0.0);
    const QString error = enqueue(id, family ? qs(family->name) : qs(m->description), std::move(files), use);
    if (error.isEmpty())
        m_pending[str(id)]->kind = QStringLiteral("curated");
    return error;
}

QString LocalModelsController::downloadWhisper(const QString& id, bool use) {
    const ModelInfo* m = builtin(str(id), ModelKind::Whisper);
    if (!m)
        return tr("Unknown transcription model.");
    if (offline())
        return tr("Offline mode or the project's no-cloud lock is on: downloads are blocked.");
    std::vector<ModelInfo> files{*m};
    if (const ModelInfo* vad = findModel("silero"))
        files.push_back(*vad); // local transcription skips silence with it
    const QString name = qs(m->description.substr(0, m->description.find(':')));
    const QString error = enqueue(id, name, std::move(files), use);
    if (error.isEmpty())
        m_pending[str(id)]->kind = QStringLiteral("whisper");
    return error;
}

bool LocalModelsController::pause(const QString& id) { return m_queue && m_queue->pause(str(id)); }
bool LocalModelsController::resume(const QString& id) {
    if (offline())
        return false;
    readHubToken();
    return m_queue && m_queue->resume(str(id));
}
bool LocalModelsController::cancel(const QString& id) {
    if (!m_queue || !m_queue->cancel(str(id)))
        return false;
    m_pending.erase(str(id));
    updateDownloads();
    return true;
}
void LocalModelsController::clearFinished() {
    if (m_queue)
        m_queue->clearFinished();
    updateDownloads();
}

bool LocalModelsController::downloading() const { return m_queue && m_queue->busy(); }

QString LocalModelsController::humanError(const QString& raw) const {
    if (raw.contains(QLatin1String("ffline")) || raw.contains(QLatin1String("no-cloud")))
        return tr("Offline mode or the project's no-cloud lock blocked the download. Resume it when you are online.");
    if (raw.contains(QLatin1String("HTTP 401")) || raw.contains(QLatin1String("HTTP 403")))
        return tr("Hugging Face refused the download. The model may be gated: accept its terms on its page and add your "
                  "Hugging Face access token in Settings > AI > Providers.");
    if (raw.contains(QLatin1String("HTTP 404")))
        return tr("The file is no longer on Hugging Face at this revision.");
    if (raw.contains(QLatin1String("verification")))
        return tr("The download did not match its SHA-256 checksum, so it was deleted. Try again.");
    if (raw.startsWith(QLatin1String("Not enough free space")))
        return tr("Not enough free space on the drive. %1").arg(raw.section(QLatin1Char(':'), 1).trimmed());
    if (raw.contains(QLatin1String("cannot write")))
        return tr("Could not write the model file. Is the drive full or read-only?");
    return tr("The download stopped: %1. Resume it to continue where it stopped.").arg(raw);
}

void LocalModelsController::updateDownloads() {
    QVariantList rows;
    if (m_queue) {
        for (const DownloadState& s : m_queue->snapshot()) {
            const double progress = s.total > 0 ? static_cast<double>(s.received) / static_cast<double>(s.total) : 0.0;
            QString eta;
            if (s.eta) {
                const auto secs = s.eta->count();
                eta = secs >= 3600 ? tr("%1 h %2 min left").arg(secs / 3600).arg((secs % 3600) / 60)
                    : secs >= 60   ? tr("%1 min left").arg((secs + 30) / 60)
                                   : tr("%1 s left").arg(secs);
            }
            const bool active = s.status == DownloadStatus::Queued || s.status == DownloadStatus::Running;
            rows.push_back(QVariantMap{
                {QStringLiteral("id"), qs(s.id)},
                {QStringLiteral("name"), qs(s.name)},
                {QStringLiteral("status"), qs(toString(s.status))},
                {QStringLiteral("progress"), progress},
                {QStringLiteral("receivedText"), tr("%1 of %2").arg(sizeText(s.received).isEmpty() ? QStringLiteral("0 MB") : sizeText(s.received),
                                                                    sizeText(s.total))},
                {QStringLiteral("speedText"), s.bytesPerSecond > 0.0 ? tr("%1/s").arg(sizeText(static_cast<std::uint64_t>(s.bytesPerSecond))) : QString()},
                {QStringLiteral("etaText"), eta},
                {QStringLiteral("error"), s.status == DownloadStatus::Failed ? humanError(qs(s.error)) : QString()},
                {QStringLiteral("canPause"), active},
                {QStringLiteral("canResume"), s.status == DownloadStatus::Paused || s.status == DownloadStatus::Failed},
                {QStringLiteral("fileText"), s.fileCount > 1 && s.fileIndex > 0 ? tr("file %1 of %2").arg(s.fileIndex).arg(s.fileCount) : QString()},
            });
        }
    }
    if (rows != m_downloads) {
        m_downloads = rows;
        emit downloadsChanged();
    }
}

void LocalModelsController::finished(const DownloadState& state) {
    updateDownloads();
    if (state.status != DownloadStatus::Done) {
        // Kept: "Try again" resumes the same request, and what it installs is still needed then (cancel drops it).
        emit downloadFinished(qs(state.id), false, humanError(qs(state.error)));
        return;
    }
    std::unique_ptr<Pending> pending;
    if (auto it = m_pending.find(state.id); it != m_pending.end()) {
        pending = std::move(it->second);
        m_pending.erase(it);
    }
    const QString id = qs(state.id);
    if (pending && pending->install) {
        lib::LibraryIndex index(ModelStore::defaultRoot());
        (void)index.load(); // a damaged index is replaced by one that holds what is known to be there
        index.upsert(*pending->install);
        if (auto saved = index.save(); !saved)
            emit notice(tr("The model was downloaded, but the library index could not be saved: %1").arg(qs(saved.error().message)));
        for (ModelInfo& info : lib::modelInfosOf(*pending->install))
            (void)registerModel(std::move(info));
        if (m_ai)
            m_ai->registerLocalModel(lib::registrySpecOf(*pending->install));
    }
    emit downloadFinished(id, true, tr("%1 is ready.").arg(qs(state.name)));
    rebuildLibrary();
    rebuildResults();
    if (!m_detail.isEmpty()) {
        if (m_detailModel)
            m_detail = detailFor(*m_detailModel, nullptr, QStringLiteral("hub"));
        else if (m_detail.value(QStringLiteral("id")).toString() == id)
            m_detail.insert(QStringLiteral("installed"), true);
        emit detailChanged();
    }
    if (pending && pending->use) {
        const QString message = pending->kind == QLatin1String("whisper") ? useForTranscription(id) : useForAssistant(id);
        if (!message.isEmpty())
            emit notice(message);
    }
}

QString LocalModelsController::remove(const QString& id) {
    const std::string key = str(id);
    if (m_queue) {
        if (const auto s = m_queue->state(key); s && (s->status == DownloadStatus::Queued || s->status == DownloadStatus::Running ||
                                                      s->status == DownloadStatus::Paused))
            return tr("Cancel its download first.");
    }
    const std::filesystem::path root = ModelStore::defaultRoot();
    ModelStore store(root);
    lib::LibraryIndex index(root);
    (void)index.load();
    if (const lib::InstalledModel* m = index.find(key)) {
        const auto infos = lib::modelInfosOf(*m);
        const auto exclusive = index.exclusiveFiles(key);
        if (m_ai && m_ai->chatModel() == id)
            m_ai->setChatModel(QString());
        if (m_ai)
            m_ai->unregisterLocalModel(id);
        for (const std::string& name : exclusive) {
            ModelInfo file;
            file.fileName = name;
            if (auto removed = store.remove(file); !removed)
                return tr("Could not delete %1: %2").arg(qs(name), qs(removed.error().message));
        }
        // Empty folders of the model go too (never the store itself).
        std::error_code ec;
        for (const std::string& name : exclusive) {
            std::filesystem::path dir = store.pathFor(ModelInfo{.fileName = name}).parent_path();
            while (dir != root && dir.native().size() > root.native().size() && std::filesystem::is_empty(dir, ec) && !ec) {
                std::filesystem::remove(dir, ec);
                dir = dir.parent_path();
            }
        }
        for (const ModelInfo& info : infos)
            unregisterModel(info.id);
        llm::setLaunchOverrides(key, {});
        index.remove(key);
        if (auto saved = index.save(); !saved)
            return tr("The files were deleted, but the library index could not be saved: %1").arg(qs(saved.error().message));
    } else if (const ModelInfo* single = findModel(key); single && (single->kind == ModelKind::Llm || single->kind == ModelKind::Whisper)) {
        if (single->kind == ModelKind::Llm && m_ai && m_ai->chatModel() == id)
            m_ai->stopLocal();
        if (auto removed = store.remove(*single); !removed)
            return tr("Could not delete the model: %1").arg(qs(removed.error().message));
        if (single->kind == ModelKind::Whisper && whisperModel() == id)
            setWhisperModel(QString());
    } else {
        return tr("Unknown model.");
    }
    rebuildLibrary();
    rebuildResults();
    return {};
}

QString LocalModelsController::useForAssistant(const QString& id) {
    const ModelInfo* m = findModel(str(id));
    ModelStore store(ModelStore::defaultRoot());
    if (!m || m->kind != ModelKind::Llm)
        return tr("Unknown model.");
    if (!store.isInstalled(*m))
        return tr("Download the model first.");
    if (!m_ai)
        return tr("The assistant is not available.");
    // QA-03: the choice is made now, whether or not the engine can start yet (offline before llama-server is there):
    // the assistant starts it on first use.
    m_ai->setChatModel(id);
    m_ai->startLocal(id);
    emit notice(tr("Starting the local assistant with %1.").arg(qs(m->description)));
    return {};
}

QString LocalModelsController::useForTranscription(const QString& id) {
    if (id.isEmpty()) {
        setWhisperModel(QString());
        return {};
    }
    const ModelInfo* m = builtin(str(id), ModelKind::Whisper);
    if (!m)
        return tr("Unknown transcription model.");
    if (!ModelStore(ModelStore::defaultRoot()).isInstalled(*m))
        return downloadWhisper(id, true);
    setWhisperModel(id);
    emit notice(tr("Local transcription now uses %1.").arg(qs(m->description.substr(0, m->description.find(':')))));
    return {};
}

QString LocalModelsController::whisperModel() const { return QSettings().value(QLatin1String(kWhisperKey)).toString(); }

void LocalModelsController::setWhisperModel(const QString& id) {
    if (!id.isEmpty() && !builtin(str(id), ModelKind::Whisper))
        return;
    QSettings settings;
    if (id.isEmpty())
        settings.remove(QLatin1String(kWhisperKey));
    else
        settings.setValue(QLatin1String(kWhisperKey), id);
    rebuildLibrary();
}

QString LocalModelsController::whisperModelName() const {
    const QString id = transcriptionModelId();
    const ModelInfo* m = findModel(str(id));
    const QString name = m ? qs(m->description.substr(0, m->description.find(':'))) : id;
    return whisperModel().isEmpty() ? tr("Automatic: %1").arg(name) : name;
}

QVariantMap LocalModelsController::launchOptions(const QString& id) const {
    lib::LibraryIndex index(ModelStore::defaultRoot());
    (void)index.load();
    llm::LaunchOverrides o;
    if (const lib::InstalledModel* m = index.find(str(id)))
        o = {m->context, m->gpuLayers, m->threads};
    else
        o = storedLaunch(id);
    return {{QStringLiteral("context"), o.context}, {QStringLiteral("gpuLayers"), o.gpuLayers}, {QStringLiteral("threads"), o.threads}};
}

bool LocalModelsController::setLaunchOptions(const QString& id, int context, int gpuLayers, int threads) {
    llm::setLaunchOverrides(str(id), {context, gpuLayers, threads});
    const llm::LaunchOverrides o = llm::launchOverrides(str(id)); // clamped
    lib::LibraryIndex index(ModelStore::defaultRoot());
    (void)index.load();
    if (lib::InstalledModel* m = index.find(str(id))) {
        m->context = o.context;
        m->gpuLayers = o.gpuLayers;
        m->threads = o.threads;
        if (!index.save())
            return false;
        if (m_ai)
            m_ai->registerLocalModel(lib::registrySpecOf(*m)); // its context is the registry's context window
    } else if (builtin(str(id), ModelKind::Llm)) {
        QSettings settings;
        const QString key = QLatin1String(kLaunchGroup) + QLatin1Char('/') + id;
        if (o == llm::LaunchOverrides{})
            settings.remove(key);
        else
            settings.setValue(key, QStringLiteral("%1,%2,%3").arg(o.context).arg(o.gpuLayers).arg(o.threads));
    } else {
        return false;
    }
    rebuildLibrary();
    return true;
}

QString LocalModelsController::storageFolder() const { return QDir::toNativeSeparators(toQt(ModelStore::defaultRoot())); }

bool LocalModelsController::customStorage() const { return !QSettings().value(QLatin1String(kFolderKey)).toString().isEmpty(); }

QString LocalModelsController::storageFolderUrl() const { return QUrl::fromLocalFile(toQt(ModelStore::defaultRoot())).toString(); }

bool LocalModelsController::setStorageFolder(const QString& folder) {
    QString path = folder.trimmed();
    if (path.startsWith(QLatin1String("file:")))
        path = QUrl(path).toLocalFile();
    QSettings settings;
    if (path.isEmpty()) {
        settings.remove(QLatin1String(kFolderKey));
        ModelStore::setDefaultRoot({});
    } else {
        if (!QDir().mkpath(path))
            return false;
        QFile probeFile(QDir(path).filePath(QStringLiteral(".wabcut-write-test")));
        if (!probeFile.open(QIODevice::WriteOnly))
            return false;
        probeFile.close();
        probeFile.remove();
        settings.setValue(QLatin1String(kFolderKey), QDir::cleanPath(path));
        ModelStore::setDefaultRoot(fromQt(QDir::cleanPath(path)));
    }
    if (m_ai)
        loadLocalModelLibrary(*m_ai); // what the new folder already holds
    rebuildLibrary();
    rebuildResults();
    return true;
}

// ---- The library lists -------------------------------------------------------------------------------------------------

void LocalModelsController::refresh() {
    m_probe.reset();
    readHubToken();
    rebuildLibrary();
    rebuildResults();
    updateDownloads();
}

void LocalModelsController::rebuildLibrary() {
    const llm::SystemProbe p = probe();
    const std::filesystem::path root = ModelStore::defaultRoot();
    ModelStore store(root);
    const lib::BenchmarkTable& table = lib::builtinBenchmarks();
    const QString chat = m_ai ? m_ai->chatModel() : QString();
    std::uint64_t used = 0;

    // Recommended: the curated assistant models, the one this computer runs best first.
    const llm::Recommendation recommendation = llm::recommend(p);
    const ModelRegistry& registry = m_ai ? m_ai->platform().providers().models() : ModelRegistry::builtin();
    std::vector<std::pair<int, QVariantMap>> curated;
    int order = 0;
    for (const ModelInfo& m : builtinModels()) {
        if (m.kind != ModelKind::Llm)
            continue;
        const std::string repo = repoOfUrl(m.url);
        const lib::BenchmarkFamily* family = lib::familyForRepo(repo, table);
        const llm::TierSpec* tier = llm::tierForModel(m.id);
        llm::Admission a;
        if (tier) {
            a = llm::admit(*tier, m, p);
        } else {
            a = llm::estimateFit(m.size, family ? family->paramsB : 0.0, p);
        }
        const bool recommended = tier && recommendation.tier && *recommendation.tier == tier->id;
        const bool installed = store.isInstalled(m);
        used += installed ? m.size : 0;
        const ModelSpec* spec = registry.model(m.id);
        const auto score = family ? lib::compositeScore(*family, table) : std::nullopt;
        QStringList tasks{QStringLiteral("chat")};
        QStringList taskNames{taskName(lib::Task::Chat)};
        if (family) {
            for (const std::string& t : family->tasks) {
                if (const auto task = lib::taskFromString(t); task && *task != lib::Task::Chat) {
                    tasks << qs(t);
                    taskNames << taskName(*task);
                }
            }
        }
        QVariantMap row{
            {QStringLiteral("id"), qs(m.id)},
            {QStringLiteral("kind"), QStringLiteral("curated")},
            {QStringLiteral("repo"), qs(repo)},
            {QStringLiteral("name"), family ? qs(family->name) : qs(m.description.substr(0, m.description.find(',')))},
            {QStringLiteral("note"), qs(m.description)},
            {QStringLiteral("tierName"), tier ? qs(tier->name) : QString()},
            {QStringLiteral("publisher"), qs(repo.substr(0, repo.find('/')))},
            {QStringLiteral("publisherKind"),
             family && QString::compare(qs(repo.substr(0, repo.find('/'))), qs(family->id.substr(0, family->id.find('/'))), Qt::CaseInsensitive) == 0
                 ? QStringLiteral("official")
                 : lib::isKnownQuantizer(repo.substr(0, repo.find('/'))) ? QStringLiteral("quantizer") : QStringLiteral("community")},
            {QStringLiteral("params"), paramsText(family ? family->paramsB : 0.0)},
            {QStringLiteral("size"), sizeText(m.size)},
            {QStringLiteral("sizeBytes"), static_cast<double>(m.size)},
            {QStringLiteral("quant"), qs(lib::quantOf(m.fileName))},
            {QStringLiteral("license"), qs(m.license)},
            {QStringLiteral("licenseName"), qs(lib::licenseDisplayName(m.license))},
            {QStringLiteral("permissive"), true},
            {QStringLiteral("tasks"), tasks},
            {QStringLiteral("taskNames"), taskNames.join(QStringLiteral(" · "))},
            {QStringLiteral("score"), score ? *score : -1.0},
            {QStringLiteral("scoreText"), score ? QString::number(std::lround(*score)) : QString()},
            {QStringLiteral("scoreFrom"), family ? qs(family->name) : QString()},
            {QStringLiteral("fit"), fitKey(!a.fits ? lib::Fit::No : a.gpu ? lib::Fit::Gpu : lib::Fit::Cpu)},
            {QStringLiteral("fitText"), qs(a.reason)},
            {QStringLiteral("ram"), sizeText(a.ramNeeded)},
            {QStringLiteral("vram"), a.gpu ? sizeText(a.vramNeeded) : QString()},
            {QStringLiteral("installed"), installed},
            {QStringLiteral("recommended"), recommended},
            {QStringLiteral("curated"), true},
            {QStringLiteral("inUse"), chat == qs(m.id)},
            {QStringLiteral("toolCalls"), spec && spec->caps.toolsOnChat},
            {QStringLiteral("downloads"), 0.0},
            {QStringLiteral("gated"), false},
        };
        const int rank = (recommended ? 0 : 1000) + (a.fits ? (a.gpu ? 0 : 100) : 500) + order++;
        curated.emplace_back(rank, row);
    }
    std::stable_sort(curated.begin(), curated.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    m_recommended.clear();
    for (auto& [rank, row] : curated)
        m_recommended.push_back(row);

    // Transcription: the whisper models.
    const QString automatic = automaticWhisperModel();
    const QString effective = transcriptionModelId();
    m_whisper.clear();
    for (const ModelInfo& m : builtinModels()) {
        if (m.kind != ModelKind::Whisper)
            continue;
        const bool installed = store.isInstalled(m);
        used += installed ? m.size : 0;
        const auto colon = m.description.find(':');
        m_whisper.push_back(QVariantMap{
            {QStringLiteral("id"), qs(m.id)},
            {QStringLiteral("name"), qs(m.description.substr(0, colon))},
            {QStringLiteral("note"), colon == std::string::npos ? QString() : qs(m.description.substr(colon + 1)).trimmed()},
            {QStringLiteral("size"), sizeText(m.size)},
            {QStringLiteral("sizeBytes"), static_cast<double>(m.size)},
            {QStringLiteral("installed"), installed},
            {QStringLiteral("selected"), whisperModel() == qs(m.id)},
            {QStringLiteral("active"), effective == qs(m.id)},
            {QStringLiteral("recommended"), automatic == qs(m.id)},
            {QStringLiteral("englishOnly"), m.id.find(".en") != std::string::npos},
        });
    }

    // Installed: the library's models, then the curated ones and whisper models on disk.
    m_installed.clear();
    lib::LibraryIndex index(root);
    const bool indexOk = index.load().has_value();
    for (const lib::InstalledModel& m : index.models()) {
        const auto infos = lib::modelInfosOf(m);
        bool present = !infos.empty();
        std::uint64_t bytes = 0;
        for (const ModelInfo& info : infos) {
            present = present && store.isInstalled(info);
            bytes += installedBytes(store, info);
        }
        used += bytes;
        m_installed.push_back(QVariantMap{
            {QStringLiteral("id"), qs(m.id)},
            {QStringLiteral("name"), qs(m.name) + QLatin1Char(' ') + qs(m.quant)},
            {QStringLiteral("detail"), qs(m.repo)},
            {QStringLiteral("kind"), QStringLiteral("assistant")},
            {QStringLiteral("quant"), qs(m.quant)},
            {QStringLiteral("size"), sizeText(lib::bytesOf(m))},
            {QStringLiteral("sizeBytes"), static_cast<double>(lib::bytesOf(m))},
            {QStringLiteral("license"), qs(m.licenseName)},
            {QStringLiteral("inUse"), chat == qs(m.id)},
            {QStringLiteral("library"), true},
            {QStringLiteral("vision"), m.vision},
            {QStringLiteral("missing"), !present},
            {QStringLiteral("context"), m.context},
            {QStringLiteral("gpuLayers"), m.gpuLayers},
            {QStringLiteral("threads"), m.threads},
        });
    }
    for (const QVariant& v : m_recommended) {
        const QVariantMap r = v.toMap();
        if (!r.value(QStringLiteral("installed")).toBool())
            continue;
        const llm::LaunchOverrides o = storedLaunch(r.value(QStringLiteral("id")).toString());
        m_installed.push_back(QVariantMap{
            {QStringLiteral("id"), r.value(QStringLiteral("id"))},
            {QStringLiteral("name"), r.value(QStringLiteral("name"))},
            {QStringLiteral("detail"), r.value(QStringLiteral("repo"))},
            {QStringLiteral("kind"), QStringLiteral("assistant")},
            {QStringLiteral("quant"), r.value(QStringLiteral("quant"))},
            {QStringLiteral("size"), r.value(QStringLiteral("size"))},
            {QStringLiteral("sizeBytes"), r.value(QStringLiteral("sizeBytes"))},
            {QStringLiteral("license"), r.value(QStringLiteral("licenseName"))},
            {QStringLiteral("inUse"), r.value(QStringLiteral("inUse"))},
            {QStringLiteral("library"), false},
            {QStringLiteral("vision"), false},
            {QStringLiteral("missing"), false},
            {QStringLiteral("context"), o.context},
            {QStringLiteral("gpuLayers"), o.gpuLayers},
            {QStringLiteral("threads"), o.threads},
        });
    }
    for (const QVariant& v : m_whisper) {
        const QVariantMap r = v.toMap();
        if (!r.value(QStringLiteral("installed")).toBool())
            continue;
        m_installed.push_back(QVariantMap{
            {QStringLiteral("id"), r.value(QStringLiteral("id"))},
            {QStringLiteral("name"), r.value(QStringLiteral("name"))},
            {QStringLiteral("detail"), QStringLiteral("ggerganov/whisper.cpp")},
            {QStringLiteral("kind"), QStringLiteral("transcription")},
            {QStringLiteral("quant"), QString()},
            {QStringLiteral("size"), r.value(QStringLiteral("size"))},
            {QStringLiteral("sizeBytes"), r.value(QStringLiteral("sizeBytes"))},
            {QStringLiteral("license"), QStringLiteral("MIT License")},
            {QStringLiteral("inUse"), r.value(QStringLiteral("active"))},
            {QStringLiteral("library"), false},
            {QStringLiteral("vision"), false},
            {QStringLiteral("missing"), false},
            {QStringLiteral("context"), 0},
            {QStringLiteral("gpuLayers"), -1},
            {QStringLiteral("threads"), 0},
        });
    }

    const auto free = m_testFreeSpace ? m_testFreeSpace : freeBytes(root);
    m_storageText = tr("%1 used by these models").arg(used > 0 ? sizeText(used) : tr("Nothing"));
    if (free)
        m_storageText += QStringLiteral(" · ") + tr("%1 free").arg(sizeText(*free));
    if (!indexOk)
        m_storageText += QStringLiteral(" · ") + tr("the library index in this folder is damaged");

    QStringList machine;
    if (const llm::GpuInfo* gpu = p.bestGpu(); gpu && !gpu->integrated)
        machine << (gpu->freeBytes > 0 ? tr("%1, %2 free").arg(qs(gpu->name), sizeText(gpu->freeBytes)) : qs(gpu->name));
    else
        machine << tr("No dedicated GPU");
    if (p.ramAvailable > 0)
        machine << tr("%1 RAM free").arg(sizeText(p.ramAvailable));
    if (p.cpuThreads > 0)
        machine << tr("%n CPU thread(s)", nullptr, p.cpuThreads);
    m_machine = machine.join(QStringLiteral(" · "));
    emit libraryChanged();
}

} // namespace wabcut::app
