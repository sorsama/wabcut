#include "EffectsTreeModel.h"

#include "CommandPalette.h"
#include "EffectControls.h"
#include "EffectPresetsBridge.h"
#include "PluginBridge.h"
#include "ProjectController.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/EffectPresets.h>

#include <QCoreApplication>
#include <QQmlEngine>
#include <QSettings>

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>

namespace wabcut::app {

namespace {

/// QSettings keys of the expanded state.
constexpr const char* kExpandedKey = "effects/expandedFolders";
constexpr const char* kLastOpenedKey = "effects/lastOpenedFolder";

QString tx(const char* text) { return QCoreApplication::translate("wabcut::app::EffectsTreeModel", text); }

/// The roles' QML names, in Role order.
const char* const kRoleNames[] = {
    "rowKey",     "rowType",   "depth",       "parentKey",    "title",          "icon",       "count",
    "expanded",   "folderKind", "itemId",     "item",         "name",           "category",   "description",
    "kind",       "presetKind", "presetType", "builtIn",      "plugin",         "vendor",     "format",
    "problem",    "favorite",  "customBin",   "presetsBin",   "canHoldItems",   "canHoldPresets", "renamable",
    "deletable",  "removable", "userPresets", "home",         "tiles",
};
constexpr int kRoleCount = static_cast<int>(sizeof(kRoleNames) / sizeof(kRoleNames[0]));
static_assert(kRoleCount == EffectsTreeModel::TilesRole - EffectsTreeModel::RowKeyRole + 1, "a role without a name");

/// A row with every role at its empty value (QML delegates' required properties always get a value).
const QVariantMap& emptyRow() {
    static const QVariantMap row = [] {
        QVariantMap m;
        for (const char* name : kRoleNames)
            m.insert(QLatin1String(name), QString());
        for (const char* name : {"depth", "count", "userPresets"})
            m.insert(QLatin1String(name), 0);
        for (const char* name : {"expanded", "builtIn", "plugin", "favorite", "canHoldItems", "canHoldPresets", "renamable",
                                 "deletable", "removable", "home"})
            m.insert(QLatin1String(name), false);
        m.insert(QStringLiteral("tiles"), QVariantList());
        return m;
    }();
    return row;
}

QString str(const QVariantMap& m, const char* key) { return m.value(QLatin1String(key)).toString(); }

/// Whether a folder key is a top-level folder's ("custom:<name>" may hold any character).
bool isTopLevel(const QString& key) {
    return key.startsWith(QLatin1String("custom:")) || !key.contains(QLatin1Char('/'));
}

QVariantMap folderRow(const QString& title, const QString& icon, const QString& folderKind) {
    QVariantMap row = emptyRow();
    row.insert(QStringLiteral("rowType"), QStringLiteral("folder"));
    row.insert(QStringLiteral("title"), title);
    row.insert(QStringLiteral("name"), title);
    row.insert(QStringLiteral("icon"), icon);
    row.insert(QStringLiteral("folderKind"), folderKind);
    return row;
}

/// An item row from a bridge's item map ({id, name, category, description, icon, kind, item?, presetKind?, ...}).
QVariantMap itemRow(const QVariantMap& src) {
    QVariantMap row = emptyRow();
    const QString id = str(src, "id");
    row.insert(QStringLiteral("rowType"), QStringLiteral("item"));
    row.insert(QStringLiteral("itemId"), id);
    QString item = str(src, "item");
    if (item.isEmpty())
        item = id.startsWith(QLatin1String("preset:")) ? id : QStringLiteral("effect:") + id;
    row.insert(QStringLiteral("item"), item);
    row.insert(QStringLiteral("title"), src.value(QStringLiteral("name")));
    for (const char* key : {"name", "category", "description", "icon", "kind", "presetKind", "presetType", "vendor", "format",
                            "problem"}) {
        if (src.contains(QLatin1String(key)))
            row.insert(QLatin1String(key), src.value(QLatin1String(key)).toString());
    }
    for (const char* key : {"builtIn", "plugin"}) {
        if (src.contains(QLatin1String(key)))
            row.insert(QLatin1String(key), src.value(QLatin1String(key)).toBool());
    }
    row.insert(QStringLiteral("deletable"), str(row, "kind") == QLatin1String("preset") && !row.value(QStringLiteral("builtIn")).toBool());
    return row;
}

/// An item row of a plug-in (PluginBridge::allPlugins: {id, name, vendor, category, format, version, latencyText,
/// hasEditor, problem}).
QVariantMap pluginRow(const QVariantMap& p) {
    QVariantMap row = itemRow(p);
    QStringList about;
    for (const QString& part : {str(p, "vendor"), (str(p, "format") + QLatin1Char(' ') + str(p, "version")).trimmed(),
                                str(p, "latencyText")}) {
        if (!part.isEmpty())
            about << part;
    }
    row.insert(QStringLiteral("description"), about.join(QStringLiteral(" · ")));
    row.insert(QStringLiteral("icon"), QStringLiteral("sliders-horizontal"));
    row.insert(QStringLiteral("kind"), QStringLiteral("audio"));
    row.insert(QStringLiteral("plugin"), true);
    return row;
}

} // namespace

EffectsTreeModel::EffectsTreeModel(QObject* parent) : QAbstractListModel(parent) { load(); }

EffectsTreeModel::~EffectsTreeModel() = default;

void EffectsTreeModel::setSources(ProjectController* controller, EffectPresetsBridge* presets, PluginBridge* plugins) {
    if (m_presets)
        disconnect(m_presets, nullptr, this, nullptr);
    if (m_plugins)
        disconnect(m_plugins, nullptr, this, nullptr);
    m_controller = controller;
    m_presets = presets;
    m_plugins = plugins;
    if (m_presets) {
        connect(m_presets, &EffectPresetsBridge::changed, this, &EffectsTreeModel::rebuild);
        connect(m_presets, &EffectPresetsBridge::binRenamed, this, &EffectsTreeModel::renameKeys);
    }
    if (m_plugins) {
        connect(m_plugins, &PluginBridge::pluginsChanged, this, [this] {
            if (m_showPlugins)
                rebuild();
        });
        connect(m_plugins, &PluginBridge::availableChanged, this, &EffectsTreeModel::rebuild);
    }
    rebuild();
}

void EffectsTreeModel::setPersistent(bool persistent) {
    if (persistent == m_persistent)
        return;
    m_persistent = persistent;
    load();
    rebuild();
}

void EffectsTreeModel::classBegin() { m_deferred = true; }

void EffectsTreeModel::componentComplete() {
    m_deferred = false;
    if (!m_presets && !m_controller) {
        if (QQmlEngine* engine = qmlEngine(this)) {
            setSources(engine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"),
                       engine->singletonInstance<EffectPresetsBridge*>("Wabcut", "EffectPresets"),
                       engine->singletonInstance<PluginBridge*>("Wabcut", "Plugins"));
            return;
        }
    }
    rebuild();
}

// ---- Properties ---------------------------------------------------------------------------------------------------------

void EffectsTreeModel::setQuery(const QString& query) {
    if (query == m_query)
        return;
    m_query = query;
    m_searchCollapsed.clear(); // a new search opens every folder with a match again
    emit queryChanged();
    rebuild();
}

void EffectsTreeModel::setFilter(const QString& filter) {
    static const QStringList known{QStringLiteral("all"),         QStringLiteral("video"),   QStringLiteral("audio"),
                                   QStringLiteral("transitions"), QStringLiteral("presets"), QStringLiteral("favorites"),
                                   QStringLiteral("plugins")};
    const QString next = known.contains(filter) ? filter : QStringLiteral("all");
    if (next == m_filter)
        return;
    m_filter = next;
    emit filterChanged();
    rebuild();
}

void EffectsTreeModel::setShowPlugins(bool show) {
    if (show == m_showPlugins)
        return;
    m_showPlugins = show;
    emit showPluginsChanged();
    rebuild();
}

void EffectsTreeModel::setColumns(int columns) {
    columns = std::max(1, columns);
    if (columns == m_columns)
        return;
    m_columns = columns;
    emit columnsChanged();
    rebuild();
}

// ---- The model ------------------------------------------------------------------------------------------------------------

int EffectsTreeModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant EffectsTreeModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount() || role < RowKeyRole || role > TilesRole)
        return {};
    return m_rows[static_cast<std::size_t>(index.row())].value(QLatin1String(kRoleNames[role - RowKeyRole]));
}

