#pragma once

// OpenFX video plug-ins in the UI (0.7, docs/plugins.md, "OpenFX video plug-ins"): what the Effects panel lists, what an
// effect's badge says in Effect Controls, and putting a plug-in on the selected clips.
//
// The plug-in support is ofxhost::OfxSystem::current() (the app starts it; wabcut-cli and the tests may not): without it
// (a build without OpenFX, or tests that did not start one) `available()` is false and every function answers "nothing".
// A plug-in's effect is an ordinary effect of the clip's stack (id "plugin.ofx.<plug-in id>"): its parameters are Effect
// Controls rows like any effect's (EffectControls.cpp), edits are the model's one-undo-step edits, and the project saves it
// like any effect. A plug-in the computer does not have is an unknown effect: kept, bypassed, badged "Missing".
//
// Principle 9: QML sees keys and display text.

#include <wabcut/model/Effects.h>
#include <wabcut/model/Ids.h>

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <memory>

namespace wabcut::ofxhost {
class OfxCatalog;
class OfxEffects;
}

namespace wabcut::app {

class ProjectController;

class OfxUi : public QObject {
    Q_OBJECT

public:
    static OfxUi& instance();

    /// Tests: the catalog and effects to use instead of ofxhost::OfxSystem::current() (null: back to it).
    void setSources(std::shared_ptr<ofxhost::OfxCatalog> catalog, std::shared_ptr<ofxhost::OfxEffects> effects);
    /// OpenFX support runs in this process.
    bool available() const;
    /// The usable plug-ins by name, as rows the Effects panel lists: { id ("plugin.ofx.<id>"), name, vendor (the group the
    /// plug-in files itself under), category, format ("OpenFX"), version, latencyText (""), hasEditor (false), problem ("") }.
    QVariantList plugins() const;
    /// Plug-ins found that cannot be used here, with the reason: [{ name, problem }].
    QVariantList unusable() const;
    /// Binaries that crashed or hung while scanned: [{ file, name, reason }].
    QVariantList blocked() const;
    /// What an effect is, in PluginBridge::effectInfo's shape: { plugin, openfx, name, vendor, format, hasEditor, kind ("" |
    /// "missing" | "crashed" | "failed" | "hung"), text (the badge's), missedBlocks } ; plugin false for other effects.
    QVariantMap effectInfo(const EffectInstance& effect) const;
    /// Takes a crashed, hung or failed effect's badge off and tries it again (turning the effect off and on does this).
    void retry(const EffectInstance& effect) const;
    /// Scans the plug-in folders again on a worker (`all`: every binary). "" when started, else why not.
    QString rescan(bool all = false);
    /// Adds the plug-in (an effect id) to every selected video clip, one instance each, as one undo step ("Add Plug-in").
    /// Returns { ok, error ("" or why not), message, clips }.
    QVariantMap applyToSelectedClips(ProjectController* controller, const QString& effectId) const;
    /// Adds the plug-in to one video clip (a drop on the timeline): one undo step. "" or why not.
    QString applyToClip(ProjectController* controller, const QString& effectId, ClipId clip) const;
    /// Bumped when a badge or the list of plug-ins may have changed.
    std::uint64_t revision() const;

signals:
    /// The plug-ins known changed (a scan finished), or an effect's status did (a crash, a retry). Emitted on the GUI thread.
    void changed();

private:
    OfxUi();
    struct Sources {
        std::shared_ptr<ofxhost::OfxCatalog> catalog;
        std::shared_ptr<ofxhost::OfxEffects> effects;
    };
    Sources sources() const;
    void listen() const;
    mutable bool m_listening = false;
    std::shared_ptr<ofxhost::OfxCatalog> m_catalog; ///< tests
    std::shared_ptr<ofxhost::OfxEffects> m_effects;
};

} // namespace wabcut::app
