#pragma once

// EffectPresetsBridge: the Effects panel's presets, custom bins and favourites for QML (singleton `EffectPresets`, 0.2,
// docs/effects.md "Presets, custom bins and favourites").
//
// The library is the user's (Premiere keeps presets with the user, not the project): one EffectLibraryServices for the
// whole app (sharedLibrary(): the WABCUT_EFFECTS_LIBRARY file, else "effects-library.json" in the app's data folder),
// which the assistant's engine and the timeline's drops use too. Every change goes through the effects.* commands
// (CatalogEffects.cpp), so the panel, the CLI, MCP and the assistant behave the same: saving a preset or a bin is not an
// undo step, applying a preset is one.
//
// The Effects panel lists leadingGroups() (Favorites, Presets with its bins, Lumetri Presets) before Premiere's effect
// groups (EffectControlsModel::library) and customGroups() (the custom bins) after them; items have the same shape as
// the effect items, with "preset:<name>" ids for presets (dragged with the same mime type: TimelineView::planDrop and
// EffectControlsModel::addEffect apply them).
//
// UX rework: groups and bins carry a stable untranslated `key` (effectsCustomBinKey / effectsPresetsBinKey /
// effectsBuiltInPresetsBinKey), which EffectsTreeModel (the panel's folder tree) uses for expanded state; Import
// Presets can target a bin, a single presets bin can be exported, the file dialogs remember their folder
// (presetsFolder), and the panel hears about changes made elsewhere (the keymap's New Custom Bin / Delete Custom Item)
// through customBinCreated, deleteCurrentItemRequested and statusMessage.

#include <wabcut/model/EffectPresets.h>
#include <wabcut/model/Ids.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::command {
class EffectLibraryServices;
}

namespace wabcut::app {

class ProjectController;

/// The app's effects library (created on first use).
std::shared_ptr<command::EffectLibraryServices> sharedEffectLibrary();
/// Applies preset `name` (built-in or the user's) to `clips` as one undo step ("Apply Preset"): "" or why not.
QString applyEffectPresetToClips(ProjectController& controller, const std::vector<ClipId>& clips, const QString& name);
/// The preset an Effects panel item id names ("preset:<name>"), for the current sequence's frame; nullopt otherwise.
std::optional<EffectPreset> effectPresetForItem(ProjectController* controller, const QString& itemId);

// UX rework: the Effects panel's stable folder keys (untranslated), shared by the bridges' `key` fields and
// EffectsTreeModel. Top-level: "favorites", "presets", "lumetriPresets", "audio", "audioTransition", "video",
// "videoTransition", "plugins", and "custom:<name>" per custom bin.
/// A custom bin: "custom:<name>".
QString effectsCustomBinKey(const QString& name);
/// One of the user's presets bins: "presets/u:<path>"; "" (the Presets bin itself) is "presets".
QString effectsPresetsBinKey(const QString& path);
/// A built-in presets bin: "presets/b:<path>", or "lumetriPresets/b:<path>" for the Lumetri looks' bins.
QString effectsBuiltInPresetsBinKey(const QString& path, bool lumetri);

class EffectPresetsBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(EffectPresets)
    QML_SINGLETON

    /// Bumped on every library change: bind lists to it.
    Q_PROPERTY(int revision READ revision NOTIFY changed FINAL)
    /// The user's custom bins and presets bins, by name / path.
    Q_PROPERTY(QStringList customBins READ customBins NOTIFY changed FINAL)
    Q_PROPERTY(QStringList presetBins READ presetBins NOTIFY changed FINAL)
    /// The Effects panel's current item reference ("effect:<id>", "preset:<name>") for Delete Custom Item.
    Q_PROPERTY(QString currentItem READ currentItem WRITE setCurrentItem NOTIFY currentItemChanged FINAL)
    /// The custom bin the current item is in ("" when it is not in one).
    Q_PROPERTY(QString currentBin READ currentBin WRITE setCurrentBin NOTIFY currentItemChanged FINAL)
    /// Why the library file could not be read or written, or "".
    Q_PROPERTY(QString libraryError READ libraryError NOTIFY changed FINAL)
    /// UX rework: the error of the last library call ("" after one that worked): why newCustomBin() returned "".
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged FINAL)
    /// UX rework: how many presets the user has (Export Presets... needs one).
    Q_PROPERTY(int userPresetCount READ userPresetCount NOTIFY changed FINAL)
    /// UX rework: the folder the presets file dialogs open in: the last one presets were imported from or exported to
    /// (QSettings "effects/presetsFolder"), else the Documents folder.
    Q_PROPERTY(QUrl presetsFolder READ presetsFolder NOTIFY presetsFolderChanged FINAL)
    /// UX rework: the presets the last import added, by their names in the library ("Glow 2" when "Glow" was taken).
    Q_PROPERTY(QStringList lastImported READ lastImported NOTIFY lastImportedChanged FINAL)