QHash<int, QByteArray> EffectsTreeModel::roleNames() const {
    QHash<int, QByteArray> names;
    for (int i = 0; i < kRoleCount; ++i)
        names.insert(RowKeyRole + i, QByteArray(kRoleNames[i]));
    return names;
}

QVariantMap EffectsTreeModel::get(int row) const {
    if (row < 0 || row >= rowCount())
        return {};
    return m_rows[static_cast<std::size_t>(row)];
}

int EffectsTreeModel::indexOf(const QString& rowKey) const { return m_index.value(rowKey, -1); }

int EffectsTreeModel::indexOfItem(const QString& itemId) const {
    int copy = -1;
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        const QVariantMap& row = m_rows[i];
        QVariantList candidates;
        if (str(row, "rowType") == QLatin1String("tiles"))
            candidates = row.value(QStringLiteral("tiles")).toList();
        else
            candidates.append(row);
        for (const QVariant& v : candidates) {
            const QVariantMap item = v.toMap();
            if (str(item, "rowType") != QLatin1String("item") || str(item, "itemId") != itemId)
                continue;
            if (item.value(QStringLiteral("home")).toBool())
                return static_cast<int>(i);
            if (copy < 0)
                copy = static_cast<int>(i);
        }
    }
    return copy;
}

int EffectsTreeModel::parentIndex(int row) const {
    if (row < 0 || row >= rowCount())
        return -1;
    const QString parent = str(m_rows[static_cast<std::size_t>(row)], "parentKey");
    return parent.isEmpty() ? -1 : indexOf(parent);
}

