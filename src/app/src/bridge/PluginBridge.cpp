#include "PluginBridge.h"

#include "BridgeIds.h"
#include "ProjectController.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>

#if defined(WABCUT_WITH_AUDIO_PLUGINS)
#include <wabcut/plughost/PluginCatalog.h>
#include <wabcut/plughost/PluginEffects.h>
#include <wabcut/plughost/PluginHost.h>
#include <wabcut/plughost/PluginSystem.h>
#endif

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QQmlEngine>
#include <QSettings>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>

namespace wabcut::app {

namespace {

QString text(const std::string& s) { return QString::fromStdString(s); }
/// QSettings key of the remembered plug-in grouping (UX rework).
constexpr const char* kGroupByKey = "effects/pluginGroupBy";
[[maybe_unused]] QString pathText(const std::filesystem::path& p) { return QDir::toNativeSeparators(QString::fromStdWString(p.wstring())); }
[[maybe_unused]] std::filesystem::path toPath(const QString& s) { return std::filesystem::path(QDir::toNativeSeparators(s).toStdWString()); }
bool isPluginEffect(std::string_view effect) { return effect.starts_with(kPluginEffectPrefix); }
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
constexpr std::string_view kStatePrefix = plughost::kStateKeyPrefix;
#endif

} // namespace

struct PluginBridge::Found {
    edit::EffectTarget target;
    EffectInstance effect;
};

/// One thread for the editors' helper round trips (open, read a state), in order; each job's result is posted back to
/// the bridge while it exists (the gate: the destructor clears `owner` under the lock first).
struct PluginBridge::EditorWork {
    struct Gate {
        std::mutex mutex;
        PluginBridge* owner = nullptr;
    };
    std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    std::mutex mutex;
    std::condition_variable_any wake;
    std::deque<std::function<void()>> queue;
    std::jthread thread; ///< last: starts once the rest exists

    explicit EditorWork(PluginBridge* owner) {
        gate->owner = owner;
        thread = std::jthread([this](std::stop_token stop) { run(stop); });
    }
    void post(std::function<void()> job) {
        {
            const std::lock_guard lock(mutex);
            queue.push_back(std::move(job));
        }
        wake.notify_one();
    }
    void run(std::stop_token stop) {
        std::unique_lock lock(mutex);
        while (wake.wait(lock, stop, [&] { return !queue.empty(); }) && !stop.stop_requested()) {
            std::function<void()> job = std::move(queue.front());
            queue.pop_front();
            lock.unlock();
            job();
            lock.lock();
        }
    }
};

PluginBridge::PluginBridge(QObject* parent) : QObject(parent) {
    // UX rework: the Effects panel's plug-in grouping is remembered.
    if (QSettings().value(QLatin1String(kGroupByKey)).toString() == QLatin1String("category"))
        m_groupBy = QStringLiteral("category");
    m_editorTimer.setInterval(500);
    connect(&m_editorTimer, &QTimer::timeout, this, &PluginBridge::pollEditors);
    m_statusTimer.setInterval(1000);
    connect(&m_statusTimer, &QTimer::timeout, this, &PluginBridge::pollStatus);
}

PluginBridge::~PluginBridge() {
    // The scan worker posts to this object: stop it before the members go.
    m_alive->store(false);
    if (m_scanThread.joinable()) {
        m_scanThread.request_stop();
        m_scanThread.join();
    }
    if (m_editorWork) {
        {
            const std::lock_guard lock(m_editorWork->gate->mutex);
            m_editorWork->gate->owner = nullptr; // no result is posted to this object any more
        }
        m_editorWork.reset(); // waits for a round trip under way (bounded by the helper's timeout)
    }
}

void PluginBridge::editorJob(std::function<std::function<void(PluginBridge&)>()> work) {
    if (m_synchronous) {
        work()(*this);
        return;
    }
    if (!m_editorWork) {
        m_editorWork = std::make_unique<EditorWork>(this);
    }
    m_editorWork->post([work = std::move(work), gate = m_editorWork->gate] {
        std::function<void(PluginBridge&)> done;
        try {
            done = work();
        } catch (const std::exception& e) {
            const QString why = QString::fromUtf8(e.what());
            done = [why](PluginBridge& self) { self.setMessage(why); };
        } catch (...) {
            done = [](PluginBridge& self) { self.setMessage(tr("The plugin host failed")); };
        }
        const std::lock_guard lock(gate->mutex);
        if (PluginBridge* owner = gate->owner) {
            QMetaObject::invokeMethod(owner, [owner, done = std::move(done)] { done(*owner); }, Qt::QueuedConnection);
        }
    });
}

PluginBridge* PluginBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new PluginBridge;
    if (qmlEngine)
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
    if (plughost::PluginSystem* system = plughost::PluginSystem::current())
        bridge->setPlugins(system->catalog(), system->effects());
#endif
    return bridge;
}

void PluginBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::revisionChanged, this, &PluginBridge::bumpRevision);
        connect(controller, &ProjectController::sequenceChanged, this, &PluginBridge::bumpRevision);
    }
    bumpRevision();
}

void PluginBridge::setPlugins(std::shared_ptr<plughost::PluginCatalog> catalog, std::shared_ptr<plughost::PluginEffects> effects) {
    const bool was = available();
    m_catalog = std::move(catalog);
    m_effects = std::move(effects);
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
    if (m_effects && !m_opener) {
        std::weak_ptr<plughost::PluginEffects> fx = m_effects;
        m_opener = [fx](const EffectInstance& e) -> std::expected<void, Error> {
            const auto effects = fx.lock();
            return effects ? effects->openEditor(e) : std::unexpected(Error(ErrorCode::NotFound, "no plugin support"));
        };
        m_capturer = [fx](const EffectInstance& e) -> std::expected<std::string, Error> {
            const auto effects = fx.lock();
            return effects ? effects->captureState(e) : std::unexpected(Error(ErrorCode::NotFound, "no plugin support"));
        };
        m_counter = [fx](const EffectInstance& e) -> std::optional<std::uint64_t> {
            const auto effects = fx.lock();
            const auto instance = effects ? effects->instanceFor(e.id) : nullptr;
            return instance ? std::optional<std::uint64_t>(instance->stateChanges()) : std::nullopt;
        };
    }
#endif
    if (available())
        m_statusTimer.start();
    else
        m_statusTimer.stop();
    m_wasScanning = scanning();
    refresh();
    if (was != available())
        emit availableChanged();
    emit scanChanged();
    bumpRevision();
}

void PluginBridge::setEditorHooks(EditorOpener opener, StateCapturer capturer, ChangeCounter counter) {
    m_opener = std::move(opener);
    m_capturer = std::move(capturer);
    m_counter = std::move(counter);
}

bool PluginBridge::available() const {
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
    return m_catalog && m_effects;
#else
    return false;
#endif
}

bool PluginBridge::scanning() const {
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
    return m_ownScan || (m_catalog && m_catalog->scanning());
#else
    return false;
#endif
}

void PluginBridge::setMessage(const QString& message) {
    m_message = message;
    emit messageChanged();
}

void PluginBridge::bumpRevision() {
    ++m_revision;
    m_statusSignature = statusSignature();
    // Editors of effects that are gone stop being polled.
    for (auto it = m_editing.begin(); it != m_editing.end();) {
        if (!findEffect(EffectInstanceId{it->first}))
            it = m_editing.erase(it);
        else
            ++it;
    }
    if (m_editing.empty())
        m_editorTimer.stop();
    emit revisionChanged();
}

// ---- The catalog ---------------------------------------------------------------------------------------------------

void PluginBridge::setQuery(const QString& query) {
    if (query == m_query)
        return;
    m_query = query;
    refresh();
}

void PluginBridge::setGroupBy(const QString& groupBy) {
    const QString next = groupBy == QLatin1String("category") ? groupBy : QStringLiteral("vendor");
    if (next == m_groupBy)
        return;
    m_groupBy = next;
    if (m_persistent)
        QSettings().setValue(QLatin1String(kGroupByKey), m_groupBy);
    emit pluginsChanged();
}

QString PluginBridge::groupKey(const QVariantMap& plugin, const QString& groupBy) {
    const bool byCategory = groupBy == QLatin1String("category");
    return (byCategory ? QStringLiteral("category:") : QStringLiteral("vendor:"))
        + plugin.value(byCategory ? QStringLiteral("category") : QStringLiteral("vendor")).toString();
}

