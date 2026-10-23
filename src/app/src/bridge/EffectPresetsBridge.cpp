#include "EffectPresetsBridge.h"

#include "BridgeIds.h"
#include "CommandPalette.h"
#include "EffectControls.h"
#include "ProjectController.h"

#include <wabcut/command/Engine.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/EffectPresets.h>
#include <wabcut/model/MediaLocator.h>

#include <wabcut/model/ProjectIO.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QQmlEngine>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <map>
#include <mutex>

namespace wabcut::app {

using Json = nlohmann::json;

namespace {

/// QSettings key of the presets file dialogs' folder.
constexpr const char* kPresetsFolderKey = "effects/presetsFolder";

QString tx(const char* text) { return QCoreApplication::translate("wabcut::app::EffectPresetsBridge", text); }

QString fromUtf8(const std::string& text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

/// The engine's ProjectController, for the QML singleton. EffectPresets is registered with QML_SINGLETON and is
/// default-constructible, so the engine constructs it directly and never calls create() (Qt 6.11: Constructor wins
/// over Factory): the singleton looks the controller up itself once the engine has given it a context. A bridge
/// made outside QML (tests) has no engine and keeps the controller it is given.
ProjectController* engineController(const QObject* bridge) {
    QQmlEngine* engine = qmlEngine(bridge);
    return engine ? engine->singletonInstance<ProjectController*>("Wabcut", "ProjectController") : nullptr;
}

std::pair<std::int32_t, std::int32_t> frameOf(const ProjectController* controller) {
    if (controller) {
        if (const SequencePtr seq = controller->currentSequence())
            return {seq->width, seq->height};
    }
    return {1920, 1080};
}

QString kindName(EffectKind kind) {
    switch (kind) {
    case EffectKind::Video:
        return QStringLiteral("video");
    case EffectKind::Audio:
        return QStringLiteral("audio");
    case EffectKind::VideoTransition:
        return QStringLiteral("videoTransition");
    case EffectKind::AudioTransition:
        return QStringLiteral("audioTransition");
    }
    return QStringLiteral("video");
}

QVariantMap presetItem(const EffectPreset& p, bool builtIn, const QString& category) {
    QStringList effects;
    for (const EffectInstance& e : p.effects) {
        if (e.effect == kMotionEffect)
            effects << tx("Motion");
        else if (const EffectDescriptor* d = EffectRegistry::builtin().find(e.effect))
            effects << QCoreApplication::translate("wabcut::app::EffectControlsModel", d->displayName.data());
        else
            effects << fromUtf8(e.effect);
    }
    QString description = fromUtf8(p.description);
    if (description.isEmpty())
        description = effects.join(QStringLiteral(", "));
    const QString name = fromUtf8(p.name);
    return QVariantMap{
        {QStringLiteral("id"), QStringLiteral("preset:") + name},
        {QStringLiteral("name"), name},
        {QStringLiteral("category"), category},
        {QStringLiteral("description"), description},
        {QStringLiteral("icon"), isLumetriPreset(p) ? QStringLiteral("sun") : QStringLiteral("layers")},
        {QStringLiteral("kind"), QStringLiteral("preset")},
        {QStringLiteral("presetKind"), kindName(p.kind())},
        {QStringLiteral("presetType"), fromUtf8(std::string(displayName(p.type)))},
        {QStringLiteral("builtIn"), builtIn},
        {QStringLiteral("item"), QStringLiteral("preset:") + name},
    };
}

std::optional<QVariantMap> effectItem(const std::string& id) {
    const EffectDescriptor* d = EffectRegistry::builtin().find(id);
    if (!d || !isListedEffect(*d))
        return std::nullopt;
    // An audio plug-in (its descriptor comes from the plug-in catalog): its own names, untranslated; applied through
    // PluginBridge (`plugin`), which keeps the plug-in's name with the effect.
    const bool plugin = d->id.starts_with(kPluginEffectPrefix);
    const auto text = [&](std::string_view s) {
        return plugin ? fromUtf8(std::string(s))
                      : QCoreApplication::translate("wabcut::app::EffectControlsModel", std::string(s).c_str());
    };
    return QVariantMap{
        {QStringLiteral("id"), fromUtf8(std::string(d->id))},
        {QStringLiteral("name"), text(d->displayName)},
        {QStringLiteral("category"), text(d->category)},
        {QStringLiteral("description"), fromUtf8(std::string(d->description))},
        {QStringLiteral("icon"), effectLibraryIcon(*d)},
        {QStringLiteral("kind"), kindName(d->kind)},
        {QStringLiteral("plugin"), plugin},
        {QStringLiteral("item"), QStringLiteral("effect:") + fromUtf8(std::string(d->id))},
    };
}

bool matches(const QString& query, const QVariantMap& item) {
    if (query.isEmpty())
        return true;
    return std::max({fuzzyScore(query, item.value(QStringLiteral("name")).toString()) * 3,
                     fuzzyScore(query, item.value(QStringLiteral("category")).toString())}) > 0;
}

/// An item of a custom bin or the favourites: the effect's or preset's row, or nullopt when it is gone.
std::optional<QVariantMap> itemFor(const std::string& reference, const EffectsLibrary& library, std::int32_t w, std::int32_t h) {
    if (reference.starts_with(kEffectItemPrefix))
        return effectItem(reference.substr(kEffectItemPrefix.size()));
    const std::string name = reference.substr(kPresetItemPrefix.size());
    if (const EffectPreset* p = library.findPreset(name))
        return presetItem(*p, false, fromUtf8(p->bin));
    if (const auto b = findBuiltinEffectPreset(name, w, h))
        return presetItem(*b, true, fromUtf8(b->bin));
    return std::nullopt;
}

} // namespace

// ---- Folder keys --------------------------------------------------------------------------------------------------------

QString effectsCustomBinKey(const QString& name) { return QStringLiteral("custom:") + name; }

QString effectsPresetsBinKey(const QString& path) {
    return path.isEmpty() ? QStringLiteral("presets") : QStringLiteral("presets/u:") + path;
}

QString effectsBuiltInPresetsBinKey(const QString& path, bool lumetri) {
    return (lumetri ? QStringLiteral("lumetriPresets/b:") : QStringLiteral("presets/b:")) + path;
}

// ---- The app's library ------------------------------------------------------------------------------------------------

std::shared_ptr<command::EffectLibraryServices> sharedEffectLibrary() {
    static std::once_flag once;
    static std::shared_ptr<command::EffectLibraryServices> library;
    std::call_once(once, [] {
        const QString env = qEnvironmentVariable("WABCUT_EFFECTS_LIBRARY");
        const QString file = !env.isEmpty() ? env
                                            : QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                                                  .filePath(QStringLiteral("effects-library.json"));
        library = std::make_shared<command::EffectLibraryServices>(std::filesystem::path(file.toStdU16String()));
    });
    return library;
}

std::optional<EffectPreset> effectPresetForItem(ProjectController* controller, const QString& itemId) {
    if (!itemId.startsWith(QLatin1String("preset:")))
        return std::nullopt;
    const std::string name = itemId.mid(7).toStdString();
    const EffectsLibrary library = sharedEffectLibrary()->library();
    if (const EffectPreset* p = library.findPreset(name))
        return *p;
    const auto [w, h] = frameOf(controller);
    return findBuiltinEffectPreset(name, w, h);
}

QString applyEffectPresetToClips(ProjectController& controller, const std::vector<ClipId>& clips, const QString& name) {
    if (clips.empty())
        return tx("Select a clip first");
    command::EnginePolicy policy;
    policy.services = sharedEffectLibrary();
    command::CommandEngine engine(controller.session(), command::builtinCommands(), policy);
    Json ids = Json::array();
    for (const ClipId c : clips)
        ids.push_back(c.value);
    command::CommandRequest request;
    request.command = "effects.applyPreset";
    request.params = Json{{"sequence", controller.currentSequenceId().value}, {"preset", name.toStdString()}, {"clips", ids}};
    const command::CommandResult result = engine.execute(request);
    if (!result.ok())
        return fromUtf8(result.error->message);
    return {};
}

// ---- Bridge -------------------------------------------------------------------------------------------------------------

EffectPresetsBridge::EffectPresetsBridge(QObject* parent) : QObject(parent), m_library(sharedEffectLibrary()) {
    m_seen = m_library->revision();
    // The QML singleton (see engineController): its controller once the engine has made it, then the views read the
    // built-in presets again for the sequence's frame size.
    QTimer::singleShot(0, this, [this] {
        if (m_controller)
            return;
        setController(engineController(this));
        if (m_controller) {
            ++m_revision;
            emit changed();
        }
    });
}

EffectPresetsBridge::~EffectPresetsBridge() = default;

EffectPresetsBridge* EffectPresetsBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new EffectPresetsBridge;
    if (qmlEngine)
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    return bridge;
}

void EffectPresetsBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (m_controller) {
        // The assistant may change the library too: look again after every edit and on sequence switches (the
        // built-in presets follow the frame size).
        connect(m_controller, &ProjectController::revisionChanged, this, &EffectPresetsBridge::noteChange);
        connect(m_controller, &ProjectController::sequenceChanged, this, [this] {
            ++m_revision;
            emit changed();
        });
    }
}