// ---- Building -------------------------------------------------------------------------------------------------------------

bool EffectsTreeModel::keepItem(const QVariantMap& item) const {
    const QString kind = str(item, "kind");
    const bool preset = kind == QLatin1String("preset");
    if (m_filter == QLatin1String("video"))
        return kind == QLatin1String("video") || kind == QLatin1String("videoTransition")
            || (preset && str(item, "presetKind") == QLatin1String("video"));
    if (m_filter == QLatin1String("audio"))
        return kind == QLatin1String("audio") || kind == QLatin1String("audioTransition")
            || (preset && str(item, "presetKind") == QLatin1String("audio")) || item.value(QStringLiteral("plugin")).toBool();
    if (m_filter == QLatin1String("transitions"))
        return kind == QLatin1String("videoTransition") || kind == QLatin1String("audioTransition");
    if (m_filter == QLatin1String("presets"))
        return preset;
    return true; // "all", and "favorites" / "plugins" choose folders instead
}

void EffectsTreeModel::addPlugins(std::vector<Node>& top) const {
    if (!m_plugins || !m_showPlugins || !m_plugins->available())
        return;
    const QString q = m_query.trimmed();
    const QString groupBy = m_plugins->groupBy();
    const bool byCategory = groupBy == QLatin1String("category");
    // Favourites mark a plug-in's own row too (the star, the item menu's Add to / Remove from Favorites).
    std::set<std::string> favorites;
    if (m_presets) {
        const EffectsLibrary library = sharedEffectLibrary()->library();
        favorites.insert(library.favorites.begin(), library.favorites.end());
    }
    Node folder;
    folder.key = QStringLiteral("plugins");
    folder.folder = folderRow(tx("Audio Plug-ins"), QStringLiteral("sliders-horizontal"), QStringLiteral("plugins"));
    // The vendors or categories, alphabetical (as PluginBridge::groups), each plug-in by name (allPlugins' order).
    std::map<QString, Node> groups; // by case-folded title
    for (const QVariant& v : m_plugins->allPlugins()) {
        const QVariantMap p = v.toMap();
        if (!q.isEmpty()
            && std::max({fuzzyScore(q, str(p, "name")) * 3, fuzzyScore(q, str(p, "vendor")), fuzzyScore(q, str(p, "category"))}) <= 0)
            continue;
        QString title = str(p, byCategory ? "category" : "vendor");
        if (title.isEmpty())
            title = byCategory ? tx("Other") : tx("Unknown vendor");
        Node& node = groups[title.toCaseFolded()];
        if (node.key.isEmpty()) {
            node.key = QStringLiteral("plugins/") + PluginBridge::groupKey(p, groupBy);
            node.folder = folderRow(title, QStringLiteral("folder"), QStringLiteral("pluginGroup"));
        }
        QVariantMap row = pluginRow(p);
        row.insert(QStringLiteral("home"), true);
        row.insert(QStringLiteral("favorite"), favorites.contains(str(row, "item").toStdString()));
        node.items.append(row);
    }
    for (auto& [folded, node] : groups)
        folder.children.push_back(std::move(node));
    top.push_back(std::move(folder));
}