public:
    explicit EffectPresetsBridge(QObject* parent = nullptr);
    ~EffectPresetsBridge() override;
    static EffectPresetsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);

    int revision() const { return m_revision; }
    QStringList customBins() const;
    QStringList presetBins() const;
    QString currentItem() const { return m_currentItem; }
    void setCurrentItem(const QString& item);
    QString currentBin() const { return m_currentBin; }
    void setCurrentBin(const QString& bin);
    QString libraryError() const;
    QString lastError() const { return m_lastError; }
    int userPresetCount() const;
    QUrl presetsFolder() const;
    QStringList lastImported() const { return m_lastImported; }

    // ---- Stable folder keys (untranslated; EffectsTreeModel and the panel's expanded state) ----
    /// "custom:<name>" (effectsCustomBinKey).
    Q_INVOKABLE QString customBinKey(const QString& name) const;
    /// One of the user's presets bins: "presets/u:<path>" ("A/B" -> "presets/u:A/B"); "" is the Presets bin itself
    /// ("presets") (effectsPresetsBinKey).
    Q_INVOKABLE QString presetsBinKey(const QString& path) const;

    /// Groups listed before the effects: Favorites (when there are any), Presets (built-in bins, then the user's
    /// presets bins), Lumetri Presets; [{title, icon, kind, key, count, bins: [{title, key, path, builtIn, count,
    /// presetsBin?, items}]}], filtered by `query`. UX rework: `key` is stable ("favorites", "presets",
    /// "lumetriPresets"; a bin's: effectsPresetsBinKey / effectsBuiltInPresetsBinKey; the Presets bin's own presets
    /// and the Favorites list have their group's key), `path` the bin's untranslated path ("" for those two),
    /// `builtIn` whether it is one of Wabcut's bins, `count` the number of items. The user's bins whose paths differ
    /// only in case are one bin.
    Q_INVOKABLE QVariantList leadingGroups(const QString& query) const;
    /// The custom bins, one group each: [{title, icon, kind: "custom", key (customBinKey), count, customBin,
    /// bins: [{title: "", key, count, items}]}].
    Q_INVOKABLE QVariantList customGroups(const QString& query) const;
    Q_INVOKABLE bool isFavorite(const QString& item) const;
    /// A preset's properties: {name, type, typeName, description, bin, builtIn, kind}; {} when there is none.
    Q_INVOKABLE QVariantMap preset(const QString& name) const;
    /// The item reference of an Effects panel item id: "preset:<name>" stays, an effect id becomes "effect:<id>".
    Q_INVOKABLE QString itemReference(const QString& itemId) const;

    // ---- Applying ----
    /// Applies a preset to the selected clips (double-click or Enter in the Effects panel). "" or why not.
    Q_INVOKABLE QString applyToSelection(const QString& name);

    // ---- Save Preset (Effect Controls) ----
    /// Effect Controls sections of the shown clip ("motion", "opacity", "audio", "e12"): every one of the clip
    /// `section` belongs to, for "Save All as Preset".
    Q_INVOKABLE QStringList sectionsOfClip(const QString& section) const;
    /// Saves the sections (one clip's) as a preset: "scale", "anchorToInPoint" or "anchorToOutPoint". "" or why not.
    Q_INVOKABLE QString savePreset(const QStringList& sections, const QString& name, const QString& type,
                                   const QString& description, const QString& bin);
    /// Saves one effect of a clip (by clip key and effect instance id) as a preset: the Color panel's Save Preset of the
    /// grade (Lumetri's). "" or why not.
    Q_INVOKABLE QString saveEffectOfClip(const QString& clip, const QString& effect, const QString& name, const QString& type,
                                         const QString& description, const QString& bin);
    /// A free preset name based on `base` ("Gaussian Blur", "Gaussian Blur 2", ...).
    Q_INVOKABLE QString suggestName(const QString& base) const;
    /// The display name of a section's effect ("Motion", "Gaussian Blur").
    Q_INVOKABLE QString sectionTitle(const QString& section) const;

    // ---- Library items ----
    Q_INVOKABLE QString setPresetProperties(const QString& preset, const QString& name, const QString& type,
                                            const QString& description);
    Q_INVOKABLE QString movePreset(const QString& preset, const QString& bin);
    /// New custom bin (Premiere's name: "Custom Bin 01"...), with `item` in it when given. Returns its name, or ""
    /// (lastError says why). UX rework: emits customBinCreated(name), or statusMessage(error).
    Q_INVOKABLE QString newCustomBin(const QString& item = {});
    Q_INVOKABLE QString newPresetsBin(const QString& path);
    Q_INVOKABLE QString addToCustomBin(const QString& bin, const QString& item);
    /// UX rework: emits binRenamed(customBinKey(bin), customBinKey(new name)) when it worked.
    Q_INVOKABLE QString renameCustomBin(const QString& bin, const QString& name);
    /// UX rework: emits binRenamed(presetsBinKey(path), presetsBinKey(new path)) when it worked.
    Q_INVOKABLE QString renamePresetsBin(const QString& path, const QString& name);
    /// Delete Custom Item: {"preset": name} | {"presetsBin": path} | {"customBin": name[, "item": reference]}.
    Q_INVOKABLE QString deleteCustomItem(const QVariantMap& what);
    /// The keymap's Delete Custom Item: the current item (out of its custom bin, or a user preset). UX rework: the
    /// current item and bin are cleared when it worked.
    Q_INVOKABLE QString deleteCurrentItem();
    /// UX rework: what deleteCurrentItem() would do, for the panel's confirmation: { item, bin, name, deletable,
    /// action ("removeFromBin" | "deletePreset" | ""), question (the dialog's text), confirmText ("Remove" / "Delete"),
    /// reason ("" or why it cannot be deleted) }.
    Q_INVOKABLE QVariantMap currentItemInfo() const;
    /// UX rework (the keymap's Delete Custom Item, Backspace): asks the panel to confirm: emits
    /// deleteCurrentItemRequested(currentItemInfo()) when the current item can be deleted, else statusMessage(reason).
    /// Deletes nothing itself.
    Q_INVOKABLE void requestDeleteCurrentItem();
    Q_INVOKABLE QString setFavorite(const QString& item, bool favorite);
    /// Import Presets... UX rework: `bin` omitted / undefined / null keeps the bins the presets were exported from; a
    /// string puts every imported preset in that presets bin ("" = the Presets bin itself, "A/B" a bin inside a bin).
    /// Presets exported from the built-in Lumetri bins land in a bin of the same name ("Filmstock"). On success the
    /// file's folder becomes presetsFolder and lastImported lists the new names.
    Q_INVOKABLE QString importPresets(const QUrl& file, const QVariant& bin = QVariant());
    /// Writes presets to a file; `names` empty = every user preset. UX rework: a file name without a ".json" ending
    /// gets ".wabcut-presets.json" (and is then never overwritten silently); the file's folder becomes presetsFolder.
    Q_INVOKABLE QString exportPresets(const QUrl& file, const QStringList& names);
    /// UX rework (a presets bin's Export Presets...): every preset of the user's in presets bin `bin` and the bins inside
    /// it ("" = all of them). "" or why not (an empty bin says so).
    Q_INVOKABLE QString exportPresetsBin(const QUrl& file, const QString& bin);
    /// UX rework: how many of the user's presets are in presets bin `bin` and the bins inside it ("" = all).
    Q_INVOKABLE int userPresetsIn(const QString& bin) const;
    /// UX rework: the export dialog's file name for `name` (a preset or a bin path: its last segment), characters a file
    /// name cannot hold replaced: "Lower thirds/Blue" -> "Blue.wabcut-presets.json"; "" -> "Presets.wabcut-presets.json".
    Q_INVOKABLE QString presetsFileName(const QString& name) const;
    /// UX rework: presetsFolder + presetsFileName(name), for the save dialog's selectedFile.
    Q_INVOKABLE QUrl presetsFileUrl(const QString& name) const;
    /// UX rework: whether a dropped file is a presets file (".wabcut-presets.json", or any ".json": the import checks it).
    Q_INVOKABLE bool isPresetsFile(const QUrl& file) const;

    /// Tests: whether presetsFolder is kept in QSettings (default true).
    void setPersistent(bool persistent) { m_persistent = persistent; }