void EffectPresetsBridge::noteChange() {
    const std::uint64_t now = m_library->revision();
    if (now != m_seen) {
        m_seen = now;
        ++m_revision;
        emit changed();
    }
}

QStringList EffectPresetsBridge::customBins() const {
    QStringList out;
    for (const EffectsCustomBin& b : m_library->library().customBins)
        out << fromUtf8(b.name);
    return out;
}

QStringList EffectPresetsBridge::presetBins() const {
    QStringList out;
    const EffectsLibrary library = m_library->library();
    std::vector<std::string> bins = library.presetBins;
    for (const EffectPreset& p : library.presets) {
        if (!p.bin.empty() && std::find(bins.begin(), bins.end(), p.bin) == bins.end())
            bins.push_back(p.bin);
    }
    std::sort(bins.begin(), bins.end());
    for (const std::string& b : bins)
        out << fromUtf8(b);
    return out;
}

void EffectPresetsBridge::setCurrentItem(const QString& item) {
    if (item == m_currentItem)
        return;
    m_currentItem = item;
    emit currentItemChanged();
}

void EffectPresetsBridge::setCurrentBin(const QString& bin) {
    if (bin == m_currentBin)
        return;
    m_currentBin = bin;
    emit currentItemChanged();
}

QString EffectPresetsBridge::libraryError() const { return fromUtf8(m_library->error()); }