void PluginBridge::refresh() {
    m_all.clear();
    m_blocked.clear();
    m_folders.clear();
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
    if (m_catalog) {
        std::vector<plughost::PluginInfo> list = m_catalog->plugins();
        std::sort(list.begin(), list.end(), [](const plughost::PluginInfo& a, const plughost::PluginInfo& b) {
            return QString::compare(text(a.name), text(b.name), Qt::CaseInsensitive) < 0;
        });
        for (const plughost::PluginInfo& p : list) {
            if (!p.effect)
                continue; // instruments are listed by the scanner, not offered
            QString latency;
            if (p.latency > 0)
                latency = tr("%1 ms").arg(QLocale().toString(p.latency / 48.0, 'f', 1));
            QString category = text(p.category);
            category.replace(QLatin1Char('|'), QStringLiteral(" · "));
            m_all.append(QVariantMap{{QStringLiteral("id"), text(p.effectId())},
                                     {QStringLiteral("name"), text(p.name)},
                                     {QStringLiteral("vendor"), text(p.vendor)},
                                     {QStringLiteral("category"), category},
                                     {QStringLiteral("format"), p.format == "clap" ? QStringLiteral("CLAP") : QStringLiteral("VST3")},
                                     {QStringLiteral("version"), text(p.version)},
                                     {QStringLiteral("latencyText"), latency},
                                     {QStringLiteral("hasEditor"), p.editor},
                                     {QStringLiteral("problem"), text(p.problem)}});
        }
        for (const plughost::BlockedPlugin& b : m_catalog->blocked()) {
            m_blocked.append(QVariantMap{{QStringLiteral("file"), pathText(b.path)},
                                         {QStringLiteral("name"), QString::fromStdWString(b.path.filename().wstring())},
                                         {QStringLiteral("reason"), text(b.reason)}});
        }
        const auto& fixed = m_catalog->options().folders;
        for (const auto& f : fixed.empty() ? plughost::PluginCatalog::standardFolders() : fixed)
            m_folders.append(QVariantMap{{QStringLiteral("path"), pathText(f)}, {QStringLiteral("user"), false}});
        for (const auto& f : m_catalog->userFolders())
            m_folders.append(QVariantMap{{QStringLiteral("path"), pathText(f)}, {QStringLiteral("user"), true}});
    }
#endif
    m_pluginCount = static_cast<int>(m_all.size());
    m_plugins.clear();
    const QString q = m_query.trimmed();
    for (const QVariant& v : m_all) {
        const QVariantMap p = v.toMap();
        if (q.isEmpty() || p.value(QStringLiteral("name")).toString().contains(q, Qt::CaseInsensitive) ||
            p.value(QStringLiteral("vendor")).toString().contains(q, Qt::CaseInsensitive) ||
            p.value(QStringLiteral("category")).toString().contains(q, Qt::CaseInsensitive))
            m_plugins.append(p);
    }
    emit pluginsChanged();
}

QVariantList PluginBridge::groups() const {
    const bool byCategory = m_groupBy == QLatin1String("category");
    std::map<QString, QVariantList> grouped;
    std::map<QString, QString> keys;
    for (const QVariant& v : m_plugins) {
        const QVariantMap p = v.toMap();
        QString title = p.value(byCategory ? QStringLiteral("category") : QStringLiteral("vendor")).toString();
        if (title.isEmpty())
            title = byCategory ? tr("Other") : tr("Unknown vendor");
        grouped[title].append(p);
        keys.emplace(title, groupKey(p, m_groupBy));
    }
    std::vector<std::pair<QString, QVariantList>> ordered(grouped.begin(), grouped.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& a, const auto& b) { return QString::compare(a.first, b.first, Qt::CaseInsensitive) < 0; });
    QVariantList out;
    for (auto& [title, items] : ordered) {
        out.append(QVariantMap{{QStringLiteral("title"), title},
                               {QStringLiteral("key"), keys[title]},
                               {QStringLiteral("count"), static_cast<int>(items.size())},
                               {QStringLiteral("items"), items}});
    }
    return out;
}

