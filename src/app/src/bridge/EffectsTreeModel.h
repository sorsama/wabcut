#pragma once

// EffectsTreeModel (UX rework, QML element `EffectsTree`): the Effects panel's folder tree as one flat list of the rows
// that show, for a virtualized ListView (docs/effects.md "The Effects panel").
//
// Folders, in Premiere's order: Favorites (when there are some), Presets, Lumetri Presets, Audio Effects, Audio
// Transitions, Video Effects, Video Transitions, Audio Plug-ins (PluginBridge's catalog, its vendor or category groups
// as sub-folders), then each custom bin. Sub-folders: the effect categories (one folder each, never split on "/":
// "Noise Reduction/Restoration" is one name) and the presets bins (their paths split on "/" into nested folders:
// "PiPs/25%" is PiPs > 25%). The data comes from EffectPresetsBridge::leadingGroups / customGroups,
// EffectControlsModel::library and PluginBridge::allPlugins; each folder has a stable untranslated key
// (EffectPresetsBridge.h: effectsCustomBinKey...), which the expanded state is kept by.
//
// Expanded state: per folder key, kept in QSettings ("effects/expandedFolders", "effects/lastOpenedFolder"). At start
// the top-level folders are closed except Favorites (unless the user closed it) and the top-level folder the user opened
// last; sub-folders keep their state. A search (`query`) shows every folder with a match open (closing one lasts until
// the query changes) and hides the others; clearing it brings the user's state back. A `filter` keeps the items of one
// kind. With `columns` > 1 (the Icons view) the items of a folder come as "tiles" rows of up to `columns` items.
//
// Rows are updated in place by key (inserted, removed, moved, changed), never reset, so the ListView keeps its
// delegates, focus and scroll position when the library changes.

#include <QAbstractListModel>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlparserstatus.h>
#include <QtQml/qqmlregistration.h>

#include <vector>

namespace wabcut::app {

class EffectPresetsBridge;
class PluginBridge;
class ProjectController;

class EffectsTreeModel : public QAbstractListModel, public QQmlParserStatus {
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    QML_NAMED_ELEMENT(EffectsTree)

    /// The search: fuzzy, over names and categories (plug-ins: name, vendor, category). "" shows the user's tree.
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged FINAL)
    /// Whether `query` holds a search (not only spaces).
    Q_PROPERTY(bool searching READ searching NOTIFY queryChanged FINAL)
    /// "all" (default), "video" (video effects and transitions, video presets), "audio" (audio effects and transitions,
    /// audio presets, plug-ins), "transitions", "presets", "favorites" (the Favorites folder only) or "plugins" (the
    /// Audio Plug-ins folder only). Unknown values are "all".
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged FINAL)
    /// The Audio Plug-ins folder shows (when plug-ins are available). Default true.
    Q_PROPERTY(bool showPlugins READ showPlugins WRITE setShowPlugins NOTIFY showPluginsChanged FINAL)
    /// Items per row: 1 (default; the List and Compact views: one "item" row each) or more (the Icons view: "tiles" rows).
    Q_PROPERTY(int columns READ columns WRITE setColumns NOTIFY columnsChanged FINAL)
    /// Rows that show.
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged FINAL)
    /// Effects, transitions, presets and plug-ins that match the query and filter (each once, open folders or not).
    Q_PROPERTY(int matchCount READ matchCount NOTIFY matchCountChanged FINAL)