int EffectPresetsBridge::userPresetCount() const { return static_cast<int>(m_library->library().presets.size()); }

QUrl EffectPresetsBridge::presetsFolder() const {
    QString folder = m_persistent ? QSettings().value(QLatin1String(kPresetsFolderKey)).toString() : m_folder;
    if (folder.isEmpty() || !QFileInfo(folder).isDir())
        folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return QUrl::fromLocalFile(folder);
}

void EffectPresetsBridge::rememberFolder(const QString& filePath) {
    const QString folder = QFileInfo(filePath).absolutePath();
    if (folder.isEmpty())
        return;
    if (m_persistent) {
        if (QSettings().value(QLatin1String(kPresetsFolderKey)).toString() == folder)
            return;
        QSettings().setValue(QLatin1String(kPresetsFolderKey), folder);
    } else {
        if (m_folder == folder)
            return;
        m_folder = folder;
    }
    emit presetsFolderChanged();
}

void EffectPresetsBridge::setLastError(const QString& error) {
    if (error == m_lastError)
        return;
    m_lastError = error;
    emit lastErrorChanged();
}

QString EffectPresetsBridge::customBinKey(const QString& name) const { return effectsCustomBinKey(name); }

QString EffectPresetsBridge::presetsBinKey(const QString& path) const { return effectsPresetsBinKey(path); }