QString PluginBridge::rescan(bool all) {
    if (!available()) {
        const QString why = tr("Audio plugins are not available here");
        setMessage(why);
        return why;
    }
    if (scanning())
        return tr("A scan is running");
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
    m_ownScan = true;
    m_scanText = tr("Scanning audio plugins");
    m_progress = -1;
    emit scanChanged();
    setMessage({});
    const auto catalog = m_catalog;
    if (m_synchronous) {
        const plughost::ScanReport report = catalog->scan({}, {}, all);
        QStringList errors;
        for (const std::string& e : report.errors)
            errors << text(e);
        finishScan(report.plugins, report.scanned, static_cast<int>(report.blocked.size()), errors, report.cancelled);
        return {};
    }
    if (m_scanThread.joinable())
        m_scanThread.join(); // the previous scan has finished (m_ownScan was false)
    const auto alive = m_alive;
    m_scanThread = std::jthread([this, catalog, alive, all](std::stop_token stop) {
        plughost::ScanReport report;
        try {
            report = catalog->scan(
            stop,
            [this, alive](double fraction, std::string_view file) {
                if (!alive->load())
                    return;
                const QString name = QFileInfo(QString::fromUtf8(file.data(), static_cast<qsizetype>(file.size()))).fileName();
                QMetaObject::invokeMethod(
                    this,
                    [this, fraction, name] {
                        m_progress = fraction;
                        m_scanText = name.isEmpty() ? tr("Scanning audio plugins") : tr("Scanning %1").arg(name);
                        emit scanChanged();
                    },
                    Qt::QueuedConnection);
            },
            all);
        } catch (const std::exception& e) { // a malformed scan result: the scan fails, the app goes on
            report = plughost::ScanReport{};
            report.errors.push_back(e.what());
        } catch (...) {
            report = plughost::ScanReport{};
            report.errors.push_back("the plugin scan failed");
        }
        if (!alive->load())
            return;
        QStringList errors;
        for (const std::string& e : report.errors)
            errors << text(e);
        const int plugins = report.plugins;
        const int scanned = report.scanned;
        const int blocked = static_cast<int>(report.blocked.size());
        const bool cancelled = report.cancelled;
        QMetaObject::invokeMethod(
            this, [this, plugins, scanned, blocked, errors, cancelled] { finishScan(plugins, scanned, blocked, errors, cancelled); },
            Qt::QueuedConnection);
    });
#else
    (void)all;
#endif
    return {};
}

void PluginBridge::finishScan(int plugins, int scanned, int blockedNow, const QStringList& errors, bool cancelled) {
    m_ownScan = false;
    m_scanText.clear();
    m_progress = -1;
    m_wasScanning = scanning();
    refresh();
    emit scanChanged();
    QString message = cancelled ? tr("Scan stopped") : tr("%n audio plugin(s)", nullptr, plugins);
    if (scanned > 0)
        message += tr(" · %n file(s) scanned", nullptr, scanned);
    if (blockedNow > 0)
        message += tr(" · %n blocked", nullptr, blockedNow);
    if (!errors.isEmpty())
        message += tr(" · %n file(s) not loaded", nullptr, static_cast<int>(errors.size()));
    setMessage(message);
    bumpRevision(); // missing plugins may be found now
}

QString PluginBridge::unblock(const QString& file) {
    if (!available())
        return tr("Audio plugins are not available here");
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
    m_catalog->unblock(toPath(file));
    refresh();
#else
    (void)file;
#endif
    return rescan();
}

QString PluginBridge::addFolder(const QUrl& folder) {
    if (!available())
        return tr("Audio plugins are not available here");
    const QString local = folder.isLocalFile() ? folder.toLocalFile() : folder.toString();
    if (local.isEmpty() || !QFileInfo(local).isDir())
        return tr("Choose a folder");
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
    m_catalog->addUserFolder(toPath(local));
    refresh();
#endif
    return rescan();
}

QString PluginBridge::removeFolder(const QString& path) {
    if (!available())
        return tr("Audio plugins are not available here");
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
    // The folder as the catalog keeps it (the list shows native separators; Windows paths ignore case).
    const QString wanted = QDir::cleanPath(path);
    for (const auto& folder : m_catalog->userFolders()) {
        if (QDir::cleanPath(pathText(folder)).compare(wanted, Qt::CaseInsensitive) == 0) {
            m_catalog->removeUserFolder(folder);
            refresh();
            return {};
        }
    }
    return tr("That folder is not one of yours");
#else
    (void)path;
    return {};
#endif
}

// ---- Plugin effects in the project ---------------------------------------------------------------------------------

