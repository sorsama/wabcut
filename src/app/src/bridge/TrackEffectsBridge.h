#pragma once

// TrackEffectsBridge: effects on a whole video track (0.7, card #139; singleton `TrackEffects`, docs/effects.md "Effects
// on a whole video track").
//
// A video track has an effect stack of its own (Track::effects) that applies to the composite of every clip on the track,
// after the clips' own effects: a colour grade or blur for all of V2 at once, without an adjustment layer and without
// touching each clip. The timeline's track header menu (Video Track Effects...) opens TrackEffectsPopover, which uses this
// bridge to list the stack, add / remove / bypass / reorder effects and edit their settings. Every change is ONE undo step
// through the same model operations as the track.addEffect family of commands (EffectTarget::ofTrack); a slider drag or
// a scrub merges into one step until endEffectGesture(). Keyframe times on a track's effects are timeline times: an
// animated setting sets its key at the playhead.
//
// Tracks and effects are named by the bridge keys ("t12", "e5": BridgeIds.h).

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class ProjectController;

class TrackEffectsBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(TrackEffects)
    QML_SINGLETON

public:
    explicit TrackEffectsBridge(QObject* parent = nullptr);
    static TrackEffectsBridge* create(QQmlEngine*, QJSEngine*);

    void setController(ProjectController* controller) { m_controller = controller; }

    /// How many effects the video track has (0 for another kind of track or an unknown one).
    Q_INVOKABLE int count(const QString& track) const;
    /// The track's stack, bottom to top: [{ key ("e5"), effect (id), name, enabled, known (this version can run it),
    /// animated (some setting has keys), summary (the first settings, "Radius 12 px") }].
    Q_INVOKABLE QVariantList effectsOf(const QString& track) const;
    /// The video effects that can go on a track and match `filter` (a case-insensitive part of the name or category):
    /// [{ id, name, category, description }], by category then name.
    Q_INVOKABLE QVariantList available(const QString& filter) const;
    /// "" when done, else the reason it was refused.
    Q_INVOKABLE QString addEffect(const QString& track, const QString& effectId);
    Q_INVOKABLE QString removeEffect(const QString& track, const QString& effect);
    Q_INVOKABLE QString setEffectEnabled(const QString& track, const QString& effect, bool enabled);
    Q_INVOKABLE QString moveEffect(const QString& track, const QString& effect, int index);
    /// One effect's settings in UI units: { key, name, description, enabled, known, params: [{ key, name, kind (slider |
    /// toggle | choice), value, defaultValue, from, to, step, decimals, origin, unit, animated, animatable, options,
    /// current }] } at the playhead; empty when the effect is gone.
    Q_INVOKABLE QVariantMap effectSettings(const QString& track, const QString& effect) const;
    /// One setting: the constant, or for an animated one the key at the playhead. `gesture` merges consecutive calls
    /// into one undo step until endEffectGesture().
    Q_INVOKABLE QString setEffectParam(const QString& track, const QString& effect, const QString& param, double value, bool gesture);
    Q_INVOKABLE void endEffectGesture();
    /// Every setting back to its default (one undo step).
    Q_INVOKABLE QString resetEffect(const QString& track, const QString& effect);
    /// The stopwatch: a setting gets a key at the playhead (and follows keys from then on), or loses its keys and keeps
    /// the value it has at the playhead.
    Q_INVOKABLE QString setAnimated(const QString& track, const QString& effect, const QString& param, bool animated);

private:
    ProjectController* controller() const;

    ProjectController* m_controller = nullptr;
};

} // namespace wabcut::app