QVariantList EffectPresetsBridge::leadingGroups(const QString& query) const {
    if (!m_controller)
        const_cast<EffectPresetsBridge*>(this)->setController(engineController(this));
    const QString q = query.trimmed();
    const EffectsLibrary library = m_library->library();
    const auto [w, h] = frameOf(m_controller);
    QVariantList out;

    // Favorites: effects, transitions and presets the user starred.
    QVariantList favorites;
    for (const std::string& ref : library.favorites) {
        if (auto item = itemFor(ref, library, w, h); item && matches(q, *item))
            favorites.append(*item);
    }
    if (!favorites.isEmpty()) {
        const QString key = QStringLiteral("favorites");
        const int count = static_cast<int>(favorites.size());
        out.append(QVariantMap{{QStringLiteral("title"), tx("Favorites")},
                               {QStringLiteral("icon"), QStringLiteral("bookmark")},
                               {QStringLiteral("kind"), key},
                               {QStringLiteral("key"), key},
                               {QStringLiteral("count"), count},
                               {QStringLiteral("bins"), QVariantList{QVariantMap{{QStringLiteral("title"), QString()},
                                                                                 {QStringLiteral("key"), key},
                                                                                 {QStringLiteral("path"), QString()},
                                                                                 {QStringLiteral("builtIn"), false},
                                                                                 {QStringLiteral("count"), count},
                                                                                 {QStringLiteral("items"), favorites}}}}});
    }

    // Presets and Lumetri Presets: Premiere's bins, built-in first (alphabetical), then the user's bins and presets.
    struct Bin {
        QString title;
        QString presetsBin; // the user's bin path ("" for built-in bins and the Presets bin itself)
        QString key;        // stable folder key
        QString path;       // untranslated bin path ("" for the Presets bin itself)
        bool builtIn = false;
        QVariantList items;
    };
    std::map<QString, Bin> presetBins;
    std::map<QString, Bin> lumetriBins;
    for (const EffectPreset& p : builtinEffectPresets(w, h)) {
        const bool lumetri = isLumetriPreset(p);
        const QString title = lumetri ? fromUtf8(p.bin.size() > 16 ? p.bin.substr(16) : std::string()) : fromUtf8(p.bin);
        QVariantMap item = presetItem(p, true, title);
        if (!matches(q, item))
            continue;
        Bin& bin = (lumetri ? lumetriBins : presetBins)[QStringLiteral("0") + title];
        bin.title = title;
        bin.key = effectsBuiltInPresetsBinKey(title, lumetri);
        bin.path = title;
        bin.builtIn = true;
        bin.items.append(item);
    }
    // The user's presets bins (also empty ones) after the built-in bins; the Presets bin's own presets last. Bin paths
    // compare without case (as the commands do), so "Mine" and "mine" are one bin, under the first spelling seen.
    const auto userBin = [&](const std::string& path) -> Bin& {
        const QString title = fromUtf8(path);
        Bin& bin = presetBins[QStringLiteral("1") + title.toCaseFolded()];
        if (bin.key.isEmpty()) {
            bin.title = title;
            bin.presetsBin = title;
            bin.key = effectsPresetsBinKey(title);
            bin.path = title;
        }
        return bin;
    };
    for (const std::string& path : library.presetBins)
        userBin(path);
    for (const EffectPreset& p : library.presets) {
        QVariantMap item = presetItem(p, false, p.bin.empty() ? tx("Presets") : fromUtf8(p.bin));
        if (!matches(q, item))
            continue;
        if (p.bin.empty()) {
            Bin& bin = presetBins[QStringLiteral("2")];
            bin.key = effectsPresetsBinKey({});
            bin.items.append(item);
        } else {
            userBin(p.bin).items.append(item);
        }
    }
    const auto group = [&](const QString& title, const QString& icon, const QString& kind, const std::map<QString, Bin>& bins) {
        QVariantList list;
        int count = 0;
        for (const auto& [key, bin] : bins) {
            // A search hides empty bins; without one, the user's empty bins show (to drop presets in later).
            if (bin.items.isEmpty() && (!q.isEmpty() || bin.presetsBin.isEmpty()))
                continue;
            count += static_cast<int>(bin.items.size());
            list.append(QVariantMap{{QStringLiteral("title"), bin.title},
                                    {QStringLiteral("presetsBin"), bin.presetsBin},
                                    {QStringLiteral("key"), bin.key},
                                    {QStringLiteral("path"), bin.path},
                                    {QStringLiteral("builtIn"), bin.builtIn},
                                    {QStringLiteral("count"), static_cast<int>(bin.items.size())},
                                    {QStringLiteral("items"), bin.items}});
        }
        if (!list.isEmpty()) {
            out.append(QVariantMap{{QStringLiteral("title"), title},
                                   {QStringLiteral("icon"), icon},
                                   {QStringLiteral("kind"), kind},
                                   {QStringLiteral("key"), kind},
                                   {QStringLiteral("count"), count},
                                   {QStringLiteral("bins"), list}});
        }
    };
    group(tx("Presets"), QStringLiteral("layers"), QStringLiteral("presets"), presetBins);
    group(tx("Lumetri Presets"), QStringLiteral("sun"), QStringLiteral("lumetriPresets"), lumetriBins);
    return out;
}

QVariantList EffectPresetsBridge::customGroups(const QString& query) const {
    if (!m_controller)
        const_cast<EffectPresetsBridge*>(this)->setController(engineController(this));
    const QString q = query.trimmed();
    const EffectsLibrary library = m_library->library();
    const auto [w, h] = frameOf(m_controller);
    QVariantList out;
    for (const EffectsCustomBin& b : library.customBins) {
        QVariantList items;
        for (const std::string& ref : b.items) {
            if (auto item = itemFor(ref, library, w, h); item && matches(q, *item))
                items.append(*item);
        }
        if (!q.isEmpty() && items.isEmpty())
            continue;
        const QString key = effectsCustomBinKey(fromUtf8(b.name));
        const int count = static_cast<int>(items.size());
        out.append(QVariantMap{{QStringLiteral("title"), fromUtf8(b.name)},
                               {QStringLiteral("icon"), QStringLiteral("folder")},
                               {QStringLiteral("kind"), QStringLiteral("custom")},
                               {QStringLiteral("key"), key},
                               {QStringLiteral("count"), count},
                               {QStringLiteral("customBin"), fromUtf8(b.name)},
                               {QStringLiteral("bins"), QVariantList{QVariantMap{{QStringLiteral("title"), QString()},
                                                                                 {QStringLiteral("key"), key},
                                                                                 {QStringLiteral("count"), count},
                                                                                 {QStringLiteral("items"), items}}}}});
    }
    return out;
}