std::optional<PluginBridge::Found> PluginBridge::findEffect(EffectInstanceId id) const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq || !id.isValid())
        return std::nullopt;
    const auto inTracks = [&](const std::vector<TrackPtr>& tracks) -> std::optional<Found> {
        for (const TrackPtr& t : tracks) {
            for (const Clip& c : t->clips) {
                for (const EffectInstance& e : c.effects()) {
                    if (e.id == id)
                        return Found{edit::EffectTarget::ofClip(c.id), e};
                }
            }
            for (const EffectInstance& e : t->effects) {
                if (e.id == id)
                    return Found{edit::EffectTarget::ofTrack(t->id), e};
            }
        }
        return std::nullopt;
    };
    if (auto found = inTracks(seq->audioTracks))
        return found;
    for (const EffectInstance& e : seq->audioEffects) {
        if (e.id == id)
            return Found{edit::EffectTarget::masterAudio(), e};
    }
    return inTracks(seq->videoTracks);
}

QString PluginBridge::apply(const QString& pluginId, const QString& targetKey) {
    if (!available() || !m_controller)
        return tr("Audio plugins are not available here");
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
    const auto plugin = m_catalog->find(pluginId.toStdString());
    if (!plugin)
        return tr("This plugin is not on this computer: rescan the plugin folders");
    if (!plugin->effect)
        return tr("%1 is an instrument, not an effect").arg(text(plugin->name));
    if (!plugin->problem.empty())
        return text(plugin->problem);
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq)
        return tr("No sequence");
    std::optional<edit::EffectTarget> target;
    if (targetKey == QLatin1String("master")) {
        target = edit::EffectTarget::masterAudio();
    } else if (const auto track = parseTrackKey(targetKey)) {
        const bool audio = std::any_of(seq->audioTracks.begin(), seq->audioTracks.end(), [&](const TrackPtr& t) { return t->id == *track; });
        if (!audio)
            return tr("%1 is an audio effect: drop it on an audio track").arg(text(plugin->name));
        target = edit::EffectTarget::ofTrack(*track);
    } else if (const auto clip = parseClipKey(targetKey)) {
        const auto loc = seq->findClip(*clip);
        if (!loc)
            return tr("Drop the plugin on a clip");
        ClipId audioClip;
        if (loc->clip().stream.kind == StreamKind::Audio) {
            audioClip = *clip;
        } else {
            const ClipId ids[] = {*clip};
            for (const ClipId partner : edit::withLinkedClips(*seq, ids)) {
                const auto p = seq->findClip(partner);
                if (p && p->clip().stream.kind == StreamKind::Audio) {
                    audioClip = partner;
                    break;
                }
            }
        }
        if (!audioClip.isValid())
            return tr("%1 is an audio effect: drop it on an audio clip").arg(text(plugin->name));
        target = edit::EffectTarget::ofClip(audioClip);
    } else {
        return tr("Drop the plugin on a clip, a track or the master");
    }
    const SequenceId sequence = seq->id;
    const std::string effect = plugin->effectId();
    // The name it was added with, for the "missing plugin" badge on a computer that does not have it.
    const std::string savedName = plugin->vendor.empty() ? plugin->name : plugin->name + " (" + plugin->vendor + ")";
    const bool ok = m_controller->applyEditFn(
        QT_TRANSLATE_NOOP("wabcut::app::PluginBridge", "Add Plugin"), [&](const ProjectState& state) -> edit::EditOutcome {
            auto added = edit::addEffect(state, sequence, *target, effect);
            if (!added || added->createdEffects.empty())
                return added;
            edit::EffectChange change;
            change.data.push_back(EffectData{std::string(plughost::kPluginNameKey), savedName});
            auto named = edit::setEffect(added->state, sequence, *target, added->createdEffects.front(), change);
            if (!named)
                return added;
            edit::EditResult result = std::move(*added);
            result.changes = diff(state, named->state);
            result.state = std::move(named->state);
            return result;
        });
    if (!ok)
        return m_controller->lastError();
    setMessage(tr("Added %1").arg(text(plugin->name)));
    return {};
#else
    (void)pluginId;
    (void)targetKey;
    return tr("Audio plugins are not available here");
#endif
}

QString PluginBridge::applyToSelection(const QString& pluginId) {
    if (!m_controller)
        return tr("No project");
    const auto target = m_controller->propertyTarget(StreamKind::Audio);
    if (!target)
        return tr("Select an audio clip first");
    return apply(pluginId, clipKey(*target));
}