public:
    /// Roles (QML names in roleNames()). Every row has every role (empty / false / 0 where it does not apply).
    enum Role {
        RowKeyRole = Qt::UserRole + 1, ///< "rowKey": unique, stable: a folder's key, "<folderKey>|<itemId>", "<folderKey>#<n>"
        RowTypeRole,       ///< "rowType": "folder" | "item" | "tiles"
        DepthRole,         ///< "depth": 0 for the top-level folders
        ParentKeyRole,     ///< "parentKey": the folder it is in ("" at the top)
        TitleRole,         ///< "title": the folder's name or the item's
        IconRole,          ///< "icon": a top-level folder's symbol ("bookmark", "layers"...), "folder" for sub-folders, an item's category icon
        CountRole,         ///< "count": folders: the items inside (sub-folders included)
        ExpandedRole,      ///< "expanded": folders: open (as shown: a search opens every folder with a match)
        FolderKindRole,    ///< "folderKind": "favorites" | "presets" | "lumetriPresets" | "audio" | "audioTransition" | "video" | "videoTransition" | "plugins" | "custom" | "category" | "presetsBin" | "pluginGroup"
        ItemIdRole,        ///< "itemId": the id dragged (mime application/x-wabcut-effect) and applied: "wabcut.blur.gaussian", "preset:Twirl In", "plugin.clap.<id>"
        ItemRole,          ///< "item": the reference custom bins and Favorites use: "effect:<id>" | "preset:<name>"
        NameRole,          ///< "name"
        CategoryRole,      ///< "category": the effect's category, a preset's bin, a plug-in's category
        DescriptionRole,   ///< "description"
        KindRole,          ///< "kind": items: "video" | "audio" | "videoTransition" | "audioTransition" | "preset"
        PresetKindRole,    ///< "presetKind": presets: "video" | "audio"
        PresetTypeRole,    ///< "presetType": presets: "Scale" | "Anchor to In Point" | "Anchor to Out Point"
        BuiltInRole,       ///< "builtIn": a preset Wabcut ships
        PluginRole,        ///< "plugin": an audio plug-in (applied through Plugins)
        VendorRole,        ///< "vendor": plug-ins
        FormatRole,        ///< "format": plug-ins: "CLAP" | "VST3"
        ProblemRole,       ///< "problem": plug-ins: why it cannot be used ("" when it can; the row is disabled otherwise)
        FavoriteRole,      ///< "favorite": the item is in Favorites
        CustomBinRole,     ///< "customBin": a custom bin's folder: its name; an item in a custom bin: that bin
        PresetsBinRole,    ///< "presetsBin": a folder that holds the user's presets: its bin path ("" = the Presets folder); a user preset: its bin
        CanHoldItemsRole,  ///< "canHoldItems": a custom bin (drop effects, transitions, presets, plug-ins on it: addToCustomBin)
        CanHoldPresetsRole, ///< "canHoldPresets": the Presets folder or one of the user's presets bins (drop a user preset: movePreset)
        RenamableRole,     ///< "renamable": a custom bin or one of the user's presets bins
        DeletableRole,     ///< "deletable": folders: custom bins and the user's presets bins; items: the user's presets
        RemovableRole,     ///< "removable": an item in a custom bin (Remove from <bin>)
        UserPresetsRole,   ///< "userPresets": folders: the user's presets inside (the Presets folder: all of them)
        HomeRole,          ///< "home": the item's own place (its category, presets bin or plug-in group), not a Favorites / custom-bin copy
        TilesRole,         ///< "tiles": "tiles" rows: the item rows in it (maps with every role)
    };
    Q_ENUM(Role)

    explicit EffectsTreeModel(QObject* parent = nullptr);
    ~EffectsTreeModel() override;

    /// Where the rows come from (tests; in QML the singletons are found when the component completes).
    void setSources(ProjectController* controller, EffectPresetsBridge* presets, PluginBridge* plugins);
    /// Tests: whether the expanded state is read from and kept in QSettings (default true).
    void setPersistent(bool persistent);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void classBegin() override;
    void componentComplete() override;

    QString query() const { return m_query; }
    void setQuery(const QString& query);
    bool searching() const { return !m_query.trimmed().isEmpty(); }
    QString filter() const { return m_filter; }
    void setFilter(const QString& filter);
    bool showPlugins() const { return m_showPlugins; }
    void setShowPlugins(bool show);
    int columns() const { return m_columns; }
    void setColumns(int columns);
    int matchCount() const { return m_matchCount; }

    /// The row at `row` as a map of every role ({} out of range).
    Q_INVOKABLE QVariantMap get(int row) const;
    /// The row of a row key (a folder key, "<folderKey>|<itemId>"; in the Icons view an item's tiles row), or -1.
    Q_INVOKABLE int indexOf(const QString& rowKey) const;
    /// The first row showing item `itemId` (its own place first, then Favorites / custom bins), or -1.
    Q_INVOKABLE int indexOfItem(const QString& itemId) const;
    /// The row of the folder `row` is in, or -1.
    Q_INVOKABLE int parentIndex(int row) const;
    /// Whether a folder shows open.
    Q_INVOKABLE bool isExpanded(const QString& folderKey) const;
    /// Opens or closes a folder (kept; during a search only until the query changes).
    Q_INVOKABLE void setExpanded(const QString& folderKey, bool expanded);
    Q_INVOKABLE void toggle(const QString& folderKey);
    /// Opens every folder ("") or a folder and every folder inside it.
    Q_INVOKABLE void expandAll(const QString& folderKey = QString());
    /// Closes every folder ("") or a folder and every folder inside it.
    Q_INVOKABLE void collapseAll(const QString& folderKey = QString());
    /// Opens the folders around a row key (a folder's or "<folderKey>|<itemId>") so it shows; returns its row, or -1.
    Q_INVOKABLE int reveal(const QString& rowKey);
    /// Opens the folders around item `itemId`'s own place (e.g. a preset just imported: "preset:Glow 2"); its row or -1.
    Q_INVOKABLE int revealItem(const QString& itemId);
    /// The panel's double-click / Enter: plug-ins through PluginBridge::applyToSelectedClips, everything else through
    /// EffectControlsModel::applyToSelectedClips (every selected clip, one undo step). { ok, error, message, clips }.
    Q_INVOKABLE QVariantMap applyToSelection(const QString& itemId);
    /// Reads the library again (the bridges' change signals do this already).
    Q_INVOKABLE void refresh();