bool EffectPresetsBridge::isFavorite(const QString& item) const {
    return m_library->library().isFavorite(itemReference(item).toStdString());
}

QVariantMap EffectPresetsBridge::preset(const QString& name) const {
    if (!m_controller)
        const_cast<EffectPresetsBridge*>(this)->setController(engineController(this));
    const auto p = effectPresetForItem(m_controller, QStringLiteral("preset:") + name);
    if (!p)
        return {};
    const bool builtIn = !m_library->library().findPreset(name.toStdString());
    return QVariantMap{{QStringLiteral("name"), fromUtf8(p->name)},
                       {QStringLiteral("type"), fromUtf8(std::string(toString(p->type)))},
                       {QStringLiteral("typeName"), fromUtf8(std::string(displayName(p->type)))},
                       {QStringLiteral("description"), fromUtf8(p->description)},
                       {QStringLiteral("bin"), fromUtf8(p->bin)},
                       {QStringLiteral("builtIn"), builtIn},
                       {QStringLiteral("kind"), kindName(p->kind())}};
}

QString EffectPresetsBridge::itemReference(const QString& itemId) const {
    if (itemId.startsWith(QLatin1String("preset:")) || itemId.startsWith(QLatin1String("effect:")))
        return itemId;
    return QStringLiteral("effect:") + itemId;
}

QString EffectPresetsBridge::run(const std::string& name, Json params, Json* data) {
    if (!m_controller)
        setController(engineController(this));
    if (!m_controller) {
        setLastError(tx("No project"));
        return m_lastError;
    }
    command::EnginePolicy policy;
    policy.services = m_library;
    command::CommandEngine engine(m_controller->session(), command::builtinCommands(), policy);
    if (const SequenceId seq = m_controller->currentSequenceId(); seq.isValid())
        params["sequence"] = seq.value;
    command::CommandRequest request;
    request.command = name;
    request.params = std::move(params);
    const command::CommandResult result = engine.execute(request);
    noteChange();
    if (!result.ok()) {
        setLastError(fromUtf8(result.error->message));
        return m_lastError;
    }
    if (data)
        *data = result.data;
    setLastError({});
    return {};
}

// ---- Applying -----------------------------------------------------------------------------------------------------------

QString EffectPresetsBridge::applyToSelection(const QString& name) {
    if (!m_controller)
        setController(engineController(this));
    if (!m_controller)
        return tx("No project");
    return applyEffectPresetToClips(*m_controller, m_controller->selection(), name);
}

// ---- Save Preset ----------------------------------------------------------------------------------------------------------

QStringList EffectPresetsBridge::sectionsOfClip(const QString& section) const {
    if (!m_controller)
        const_cast<EffectPresetsBridge*>(this)->setController(engineController(this));
    QStringList out;
    if (!m_controller)
        return out;
    EffectControlsModel* controls = m_controller->effectControls();
    const auto clip = controls ? controls->sectionClip(section) : std::nullopt;
    return clip ? controls->sectionsOf(*clip) : out;
}

QString EffectPresetsBridge::sectionTitle(const QString& section) const {
    if (!m_controller)
        const_cast<EffectPresetsBridge*>(this)->setController(engineController(this));
    if (!m_controller || !m_controller->effectControls())
        return {};
    return m_controller->effectControls()->row(QStringLiteral("h:") + section).value(QStringLiteral("title")).toString();
}

QString EffectPresetsBridge::savePreset(const QStringList& sections, const QString& name, const QString& type,
                                        const QString& description, const QString& bin) {
    if (!m_controller)
        setController(engineController(this));
    if (!m_controller || !m_controller->effectControls())
        return tx("No project");
    if (sections.isEmpty())
        return tx("Choose the effects to save");
    EffectControlsModel* controls = m_controller->effectControls();
    std::optional<ClipId> clip;
    Json fixed = Json::array();
    std::vector<std::uint64_t> instances;
    for (const QString& s : sections) {
        const auto owner = controls->sectionClip(s);
        if (!owner)
            return tx("Choose effects of a clip");
        if (clip && *clip != *owner)
            return tx("Save the video and the audio effects as two presets");
        clip = owner;
        if (s == QLatin1String("motion"))
            fixed.push_back("motion");
        else if (s == QLatin1String("opacity"))
            fixed.push_back("opacity");
        else if (s == QLatin1String("audio")) {
            fixed.push_back("volume");
            fixed.push_back("panner");
        } else if (s.startsWith(QLatin1Char('e')))
            instances.push_back(s.mid(1).toULongLong());
    }
    const SequencePtr seq = m_controller->currentSequence();
    const auto loc = seq ? seq->findClip(*clip) : std::nullopt;
    if (!loc)
        return tx("Choose effects of a clip");
    Json indices = Json::array();
    const EffectStack& stack = loc->clip().effects();
    for (std::size_t i = 0; i < stack.size(); ++i) {
        if (std::find(instances.begin(), instances.end(), stack[i].id.value) != instances.end())
            indices.push_back(i);
    }
    return run("effects.savePreset", Json{{"clip", clip->value},
                                          {"effects", indices},
                                          {"fixed", fixed},
                                          {"name", name.trimmed().toStdString()},
                                          {"type", type.isEmpty() ? std::string("scale") : type.toStdString()},
                                          {"description", description.toStdString()},
                                          {"bin", bin.toStdString()}});
}