QVariantMap PluginBridge::applyToSelectedClips(const QString& pluginId) {
    const auto failed = [](const QString& error) {
        return QVariantMap{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"), error},
                           {QStringLiteral("message"), error},
                           {QStringLiteral("clips"), 0}};
    };
    if (!available() || !m_controller)
        return failed(tr("Audio plugins are not available here"));
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
    const auto plugin = m_catalog->find(pluginId.toStdString());
    if (!plugin)
        return failed(tr("This plugin is not on this computer: rescan the plugin folders"));
    if (!plugin->effect)
        return failed(tr("%1 is an instrument, not an effect").arg(text(plugin->name)));
    if (!plugin->problem.empty())
        return failed(text(plugin->problem));
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq)
        return failed(tr("No sequence"));
    // Every selected audio clip; a selected video clip stands for its (first) linked audio clip. Once each.
    std::vector<ClipId> clips;
    const auto add = [&](ClipId id) {
        if (std::find(clips.begin(), clips.end(), id) == clips.end())
            clips.push_back(id);
    };
    bool anySelected = false;
    for (const ClipId id : m_controller->selection()) {
        const auto loc = seq->findClip(id);
        if (!loc)
            continue;
        anySelected = true;
        if (loc->clip().stream.kind == StreamKind::Audio) {
            add(id);
            continue;
        }
        const ClipId ids[] = {id};
        for (const ClipId partner : edit::withLinkedClips(*seq, ids)) {
            const auto p = seq->findClip(partner);
            if (p && p->clip().stream.kind == StreamKind::Audio) {
                add(partner);
                break;
            }
        }
    }
    if (!anySelected)
        return failed(tr("Select a clip first"));
    if (clips.empty())
        return failed(tr("%1 is an audio effect: select an audio clip").arg(text(plugin->name)));
    const SequenceId sequence = seq->id;
    const std::string effect = plugin->effectId();
    // The name it was added with, for the "missing plugin" badge on a computer that does not have it (as apply()).
    const std::string savedName = plugin->vendor.empty() ? plugin->name : plugin->name + " (" + plugin->vendor + ")";
    const bool ok = m_controller->applyEditFn(
        QT_TRANSLATE_NOOP("wabcut::app::PluginBridge", "Add Plugin"), [&](const ProjectState& state) -> edit::EditOutcome {
            auto added = edit::addEffect(state, sequence, std::span<const ClipId>(clips), effect);
            if (!added || added->createdEffects.size() != clips.size())
                return added;
            ProjectState current = added->state;
            for (std::size_t i = 0; i < clips.size(); ++i) {
                edit::EffectChange change;
                change.data.push_back(EffectData{std::string(plughost::kPluginNameKey), savedName});
                auto named = edit::setEffect(current, sequence, edit::EffectTarget::ofClip(clips[i]), added->createdEffects[i], change);
                if (!named)
                    return added;
                current = std::move(named->state);
            }
            edit::EditResult result = std::move(*added);
            result.changes = diff(state, current);
            result.state = std::move(current);
            return result;
        });
    if (!ok)
        return failed(m_controller->lastError());
    const int n = static_cast<int>(clips.size());
    const QString message = n == 1 ? tr("Added %1").arg(text(plugin->name)) : tr("Added %1 to %2 clips").arg(text(plugin->name)).arg(n);
    setMessage(message);
    return QVariantMap{{QStringLiteral("ok"), true},
                       {QStringLiteral("error"), QString()},
                       {QStringLiteral("message"), message},
                       {QStringLiteral("clips"), n}};
#else
    (void)pluginId;
    return failed(tr("Audio plugins are not available here"));
#endif
}