std::vector<EffectsTreeModel::Node> EffectsTreeModel::buildTree() const {
    std::vector<Node> top;
    const QString q = m_query.trimmed();
    const bool narrowed = !q.isEmpty() || m_filter != QLatin1String("all");

    EffectsLibrary library;
    QVariantList leading;
    QVariantList custom;
    if (m_presets) {
        library = sharedEffectLibrary()->library();
        leading = m_presets->leadingGroups(q);
        custom = m_presets->customGroups(q);
    }
    QVariantList effects;
    if (m_controller && m_controller->effectControls())
        effects = m_controller->effectControls()->library(q);
    const std::set<std::string> favorites(library.favorites.begin(), library.favorites.end());

    const auto makeItem = [&](const QVariantMap& src, bool home) {
        QVariantMap row = itemRow(src);
        row.insert(QStringLiteral("home"), home);
        row.insert(QStringLiteral("favorite"), favorites.contains(str(row, "item").toStdString()));
        return row;
    };

    // Favorites, Presets and Lumetri Presets (EffectPresetsBridge), with the presets bins split into nested folders.
    for (const QVariant& g : leading) {
        const QVariantMap group = g.toMap();
        const QString kind = str(group, "kind");
        Node node;
        node.key = str(group, "key");
        node.folder = folderRow(str(group, "title"), str(group, "icon"), kind);
        const bool lumetri = kind == QLatin1String("lumetriPresets");
        if (kind == QLatin1String("presets")) {
            node.folder.insert(QStringLiteral("canHoldPresets"), true);
            node.folder.insert(QStringLiteral("userPresets"), static_cast<int>(library.presets.size()));
        }
        for (const QVariant& b : group.value(QStringLiteral("bins")).toList()) {
            const QVariantMap bin = b.toMap();
            const QString path = str(bin, "path");
            const bool builtIn = bin.value(QStringLiteral("builtIn")).toBool();
            Node* at = &node;
            if (kind != QLatin1String("favorites") && !path.isEmpty()) {
                QString walked;
                for (const QString& segment : path.split(QLatin1Char('/'))) {
                    const auto found = std::find_if(at->children.begin(), at->children.end(), [&](const Node& child) {
                        return child.folder.value(QStringLiteral("renamable")).toBool() == !builtIn
                            && str(child.folder, "title").compare(segment, Qt::CaseInsensitive) == 0;
                    });
                    if (found != at->children.end()) {
                        // Its path as first spelled: the key after "presets/u:" / "presets/b:" / "lumetriPresets/b:".
                        walked = found->key.mid(found->key.indexOf(QLatin1Char(':')) + 1);
                        at = &*found;
                        continue;
                    }
                    walked = walked.isEmpty() ? segment : walked + QLatin1Char('/') + segment;
                    Node child;
                    child.key = builtIn ? effectsBuiltInPresetsBinKey(walked, lumetri) : effectsPresetsBinKey(walked);
                    child.folder = folderRow(segment, QStringLiteral("folder"), QStringLiteral("presetsBin"));
                    if (!builtIn) {
                        child.folder.insert(QStringLiteral("presetsBin"), walked);
                        child.folder.insert(QStringLiteral("canHoldPresets"), true);
                        child.folder.insert(QStringLiteral("renamable"), true);
                        child.folder.insert(QStringLiteral("deletable"), true);
                        child.folder.insert(QStringLiteral("userPresets"), m_presets ? m_presets->userPresetsIn(walked) : 0);
                    }
                    at->children.push_back(std::move(child));
                    at = &at->children.back();
                }
            }
            for (const QVariant& i : bin.value(QStringLiteral("items")).toList()) {
                QVariantMap row = makeItem(i.toMap(), kind != QLatin1String("favorites"));
                if (str(row, "kind") == QLatin1String("preset") && !row.value(QStringLiteral("builtIn")).toBool())
                    row.insert(QStringLiteral("presetsBin"), str(bin, "presetsBin"));
                at->items.append(row);
            }
        }
        top.push_back(std::move(node));
    }

    // Premiere's effect folders, audio before video, each category a folder (EffectControlsModel::library).
    for (const char* wanted : {"audio", "audioTransition", "video", "videoTransition"}) {
        for (const QVariant& g : effects) {
            const QVariantMap group = g.toMap();
            if (str(group, "kind") != QLatin1String(wanted))
                continue;
            Node node;
            node.key = str(group, "key");
            node.folder = folderRow(str(group, "title"), str(group, "icon"), str(group, "kind"));
            for (const QVariant& b : group.value(QStringLiteral("bins")).toList()) {
                const QVariantMap bin = b.toMap();
                Node child;
                child.key = str(bin, "key");
                child.folder = folderRow(str(bin, "title"), QStringLiteral("folder"), QStringLiteral("category"));
                for (const QVariant& i : bin.value(QStringLiteral("items")).toList())
                    child.items.append(makeItem(i.toMap(), true));
                node.children.push_back(std::move(child));
            }
            top.push_back(std::move(node));
        }
    }

    addPlugins(top);

    // The custom bins, in the order they were made.
    for (const QVariant& g : custom) {
        const QVariantMap group = g.toMap();
        const QString name = str(group, "customBin");
        Node node;
        node.key = str(group, "key");
        node.folder = folderRow(str(group, "title"), str(group, "icon"), QStringLiteral("custom"));
        node.folder.insert(QStringLiteral("customBin"), name);
        for (const char* flag : {"canHoldItems", "renamable", "deletable"})
            node.folder.insert(QLatin1String(flag), true);
        for (const QVariant& b : group.value(QStringLiteral("bins")).toList()) {
            for (const QVariant& i : b.toMap().value(QStringLiteral("items")).toList()) {
                QVariantMap row = makeItem(i.toMap(), false);
                row.insert(QStringLiteral("customBin"), name);
                row.insert(QStringLiteral("removable"), true);
                node.items.append(row);
            }
        }
        top.push_back(std::move(node));
    }

    // The filter: folders ("favorites", "plugins") or kinds of items; a search or a filter hides empty folders.
    if (m_filter == QLatin1String("favorites") || m_filter == QLatin1String("plugins")) {
        std::erase_if(top, [&](const Node& n) { return n.key != m_filter; });
    }
    const std::function<int(Node&)> finish = [&](Node& n) {
        QVariantList kept;
        for (const QVariant& v : n.items) {
            if (keepItem(v.toMap()))
                kept.append(v);
        }
        n.items = kept;
        int count = static_cast<int>(n.items.size());
        for (Node& child : n.children)
            count += finish(child);
        if (narrowed)
            std::erase_if(n.children, [](const Node& c) { return c.folder.value(QStringLiteral("count")).toInt() == 0; });
        n.folder.insert(QStringLiteral("count"), count);
        return count;
    };
    for (Node& n : top)
        finish(n);
    if (narrowed)
        std::erase_if(top, [](const Node& n) { return n.folder.value(QStringLiteral("count")).toInt() == 0; });
    return top;
}