QString EffectPresetsBridge::saveEffectOfClip(const QString& clip, const QString& effect, const QString& name,
                                              const QString& type, const QString& description, const QString& bin) {
    if (!m_controller)
        setController(engineController(this));
    if (!m_controller)
        return tx("No project");
    const auto id = parseClipKey(clip);
    const SequencePtr seq = m_controller->currentSequence();
    const auto loc = id && seq ? seq->findClip(*id) : std::nullopt;
    if (!loc)
        return tx("Choose effects of a clip");
    const EffectStack& stack = loc->clip().effects();
    const std::uint64_t instance = effect.toULongLong();
    const auto it = std::find_if(stack.begin(), stack.end(), [&](const EffectInstance& e) { return e.id.value == instance; });
    if (it == stack.end())
        return tx("The clip has no such effect");
    return run("effects.savePreset", Json{{"clip", id->value},
                                          {"effects", Json::array({static_cast<std::size_t>(it - stack.begin())})},
                                          {"name", name.trimmed().toStdString()},
                                          {"type", type.isEmpty() ? std::string("scale") : type.toStdString()},
                                          {"description", description.toStdString()},
                                          {"bin", bin.toStdString()}});
}

QString EffectPresetsBridge::suggestName(const QString& base) const {
    if (!m_controller)
        const_cast<EffectPresetsBridge*>(this)->setController(engineController(this));
    const EffectsLibrary library = m_library->library();
    const auto [w, h] = frameOf(m_controller);
    const QString stem = base.trimmed().isEmpty() ? tx("Preset") : base.trimmed();
    QString name = stem;
    for (int n = 2; library.findPreset(name.toStdString()) || findBuiltinEffectPreset(name.toStdString(), w, h); ++n)
        name = stem + QLatin1Char(' ') + QString::number(n);
    return name;
}

// ---- Library items --------------------------------------------------------------------------------------------------------

QString EffectPresetsBridge::setPresetProperties(const QString& preset, const QString& name, const QString& type,
                                                 const QString& description) {
    Json params{{"preset", preset.toStdString()}, {"description", description.toStdString()}};
    if (!name.trimmed().isEmpty() && name.trimmed() != preset)
        params["name"] = name.trimmed().toStdString();
    if (!type.isEmpty())
        params["type"] = type.toStdString();
    return run("effects.setPresetProperties", std::move(params));
}

QString EffectPresetsBridge::movePreset(const QString& preset, const QString& bin) {
    return run("effects.setPresetProperties", Json{{"preset", preset.toStdString()}, {"bin", bin.toStdString()}});
}

QString EffectPresetsBridge::newCustomBin(const QString& item) {
    Json params = Json::object();
    if (!item.isEmpty())
        params["items"] = Json::array({itemReference(item).toStdString()});
    Json data;
    const QString error = run("effects.newCustomBin", std::move(params), &data);
    const QString name = error.isEmpty() && data.contains("customBin") ? fromUtf8(data["customBin"].get<std::string>()) : QString();
    if (!name.isEmpty())
        emit customBinCreated(name);
    else
        emit statusMessage(error.isEmpty() ? tx("Could not make a custom bin") : error);
    return name;
}

QString EffectPresetsBridge::newPresetsBin(const QString& path) {
    return run("effects.newPresetsBin", Json{{"bin", path.trimmed().toStdString()}});
}

QString EffectPresetsBridge::addToCustomBin(const QString& bin, const QString& item) {
    return run("effects.addToCustomBin", Json{{"customBin", bin.toStdString()}, {"items", Json::array({itemReference(item).toStdString()})}});
}