signals:
    void queryChanged();
    void filterChanged();
    void showPluginsChanged();
    void columnsChanged();
    void countChanged();
    void matchCountChanged();

private:
    struct Node {
        QString key;
        QVariantMap folder; ///< the folder row's own roles (title, icon, folderKind, customBin...)
        std::vector<Node> children;
        QVariantList items; ///< item rows' roles (rowKey, depth and parentKey are set when flattened)
    };

    std::vector<Node> buildTree() const;
    void addPlugins(std::vector<Node>& top) const;
    void flatten(const Node& node, int depth, const QString& parentKey, std::vector<QVariantMap>& rows) const;
    bool shownExpanded(const QString& key) const;
    bool keepItem(const QVariantMap& item) const;
    /// The node of `key` in m_tree and the keys of the folders around it (outermost first); nullptr when none.
    const Node* findNode(const QString& key, QStringList* ancestors = nullptr) const;
    void rebuild();
    void applyRows(std::vector<QVariantMap> next);
    void reindex();
    void load();
    void save() const;
    void noteTopLevel(const QString& key, bool expanded);
    void renameKeys(const QString& oldKey, const QString& newKey);

    QPointer<ProjectController> m_controller;
    QPointer<EffectPresetsBridge> m_presets;
    QPointer<PluginBridge> m_plugins;
    bool m_deferred = false; ///< created by QML: rows are built when the component completes
    bool m_persistent = true;

    QString m_query;
    QString m_filter = QStringLiteral("all");
    bool m_showPlugins = true;
    int m_columns = 1;
    int m_matchCount = 0;

    QSet<QString> m_expanded;        ///< the user's open folders
    QStringList m_openedTop;         ///< top-level folders in the order they were opened (the last one reopens at start)
    QSet<QString> m_searchCollapsed; ///< folders closed during the current search

    std::vector<Node> m_tree;
    std::vector<QVariantMap> m_rows;
    QHash<QString, int> m_index; ///< row key (and, in the Icons view, item row keys) -> row
};

} // namespace wabcut::app