signals:
    void changed();
    void currentItemChanged();
    void lastErrorChanged();
    void presetsFolderChanged();
    void lastImportedChanged();
    /// UX rework: a custom bin was made (by any caller: the panel, its menus, the keymap's Ctrl+/).
    void customBinCreated(const QString& name);
    /// UX rework: a custom bin or presets bin was renamed: the folder keys before and after (expanded state follows).
    void binRenamed(const QString& oldKey, const QString& newKey);
    /// UX rework: requestDeleteCurrentItem() wants a confirmation (the panel asks, then calls deleteCurrentItem()).
    void deleteCurrentItemRequested(const QVariantMap& info);
    /// UX rework: a message for the Effects panel's status line from a call the panel did not make (the keymap's).
    void statusMessage(const QString& text);

private:
    /// Runs an effects.* command with the app's library; "" or the error message. `data` gets the command's data.
    QString run(const std::string& name, nlohmann::json params, nlohmann::json* data = nullptr);
    void noteChange();
    void setLastError(const QString& error);
    /// A local file path for an export: `file` with ".wabcut-presets.json" when it has no ".json" ending (`renamed`).
    QString exportPath(const QUrl& file, bool* renamed) const;
    void rememberFolder(const QString& filePath);

    QPointer<ProjectController> m_controller;
    std::shared_ptr<command::EffectLibraryServices> m_library;
    std::uint64_t m_seen = 0;
    int m_revision = 0;
    QString m_currentItem;
    QString m_currentBin;
    QString m_lastError;
    QStringList m_lastImported;
    QString m_folder; ///< presetsFolder when not persistent (tests)
    bool m_persistent = true;
};

} // namespace wabcut::app