bool EffectsTreeModel::shownExpanded(const QString& key) const {
    return searching() ? !m_searchCollapsed.contains(key) : m_expanded.contains(key);
}

void EffectsTreeModel::flatten(const Node& node, int depth, const QString& parentKey, std::vector<QVariantMap>& rows) const {
    QVariantMap row = node.folder;
    const bool open = shownExpanded(node.key);
    row.insert(QStringLiteral("rowKey"), node.key);
    row.insert(QStringLiteral("depth"), depth);
    row.insert(QStringLiteral("parentKey"), parentKey);
    row.insert(QStringLiteral("expanded"), open);
    rows.push_back(std::move(row));
    if (!open)
        return;
    for (const Node& child : node.children)
        flatten(child, depth + 1, node.key, rows);
    // The folder's own items after its sub-folders: one row each, or tiles rows of `columns` items.
    QVariantList tiles;
    int tileRow = 0;
    const auto closeTiles = [&] {
        if (tiles.isEmpty())
            return;
        QVariantMap t = emptyRow();
        t.insert(QStringLiteral("rowKey"), node.key + QLatin1Char('#') + QString::number(tileRow++));
        t.insert(QStringLiteral("rowType"), QStringLiteral("tiles"));
        t.insert(QStringLiteral("depth"), depth + 1);
        t.insert(QStringLiteral("parentKey"), node.key);
        t.insert(QStringLiteral("tiles"), tiles);
        rows.push_back(std::move(t));
        tiles.clear();
    };
    for (const QVariant& v : node.items) {
        QVariantMap item = v.toMap();
        item.insert(QStringLiteral("rowKey"), node.key + QLatin1Char('|') + str(item, "itemId"));
        item.insert(QStringLiteral("depth"), depth + 1);
        item.insert(QStringLiteral("parentKey"), node.key);
        if (m_columns <= 1) {
            rows.push_back(std::move(item));
            continue;
        }
        tiles.append(item);
        if (tiles.size() >= m_columns)
            closeTiles();
    }
    closeTiles();
}