QString EffectPresetsBridge::renameCustomBin(const QString& bin, const QString& name) {
    Json data;
    const QString error = run("effects.renameCustomItem", Json{{"customBin", bin.toStdString()}, {"name", name.trimmed().toStdString()}}, &data);
    if (error.isEmpty() && data.contains("customBin"))
        emit binRenamed(effectsCustomBinKey(bin), effectsCustomBinKey(fromUtf8(data["customBin"].get<std::string>())));
    return error;
}

QString EffectPresetsBridge::renamePresetsBin(const QString& path, const QString& name) {
    Json data;
    const QString error = run("effects.renameCustomItem", Json{{"presetsBin", path.toStdString()}, {"name", name.trimmed().toStdString()}}, &data);
    if (error.isEmpty() && data.contains("presetsBin"))
        emit binRenamed(effectsPresetsBinKey(path), effectsPresetsBinKey(fromUtf8(data["presetsBin"].get<std::string>())));
    return error;
}

QString EffectPresetsBridge::deleteCustomItem(const QVariantMap& what) {
    Json params = Json::object();
    for (const char* key : {"preset", "presetsBin", "customBin", "item"}) {
        if (what.contains(QLatin1String(key)))
            params[key] = what.value(QLatin1String(key)).toString().toStdString();
    }
    if (params.contains("item"))
        params["item"] = itemReference(QString::fromStdString(params["item"].get<std::string>())).toStdString();
    return run("effects.deleteCustomItem", std::move(params));
}

QString EffectPresetsBridge::deleteCurrentItem() {
    const QVariantMap info = currentItemInfo();
    if (!info.value(QStringLiteral("deletable")).toBool()) {
        setLastError(info.value(QStringLiteral("reason")).toString());
        return m_lastError;
    }
    const QString error = info.value(QStringLiteral("action")).toString() == QLatin1String("removeFromBin")
        ? deleteCustomItem(QVariantMap{{QStringLiteral("customBin"), m_currentBin}, {QStringLiteral("item"), m_currentItem}})
        : deleteCustomItem(QVariantMap{{QStringLiteral("preset"), m_currentItem.mid(7)}});
    if (error.isEmpty() && (!m_currentItem.isEmpty() || !m_currentBin.isEmpty())) {
        // The item is gone (or out of the bin): nothing is current until the panel focuses another row.
        m_currentItem.clear();
        m_currentBin.clear();
        emit currentItemChanged();
    }
    return error;
}

QVariantMap EffectPresetsBridge::currentItemInfo() const {
    QString name;
    if (m_currentItem.startsWith(QLatin1String("preset:"))) {
        name = m_currentItem.mid(7);
    } else if (!m_currentItem.isEmpty()) {
        const std::string id = itemReference(m_currentItem).mid(7).toStdString();
        const EffectDescriptor* d = EffectRegistry::builtin().find(id);
        name = !d ? fromUtf8(id)
            : d->id.starts_with(kPluginEffectPrefix) ? fromUtf8(std::string(d->displayName))
                                                     : QCoreApplication::translate("wabcut::app::EffectControlsModel",
                                                                                   std::string(d->displayName).c_str());
    }
    QString action;
    QString question;
    QString confirmText;
    QString reason;
    if (m_currentItem.isEmpty()) {
        reason = tx("Select a custom item in the Effects panel");
    } else if (!m_currentBin.isEmpty()) {
        action = QStringLiteral("removeFromBin");
        question = tx("Remove %1 from %2?").arg(name, m_currentBin);
        confirmText = tx("Remove");
    } else if (m_currentItem.startsWith(QLatin1String("preset:")) && m_library->library().findPreset(name.toStdString())) {
        action = QStringLiteral("deletePreset");
        question = tx("Delete the preset %1? It leaves your custom bins and Favorites too.").arg(name);
        confirmText = tx("Delete");
    } else if (m_currentItem.startsWith(QLatin1String("preset:"))) {
        reason = tx("%1 is a built-in preset and cannot be deleted").arg(name);
    } else {
        reason = tx("Only custom items can be deleted");
    }
    return QVariantMap{{QStringLiteral("item"), m_currentItem},
                       {QStringLiteral("bin"), m_currentBin},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("deletable"), !action.isEmpty()},
                       {QStringLiteral("action"), action},
                       {QStringLiteral("question"), question},
                       {QStringLiteral("confirmText"), confirmText},
                       {QStringLiteral("reason"), reason}};
}

void EffectPresetsBridge::requestDeleteCurrentItem() {
    const QVariantMap info = currentItemInfo();
    if (info.value(QStringLiteral("deletable")).toBool()) {
        emit deleteCurrentItemRequested(info);
        return;
    }
    setLastError(info.value(QStringLiteral("reason")).toString());
    emit statusMessage(m_lastError);
}

