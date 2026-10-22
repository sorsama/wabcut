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

    /// Groups listed before the effects: Favorites (when there are any), Presets (built-in bins, then the user's
    /// presets bins), Lumetri Presets; [{title, icon, kind, bins: [{title, presetsBin?, items}]}], filtered by `query`.
    Q_INVOKABLE QVariantList leadingGroups(const QString& query) const;
    /// The custom bins, one group each: [{title, icon, kind: "custom", customBin, bins: [{title: "", items}]}].
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
    /// New custom bin (Premiere's name: "Custom Bin 01"...), with `item` in it when given. Returns its name, or "".
    Q_INVOKABLE QString newCustomBin(const QString& item = {});
    Q_INVOKABLE QString newPresetsBin(const QString& path);
    Q_INVOKABLE QString addToCustomBin(const QString& bin, const QString& item);
    Q_INVOKABLE QString renameCustomBin(const QString& bin, const QString& name);
    Q_INVOKABLE QString renamePresetsBin(const QString& path, const QString& name);
    /// Delete Custom Item: {"preset": name} | {"presetsBin": path} | {"customBin": name[, "item": reference]}.
    Q_INVOKABLE QString deleteCustomItem(const QVariantMap& what);
    /// The keymap's Delete Custom Item: the current item (out of its custom bin, or a user preset).
    Q_INVOKABLE QString deleteCurrentItem();
    Q_INVOKABLE QString setFavorite(const QString& item, bool favorite);
    Q_INVOKABLE QString importPresets(const QUrl& file);
    /// Writes presets to a file; `names` empty = every user preset.
    Q_INVOKABLE QString exportPresets(const QUrl& file, const QStringList& names);

signals:
    void changed();
    void currentItemChanged();

private:
    /// Runs an effects.* command with the app's library; "" or the error message. `data` gets the command's data.
    QString run(const std::string& name, nlohmann::json params, nlohmann::json* data = nullptr);
    void noteChange();

    QPointer<ProjectController> m_controller;
    std::shared_ptr<command::EffectLibraryServices> m_library;
    std::uint64_t m_seen = 0;
    int m_revision = 0;
    QString m_currentItem;
    QString m_currentBin;
};

} // namespace wabcut::app