QVariantMap PluginBridge::effectInfo(const QString& effectKey) const {
    const auto id = parseEffectKey(effectKey);
    const auto found = id ? findEffect(*id) : std::nullopt;
    if (!found || !isPluginEffect(found->effect.effect))
        return {{QStringLiteral("plugin"), false}};
    QVariantMap out{{QStringLiteral("plugin"), true},
                    {QStringLiteral("hasEditor"), false},
                    {QStringLiteral("kind"), QString()},
                    {QStringLiteral("text"), QString()},
                    {QStringLiteral("missedBlocks"), 0}};
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
    if (const auto plugin = m_catalog ? m_catalog->find(found->effect.effect) : std::nullopt) {
        out.insert(QStringLiteral("name"), text(plugin->name));
        out.insert(QStringLiteral("vendor"), text(plugin->vendor));
        out.insert(QStringLiteral("format"), plugin->format == "clap" ? QStringLiteral("CLAP") : QStringLiteral("VST3"));
        out.insert(QStringLiteral("hasEditor"), plugin->editor);
    } else if (const EffectData* name = found->effect.findData(plughost::kPluginNameKey); name && name->text()) {
        out.insert(QStringLiteral("name"), text(*name->text()));
    }
    if (m_effects) {
        const plughost::PluginEffectReport report = m_effects->status(found->effect);
        QString kind;
        switch (report.status) {
        case plughost::PluginEffectStatus::Missing: kind = QStringLiteral("missing"); break;
        case plughost::PluginEffectStatus::Crashed: kind = QStringLiteral("crashed"); break;
        case plughost::PluginEffectStatus::Failed: kind = QStringLiteral("failed"); break;
        case plughost::PluginEffectStatus::Hung: kind = QStringLiteral("hung"); break;
        default: break;
        }
        out.insert(QStringLiteral("kind"), kind);
        out.insert(QStringLiteral("text"), text(report.message));
        out.insert(QStringLiteral("missedBlocks"), static_cast<qint64>(report.missedBlocks));
        if (kind == QLatin1String("missing"))
            out.insert(QStringLiteral("hasEditor"), false);
    }
#endif
    if (out.value(QStringLiteral("name")).toString().isEmpty())
        out.insert(QStringLiteral("name"), text(found->effect.effect));
    return out;
}

std::uint64_t PluginBridge::statusSignature() const {
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq || !m_effects)
        return 0;
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&](std::uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    const auto stack = [&](const EffectStack& effects) {
        for (const EffectInstance& e : effects) {
            if (!isPluginEffect(e.effect))
                continue;
            const plughost::PluginEffectReport r = m_effects->status(e);
            mix(e.id.value);
            mix(static_cast<std::uint64_t>(r.status));
            mix(r.missedBlocks > 0 ? 1 : 0);
        }
    };
    for (const TrackPtr& t : seq->audioTracks) {
        for (const Clip& c : t->clips)
            stack(c.effects());
        stack(t->effects);
    }
    stack(seq->audioEffects);
    return h;
#else
    return 0;
#endif
}

void PluginBridge::pollStatus() {
    const bool now = scanning();
    if (now != m_wasScanning) {
        m_wasScanning = now;
        emit scanChanged();
        if (!now && !m_ownScan) {
            refresh(); // the app's background scan finished
            bumpRevision();
        }
    }
    const std::uint64_t signature = statusSignature();
    if (signature != m_statusSignature) {
        m_statusSignature = signature;
        ++m_revision;
        emit revisionChanged();
    }
}

// ---- Editors and their state -----------------------------------------------------------------------------------------

QString PluginBridge::openEditor(const QString& effectKey) {
    // Errors also go to `message` (the header's button has no room for them; Main.qml toasts it).
    const auto fail = [this](const QString& why) {
        setMessage(why);
        return why;
    };
    if (!available() || !m_opener)
        return fail(tr("Audio plugins are not available here"));
    const auto id = parseEffectKey(effectKey);
    const auto found = id ? findEffect(*id) : std::nullopt;
    if (!found || !isPluginEffect(found->effect.effect))
        return fail(tr("Choose a plugin effect"));
    const QVariantMap info = effectInfo(effectKey);
    if (info.value(QStringLiteral("kind")).toString() == QLatin1String("missing"))
        return fail(info.value(QStringLiteral("text")).toString());
    using Done = std::function<void(PluginBridge&)>;
    editorJob([opener = m_opener, counter = m_counter, effect = found->effect, key = id->value]() -> Done {
        const std::expected<void, Error> opened = opener(effect);
        if (!opened) {
            const QString why = text(opened.error().message);
            return [why](PluginBridge& self) { self.setMessage(why); };
        }
        const std::optional<std::uint64_t> count = counter ? counter(effect) : std::nullopt;
        return [key, count](PluginBridge& self) {
            self.m_editing[key] = count;
            self.m_editorTimer.start();
        };
    });
    return {};
}