void EffectsTreeModel::rebuild() {
    if (m_deferred)
        return;
    m_tree = buildTree();
    std::vector<QVariantMap> rows;
    for (const Node& node : m_tree)
        flatten(node, 0, QString(), rows);
    applyRows(std::move(rows));
    // Matches: each item once, wherever it shows.
    QSet<QString> ids;
    const std::function<void(const Node&)> collect = [&](const Node& n) {
        for (const QVariant& v : n.items)
            ids.insert(str(v.toMap(), "itemId"));
        for (const Node& child : n.children)
            collect(child);
    };
    for (const Node& node : m_tree)
        collect(node);
    if (const int matches = static_cast<int>(ids.size()); matches != m_matchCount) {
        m_matchCount = matches;
        emit matchCountChanged();
    }
}

void EffectsTreeModel::refresh() { rebuild(); }

void EffectsTreeModel::applyRows(std::vector<QVariantMap> next) {
    const int before = rowCount();
    const auto keyOf = [](const QVariantMap& row) { return row.value(QStringLiteral("rowKey")).toString(); };
    QSet<QString> wanted;
    for (const QVariantMap& row : next)
        wanted.insert(keyOf(row));
    // 1. Rows that are gone, a run at a time from the bottom.
    for (int i = rowCount() - 1; i >= 0;) {
        if (wanted.contains(keyOf(m_rows[static_cast<std::size_t>(i)]))) {
            --i;
            continue;
        }
        int first = i;
        while (first > 0 && !wanted.contains(keyOf(m_rows[static_cast<std::size_t>(first - 1)])))
            --first;
        beginRemoveRows({}, first, i);
        m_rows.erase(m_rows.begin() + first, m_rows.begin() + i + 1);
        endRemoveRows();
        i = first - 1;
    }
    // 2. In order: keep, move or insert (a run of new rows at once), then update the data that changed.
    QSet<QString> present;
    for (const QVariantMap& row : m_rows)
        present.insert(keyOf(row));
    for (std::size_t i = 0; i < next.size(); ++i) {
        const QString key = keyOf(next[i]);
        const int at = static_cast<int>(i);
        if (i < m_rows.size() && keyOf(m_rows[i]) == key) {
            // in place
        } else if (present.contains(key)) {
            int from = -1;
            for (std::size_t j = i + 1; j < m_rows.size(); ++j) {
                if (keyOf(m_rows[j]) == key) {
                    from = static_cast<int>(j);
                    break;
                }
            }
            if (from < 0)
                continue; // cannot happen: every present key is in `next` once
            beginMoveRows({}, from, from, {}, at);
            QVariantMap moved = std::move(m_rows[static_cast<std::size_t>(from)]);
            m_rows.erase(m_rows.begin() + from);
            m_rows.insert(m_rows.begin() + at, std::move(moved));
            endMoveRows();
        } else {
            std::size_t end = i + 1;
            while (end < next.size() && !present.contains(keyOf(next[end])))
                ++end;
            beginInsertRows({}, at, static_cast<int>(end) - 1);
            m_rows.insert(m_rows.begin() + at, next.begin() + static_cast<std::ptrdiff_t>(i),
                          next.begin() + static_cast<std::ptrdiff_t>(end));
            endInsertRows();
            i = end - 1;
            continue;
        }
        if (m_rows[i] != next[i]) {
            m_rows[i] = std::move(next[i]);
            emit dataChanged(index(at), index(at));
        }
    }
    reindex();
    if (rowCount() != before)
        emit countChanged();
}