QString EffectPresetsBridge::setFavorite(const QString& item, bool favorite) {
    return run("effects.setFavorite", Json{{"item", itemReference(item).toStdString()}, {"favorite", favorite}});
}

QString EffectPresetsBridge::importPresets(const QUrl& file, const QVariant& bin) {
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    Json params{{"path", path.toStdString()}};
    if (bin.isValid() && bin.typeId() == QMetaType::QString)
        params["bin"] = bin.toString().trimmed().toStdString();
    Json data;
    const QString error = run("effects.importPresets", std::move(params), &data);
    if (!error.isEmpty())
        return error;
    QStringList names;
    if (data.contains("imported")) {
        for (const Json& entry : data["imported"]) {
            if (entry.contains("name"))
                names << fromUtf8(entry["name"].get<std::string>());
        }
    }
    m_lastImported = names;
    emit lastImportedChanged();
    rememberFolder(path);
    return {};
}

QString EffectPresetsBridge::exportPath(const QUrl& file, bool* renamed) const {
    QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    *renamed = false;
    if (!path.isEmpty() && !path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive)) {
        path += fromUtf8(std::string(io::kEffectPresetsFileSuffix));
        *renamed = true;
    }
    return path;
}

QString EffectPresetsBridge::exportPresets(const QUrl& file, const QStringList& names) {
    bool renamed = false;
    const QString path = exportPath(file, &renamed);
    // The save dialog asked before replacing the file it showed; a name this bridge completed is never replaced silently.
    Json params{{"path", path.toStdString()}, {"overwrite", !renamed}};
    if (names.isEmpty()) {
        if (userPresetCount() == 0) {
            setLastError(tx("You have no presets to export: save one from Effect Controls first"));
            return m_lastError;
        }
        params["presetsBin"] = "";
    } else {
        Json list = Json::array();
        for (const QString& n : names)
            list.push_back(n.toStdString());
        params["presets"] = std::move(list);
    }
    const QString error = run("effects.exportPresets", std::move(params));
    if (error.isEmpty())
        rememberFolder(path);
    return error;
}

QString EffectPresetsBridge::exportPresetsBin(const QUrl& file, const QString& bin) {
    if (userPresetsIn(bin) == 0) {
        setLastError(bin.isEmpty() ? tx("You have no presets to export: save one from Effect Controls first")
                                   : tx("%1 has no presets of yours to export").arg(bin));
        return m_lastError;
    }
    bool renamed = false;
    const QString path = exportPath(file, &renamed);
    const QString error = run("effects.exportPresets", Json{{"path", path.toStdString()},
                                                            {"presetsBin", bin.toStdString()},
                                                            {"overwrite", !renamed}});
    if (error.isEmpty())
        rememberFolder(path);
    return error;
}

int EffectPresetsBridge::userPresetsIn(const QString& bin) const {
    const std::string path = bin.toStdString();
    int n = 0;
    for (const EffectPreset& p : m_library->library().presets) {
        // The commands' inBin(): the bin itself or a bin inside it, without case.
        const bool inside = path.empty() || samePresetName(p.bin, path)
            || (p.bin.size() > path.size() && p.bin[path.size()] == '/' && samePresetName(std::string_view(p.bin).substr(0, path.size()), path));
        if (inside)
            ++n;
    }
    return n;
}

QString EffectPresetsBridge::presetsFileName(const QString& name) const {
    QString base = name.trimmed();
    if (const qsizetype slash = base.lastIndexOf(QLatin1Char('/')); slash >= 0)
        base = base.mid(slash + 1).trimmed();
    if (base.isEmpty())
        base = tx("Presets");
    // Characters Windows (and the other systems) refuse in a file name.
    for (QChar& c : base) {
        if (c.unicode() < 0x20 || QStringLiteral("\\/:*?\"<>|").contains(c))
            c = QLatin1Char('_');
    }
    while (base.endsWith(QLatin1Char('.')) || base.endsWith(QLatin1Char(' ')))
        base.chop(1);
    if (base.isEmpty())
        base = tx("Presets");
    return base + fromUtf8(std::string(io::kEffectPresetsFileSuffix));
}

QUrl EffectPresetsBridge::presetsFileUrl(const QString& name) const {
    return QUrl::fromLocalFile(QDir(presetsFolder().toLocalFile()).filePath(presetsFileName(name)));
}

bool EffectPresetsBridge::isPresetsFile(const QUrl& file) const {
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.path();
    return path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive) && !QFileInfo(path).isDir();
}

} // namespace wabcut::app