QStringList PluginBridge::editing() const {
    QStringList out;
    for (const auto& [id, count] : m_editing)
        out << effectKey(EffectInstanceId{id});
    return out;
}

QString PluginBridge::storeState(EffectInstanceId id, const std::string& base64, bool merge) {
#if defined(WABCUT_WITH_AUDIO_PLUGINS)
    const auto found = findEffect(id);
    if (!found || !m_controller)
        return tr("The plugin effect is gone");
    EffectInstance next = found->effect;
    if (auto stored = plughost::setPluginState(next, base64); !stored)
        return text(stored.error().message);
    if (plughost::pluginState(found->effect) == plughost::pluginState(next))
        return {}; // nothing new
    // Every "plugin.state.*" entry of the old and the new state: the new value, or empty (removed).
    edit::EffectChange change;
    std::set<std::string> keys;
    for (const EffectData& d : found->effect.data) {
        if (d.key.starts_with(kStatePrefix))
            keys.insert(d.key);
    }
    for (const EffectData& d : next.data) {
        if (d.key.starts_with(kStatePrefix))
            keys.insert(d.key);
    }
    for (const std::string& key : keys) {
        const EffectData* now = next.findData(key);
        change.data.push_back(now ? *now : EffectData{key, std::string()});
    }
    const SequenceId sequence = m_controller->currentSequenceId();
    const edit::EffectTarget target = found->target;
    const std::string mergeKey = merge ? "pluginState:" + std::to_string(id.value) : std::string();
    const bool ok = m_controller->applyEditFn(
        QT_TRANSLATE_NOOP("wabcut::app::PluginBridge", "Plugin Settings"),
        [&](const ProjectState& state) { return edit::setEffect(state, sequence, target, id, change); }, mergeKey);
    return ok ? QString() : m_controller->lastError();
#else
    (void)id;
    (void)base64;
    (void)merge;
    return tr("Audio plugins are not available here");
#endif
}

QString PluginBridge::captureState(const QString& effectKey) {
    if (!available())
        return tr("Audio plugins are not available here");
    const auto id = parseEffectKey(effectKey);
    const auto found = id ? findEffect(*id) : std::nullopt;
    if (!found || !isPluginEffect(found->effect.effect))
        return tr("Choose a plugin effect");
    if (!m_capturer)
        return tr("Audio plugins are not available here");
    QString result;
    editorJob([capturer = m_capturer, effect = found->effect, key = *id, &result,
               synchronous = m_synchronous]() -> std::function<void(PluginBridge&)> {
        const std::expected<std::string, Error> state = capturer(effect);
        return [state, key, synchronous, &result](PluginBridge& self) {
            QString error = state ? QString() : text(state.error().message);
            if (state) {
                if (self.m_controller)
                    self.m_controller->endPropertyGesture();
                error = self.storeState(key, *state, false);
                if (self.m_controller)
                    self.m_controller->endPropertyGesture();
            }
            if (synchronous)
                result = error; // only while captureState() runs (synchronous mode)
            else if (!error.isEmpty())
                self.setMessage(error);
        };
    });
    return result;
}

void PluginBridge::pollEditors() {
    if (m_editing.empty()) {
        m_editorTimer.stop();
        return;
    }
    for (auto it = m_editing.begin(); it != m_editing.end();) {
        const auto found = findEffect(EffectInstanceId{it->first});
        if (!found) {
            it = m_editing.erase(it);
            continue;
        }
        const std::optional<std::uint64_t> count = m_counter ? m_counter(found->effect) : std::nullopt;
        if (count && it->second && *count != *it->second && m_capturer && !m_capturing[it->first]) {
            // The plugin's editor changed its state: store it (merged into one undo step per editing session).
            m_capturing[it->first] = true;
            using Done = std::function<void(PluginBridge&)>;
            editorJob([capturer = m_capturer, effect = found->effect, key = it->first]() -> Done {
                const std::expected<std::string, Error> state = capturer(effect);
                return [state, key](PluginBridge& self) {
                    self.m_capturing.erase(key);
                    const QString error = state ? self.storeState(EffectInstanceId{key}, *state, true)
                                                : text(state.error().message);
                    if (!error.isEmpty())
                        self.setMessage(error);
                };
            });
        }
        if (count)
            it->second = count;
        ++it;
    }
}

} // namespace wabcut::app