void EffectsTreeModel::reindex() {
    m_index.clear();
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        const QVariantMap& row = m_rows[i];
        m_index.insert(str(row, "rowKey"), static_cast<int>(i));
        if (str(row, "rowType") == QLatin1String("tiles")) {
            for (const QVariant& v : row.value(QStringLiteral("tiles")).toList())
                m_index.insert(str(v.toMap(), "rowKey"), static_cast<int>(i));
        }
    }
}

// ---- Expanded state ---------------------------------------------------------------------------------------------------------

void EffectsTreeModel::load() {
    m_expanded.clear();
    m_openedTop.clear();
    m_searchCollapsed.clear();
    QSettings settings;
    if (!m_persistent || !settings.contains(QLatin1String(kExpandedKey))) {
        m_expanded.insert(QStringLiteral("favorites")); // the first start: Favorites open, the rest closed
        return;
    }
    // Sub-folders as they were; of the top-level folders only Favorites (when it was open) and the last one opened.
    for (const QString& key : settings.value(QLatin1String(kExpandedKey)).toStringList()) {
        if (!isTopLevel(key) || key == QLatin1String("favorites"))
            m_expanded.insert(key);
    }
    const QString last = settings.value(QLatin1String(kLastOpenedKey)).toString();
    if (!last.isEmpty()) {
        m_expanded.insert(last);
        m_openedTop.append(last);
    }
}

void EffectsTreeModel::save() const {
    if (!m_persistent)
        return;
    QStringList keys(m_expanded.begin(), m_expanded.end());
    keys.sort();
    QSettings settings;
    settings.setValue(QLatin1String(kExpandedKey), keys);
    settings.setValue(QLatin1String(kLastOpenedKey), m_openedTop.isEmpty() ? QString() : m_openedTop.last());
}

void EffectsTreeModel::noteTopLevel(const QString& key, bool expanded) {
    if (!isTopLevel(key) || key == QLatin1String("favorites"))
        return;
    m_openedTop.removeAll(key);
    if (expanded)
        m_openedTop.append(key);
}

bool EffectsTreeModel::isExpanded(const QString& folderKey) const { return shownExpanded(folderKey); }

void EffectsTreeModel::setExpanded(const QString& folderKey, bool expanded) {
    if (folderKey.isEmpty())
        return;
    if (searching()) {
        if (expanded == !m_searchCollapsed.contains(folderKey))
            return;
        if (expanded)
            m_searchCollapsed.remove(folderKey);
        else
            m_searchCollapsed.insert(folderKey);
        rebuild();
        return;
    }
    if (expanded == m_expanded.contains(folderKey))
        return;
    if (expanded)
        m_expanded.insert(folderKey);
    else
        m_expanded.remove(folderKey);
    noteTopLevel(folderKey, expanded);
    save();
    rebuild();
}

void EffectsTreeModel::toggle(const QString& folderKey) { setExpanded(folderKey, !isExpanded(folderKey)); }

const EffectsTreeModel::Node* EffectsTreeModel::findNode(const QString& key, QStringList* ancestors) const {
    QStringList chain;
    const std::function<const Node*(const Node&)> visit = [&](const Node& n) -> const Node* {
        if (n.key == key)
            return &n;
        chain.append(n.key);
        for (const Node& child : n.children) {
            if (const Node* found = visit(child))
                return found;
        }
        chain.removeLast();
        return nullptr;
    };
    for (const Node& n : m_tree) {
        if (const Node* found = visit(n)) {
            if (ancestors)
                *ancestors = chain;
            return found;
        }
    }
    return nullptr;
}

void EffectsTreeModel::expandAll(const QString& folderKey) {
    QStringList keys;
    const std::function<void(const Node&)> collect = [&](const Node& n) {
        keys.append(n.key);
        for (const Node& child : n.children)
            collect(child);
    };
    if (folderKey.isEmpty()) {
        for (const Node& n : m_tree)
            collect(n);
    } else if (const Node* node = findNode(folderKey)) {
        collect(*node);
    }
    if (keys.isEmpty())
        return;
    for (const QString& key : keys) {
        if (searching())
            m_searchCollapsed.remove(key);
        else
            m_expanded.insert(key);
    }
    save();
    rebuild();
}

void EffectsTreeModel::collapseAll(const QString& folderKey) {
    QStringList keys;
    const std::function<void(const Node&)> collect = [&](const Node& n) {
        keys.append(n.key);
        for (const Node& child : n.children)
            collect(child);
    };
    if (folderKey.isEmpty()) {
        for (const Node& n : m_tree)
            collect(n);
    } else if (const Node* node = findNode(folderKey)) {
        collect(*node);
    }
    if (keys.isEmpty())
        return;
    for (const QString& key : keys) {
        if (searching()) {
            m_searchCollapsed.insert(key);
        } else {
            m_expanded.remove(key);
            noteTopLevel(key, false);
        }
    }
    save();
    rebuild();
}

int EffectsTreeModel::reveal(const QString& rowKey) {
    // The folders to open: a folder's ancestors, or an item's folder and its ancestors.
    QStringList open;
    if (!findNode(rowKey, &open)) {
        open.clear();
        bool found = false;
        const std::function<void(const Node&, QStringList)> visit = [&](const Node& n, QStringList chain) {
            if (found)
                return;
            chain.append(n.key);
            for (const QVariant& v : n.items) {
                if (n.key + QLatin1Char('|') + str(v.toMap(), "itemId") == rowKey) {
                    open = chain;
                    found = true;
                    return;
                }
            }
            for (const Node& child : n.children)
                visit(child, chain);
        };
        for (const Node& n : m_tree)
            visit(n, {});
        if (!found)
            return -1;
    }
    bool changed = false;
    for (const QString& key : open) {
        if (searching()) {
            changed = m_searchCollapsed.remove(key) || changed;
        } else if (!m_expanded.contains(key)) {
            m_expanded.insert(key);
            noteTopLevel(key, true);
            changed = true;
        }
    }
    if (changed) {
        save();
        rebuild();
    }
    return indexOf(rowKey);
}

int EffectsTreeModel::revealItem(const QString& itemId) {
    QString home;
    QString copy;
    const std::function<void(const Node&)> visit = [&](const Node& n) {
        for (const QVariant& v : n.items) {
            const QVariantMap item = v.toMap();
            if (str(item, "itemId") != itemId)
                continue;
            const QString key = n.key + QLatin1Char('|') + itemId;
            if (item.value(QStringLiteral("home")).toBool() && home.isEmpty())
                home = key;
            else if (copy.isEmpty())
                copy = key;
        }
        for (const Node& child : n.children)
            visit(child);
    };
    for (const Node& n : m_tree)
        visit(n);
    const QString key = !home.isEmpty() ? home : copy;
    return key.isEmpty() ? -1 : reveal(key);
}

void EffectsTreeModel::renameKeys(const QString& oldKey, const QString& newKey) {
    if (oldKey.isEmpty() || oldKey == newKey)
        return;
    const auto renamed = [&](const QString& key) {
        if (key == oldKey)
            return newKey;
        if (!oldKey.startsWith(QLatin1String("custom:")) && key.startsWith(oldKey + QLatin1Char('/')))
            return newKey + key.mid(oldKey.size());
        return key;
    };
    QSet<QString> expanded;
    for (const QString& key : std::as_const(m_expanded))
        expanded.insert(renamed(key));
    m_expanded = expanded;
    QSet<QString> collapsed;
    for (const QString& key : std::as_const(m_searchCollapsed))
        collapsed.insert(renamed(key));
    m_searchCollapsed = collapsed;
    for (QString& key : m_openedTop)
        key = renamed(key);
    save();
    rebuild();
}

// ---- Applying -----------------------------------------------------------------------------------------------------------

QVariantMap EffectsTreeModel::applyToSelection(const QString& itemId) {
    const auto failed = [](const QString& error) {
        return QVariantMap{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"), error},
                           {QStringLiteral("message"), error},
                           {QStringLiteral("clips"), 0}};
    };
    QString id = itemId.trimmed();
    if (id.startsWith(QLatin1String("effect:")))
        id = id.mid(7);
    if (id.startsWith(QLatin1String("plugin."))) {
        if (!m_plugins)
            return failed(tx("Audio plugins are not available here"));
        return m_plugins->applyToSelectedClips(id);
    }
    EffectControlsModel* controls = m_controller ? m_controller->effectControls() : nullptr;
    if (!controls)
        return failed(tx("No project"));
    return controls->applyToSelectedClips(id);
}

} // namespace wabcut::app
