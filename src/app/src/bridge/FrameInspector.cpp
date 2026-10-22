#include "FrameInspector.h"

#include "BridgeIds.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"

#include <wabcut/editor/PlaybackSession.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/RedactionMedia.h>
#include <wabcut/model/Timecode.h>
#include <wabcut/model/TimeRemap.h>
#include <wabcut/playback/audio/TimelineAudioPlayer.h>
#include <wabcut/playback/video/PlaybackController.h>
#include <wabcut/render/EffectLibrary.h>

#include <QFileInfo>
#include <QQmlEngine>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <optional>

namespace wabcut::app {

namespace {

using State = ExplainedEffect::State;

QString text(const std::string& s) { return QString::fromStdString(s); }

QString effectName(std::string_view id) {
    if (const EffectDescriptor* d = EffectRegistry::builtin().find(id))
        return QString::fromUtf8(d->displayName.data(), static_cast<qsizetype>(d->displayName.size()));
    return QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
}

QString trackLabel(TrackKind kind, std::size_t index) {
    return (kind == TrackKind::Video ? QStringLiteral("V%1") : QStringLiteral("A%1")).arg(index + 1);
}

QString percent(double fraction) { return QStringLiteral("%1 %").arg(std::lround(fraction * 100.0)); }

QString decibels(double db) { return QStringLiteral("%1 dB").arg(db, 0, 'f', 1); }

QString balance(double pan) {
    if (std::abs(pan) < 0.005)
        return FrameInspector::tr("Centre");
    return (pan < 0 ? QStringLiteral("L %1") : QStringLiteral("R %1")).arg(std::lround(std::abs(pan) * 100.0));
}

QString speedText(std::int64_t num, std::int64_t den) {
    const double pct = den != 0 ? 100.0 * static_cast<double>(num) / static_cast<double>(den) : 0.0;
    return QStringLiteral("%1 %").arg(QString::number(pct, 'g', 4));
}

QString maskText(const Mask& mask) {
    QString out = mask.name.empty() ? FrameInspector::tr("Mask") : text(mask.name);
    QStringList notes;
    notes << QString::fromUtf8(toString(mask.mode).data(), static_cast<qsizetype>(toString(mask.mode).size()));
    if (mask.inverted)
        notes << FrameInspector::tr("inverted");
    if (!mask.enabled)
        notes << FrameInspector::tr("off");
    return out + QStringLiteral(" (") + notes.join(QStringLiteral(", ")) + QLatin1Char(')');
}

/// The render effect type an evaluable video effect becomes (to pair stack entries with LayerSpec::effects).
std::optional<render::EffectType> renderTypeOf(std::string_view id) {
    using render::EffectType;
    if (id == "wabcut.crop")
        return EffectType::Crop;
    if (id == "wabcut.blur.gaussian")
        return EffectType::GaussianBlur;
    if (id == "wabcut.color.basic")
        return EffectType::BasicCorrection;
    if (id == "wabcut.vignette")
        return EffectType::Vignette;
    if (id == "wabcut.sharpen")
        return EffectType::Sharpen;
    if (id == "wabcut.dropShadow" || id == "wabcut.title.style")
        return EffectType::DropShadow;
    if (id == "wabcut.color.grade")
        return EffectType::Grade;
    if (id == "wabcut.mask")
        return EffectType::OpacityMask;
    if (id == "wabcut.mosaic")
        return EffectType::Mosaic;
    // 0.2: the C.15 effects and the library's, so the effects after them pair with their evaluated entries too.
    if (id == kBorderEffect)
        return EffectType::Border;
    if (id == kUltraKeyEffect)
        return EffectType::UltraKey;
    if (id == kTrackMatteEffect)
        return EffectType::TrackMatte;
    if (render::findLibraryEffect(id))
        return EffectType::Library;
    return std::nullopt;
}

/// An encoded colour (0..1 components) as "#RRGGBB".
QString hexColor(double r, double g, double b) {
    const auto byte = [](double v) { return static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
    return QStringLiteral("#%1%2%3")
        .arg(byte(r), 2, 16, QLatin1Char('0'))
        .arg(byte(g), 2, 16, QLatin1Char('0'))
        .arg(byte(b), 2, 16, QLatin1Char('0'))
        .toUpper();
}

QString amount(double v) { return QString::number(std::lround(v)); }

/// 0.2: what an evaluated Border, Ultra Key or Track Matte Key does at this frame (LayerEffect::params, in the order
/// PreparedComposite.h lists them); empty for other effects. `matteFound`: the evaluator put the matte track's clip
/// before the layer (a matte group; LayerEffect::matte is only filled later, when the composite is built).
QString effectDetail(const render::LayerEffect& e, bool matteFound) {
    const auto& p = e.params;
    switch (e.type) {
    case render::EffectType::Border:
        //: Explain this frame, a Border effect: "8 px, #FFFFFF, 100 % opacity"
        return FrameInspector::tr("%1 px, %2, %3 opacity").arg(amount(p[0]), hexColor(p[1], p[2], p[3]), percent(p[4]));
    case render::EffectType::UltraKey: {
        const int output = static_cast<int>(std::lround(p[0]));
        const QString outputName = output == 1   ? FrameInspector::tr("Alpha Channel")
                                   : output == 2 ? FrameInspector::tr("Color Channel")
                                                 : FrameInspector::tr("Composite");
        //: Explain this frame, an Ultra Key: "key #00FF00, output Composite; matte: transparency 45, tolerance 50,
        //: pedestal 10; cleanup: choke 0, soften 0; spill 50"
        return FrameInspector::tr("key %1, output %2; matte: transparency %3, tolerance %4, pedestal %5; cleanup: choke %6, "
                                  "soften %7; spill %8")
            .arg(hexColor(p[1], p[2], p[3]), outputName, amount(p[4]), amount(p[7]), amount(p[8]), amount(p[9]),
                 amount(p[10]), amount(p[15]));
    }
    case render::EffectType::TrackMatte: {
        const int track = static_cast<int>(std::lround(p[0]));
        const bool luma = p[1] >= 0.5;
        const bool reversed = p[2] >= 0.5;
        if (p[3] < 0.5) {
            //: Explain this frame, a Track Matte Key whose matte track is missing: "No matte track V4: the layer shows nothing"
            return FrameInspector::tr("No matte track V%1: the layer shows nothing").arg(track);
        }
        if (!matteFound) {
            //: Explain this frame, a Track Matte Key whose matte track is empty at the playhead
            return reversed ? FrameInspector::tr("Matte V%1 is empty here: reversed, the layer shows in full").arg(track)
                            : FrameInspector::tr("Matte V%1 is empty here: the layer shows nothing").arg(track);
        }
        //: Explain this frame, a Track Matte Key: "Matte V3 (luma), reversed"
        const QString read = FrameInspector::tr("Matte V%1 (%2)").arg(track).arg(luma ? FrameInspector::tr("luma")
                                                                                       : FrameInspector::tr("alpha"));
        return reversed ? FrameInspector::tr("%1, reversed").arg(read) : read;
    }
    default:
        break;
    }
    return {};
}

std::vector<GradeStage> gradeStages(const color::GradeSpec& g) {
    return {
        {FrameInspector::tr("Input LUT"), g.inputLutActive()}, {FrameInspector::tr("Basic correction"), g.basicActive()},
        {FrameInspector::tr("Creative look"), g.creativeActive()}, {FrameInspector::tr("Curves"), g.curvesActive()},
        {FrameInspector::tr("Colour wheels"), g.wheelsActive()},   {FrameInspector::tr("HSL secondary"), g.hslActive()},
        {FrameInspector::tr("Vignette"), g.vignetteActive()},
    };
}

/// The stack as an evaluator of `kind` treats it, with preview bypasses marked.
std::vector<ExplainedEffect> explainStack(const EffectStack& stack, EffectKind kind, std::uint64_t owner,
                                          const PreviewOverrides& overrides) {
    std::vector<ExplainedEffect> out;
    out.reserve(stack.size());
    for (const EffectInstance& instance : stack) {
        ExplainedEffect e;
        e.id = instance.id;
        e.effect = instance.effect;
        e.name = effectName(instance.effect);
        if (!instance.enabled)
            e.state = State::Disabled;
        else if (!isEvaluable(instance, kind))
            e.state = State::Unsupported;
        else if (overrides.isBypassed(owner, instance.id))
            e.state = State::PreviewBypassed;
        for (const Mask& mask : instance.masks)
            e.masks << maskText(mask);
        out.push_back(std::move(e));
    }
    return out;
}

/// The transition of `track` whose range covers `time` and involves `clip`.
const Transition* transitionAt(const Track& track, ClipId clip, Ticks time) {
    for (const Transition& t : track.transitions) {
        if (t.outgoing != clip && t.incoming != clip)
            continue;
        const auto cut = transitionEditPoint(track, t);
        if (cut && TimeRange::fromStartEnd(*cut - t.before, *cut + t.after).contains(time))
            return &t;
    }
    return nullptr;
}

QString transitionKindName(render::TransitionKind kind) {
    switch (kind) {
    case render::TransitionKind::DipToColor:
        return FrameInspector::tr("Dip to colour");
    case render::TransitionKind::Wipe:
        return FrameInspector::tr("Wipe");
    case render::TransitionKind::CrossDissolve:
        return FrameInspector::tr("Cross Dissolve");
    default: // the library's transitions (0.2) are named by their model effect
        break;
    }
    return FrameInspector::tr("Transition");
}

QString mappingText(const Clip& clip, Ticks time, bool& remapped) {
    remapped = false;
    if (const Curve* remap = clip.timeRemap()) {
        remapped = true;
        const Ticks curve = clip.curveTimeAt(time);
        const auto segments = remapSegments(*remap);
        const RemapSegment segment = remapSegmentAt(*remap, curve);
        const auto it = std::find(segments.begin(), segments.end(), segment);
        const auto index = it == segments.end() ? 0 : static_cast<int>(it - segments.begin()) + 1;
        QString motion;
        switch (segment.motion) {
        case RemapMotion::Hold:
            motion = FrameInspector::tr("hold");
            break;
        case RemapMotion::Reverse:
            motion = segment.straight ? FrameInspector::tr("%1 reverse").arg(speedText((segment.sourceFrom - segment.sourceTo).value, (segment.to - segment.from).value))
                                      : FrameInspector::tr("ramp, reverse");
            break;
        case RemapMotion::Forward:
            motion = segment.straight ? FrameInspector::tr("%1 forward").arg(speedText((segment.sourceTo - segment.sourceFrom).value, (segment.to - segment.from).value))
                                      : FrameInspector::tr("ramp, forward");
            break;
        }
        //: Time remapping row: "Time remap: segment 2 of 5, 150 % forward"
        return FrameInspector::tr("Time remap: segment %1 of %2, %3").arg(index).arg(segments.size()).arg(motion);
    }
    if (clip.isFreeze())
        return FrameInspector::tr("Freeze frame");
    const QString speed = speedText(clip.speed.num, clip.speed.den);
    return clip.reversed ? FrameInspector::tr("%1 reverse").arg(speed) : FrameInspector::tr("%1 forward").arg(speed);
}

const Clip* findClip(const Sequence& sequence, TrackKind kind, int trackIndex, ClipId clip) {
    const auto& tracks = sequence.tracks(kind);
    if (trackIndex < 0 || static_cast<std::size_t>(trackIndex) >= tracks.size())
        return nullptr;
    return tracks[static_cast<std::size_t>(trackIndex)]->findClip(clip);
}

QStringList redactionNames(const ProjectState& project, const Clip& clip, Ticks sourceTime, bool audio) {
    QStringList out;
    for (const Redaction* covering : redactionsCovering(project, clip.media)) { // a subclip's master's too (wave 5)
        const Redaction& r = *covering;
        if (!r.enabled || !r.appliesTo(clip.id))
            continue;
        if (!audio) {
            if (r.source.contains(sourceTime))
                out << (r.name.empty() ? FrameInspector::tr("Redaction") : text(r.name)) + QStringLiteral(": ") +
                           QString::fromUtf8(toString(r.style).data(), static_cast<qsizetype>(toString(r.style).size()));
            continue;
        }
        if (r.audio == RedactionAudio::None)
            continue;
        const bool covered = r.audioRanges.empty()
                                 ? r.source.contains(sourceTime)
                                 : std::any_of(r.audioRanges.begin(), r.audioRanges.end(),
                                               [&](const TimeRange& range) { return range.contains(sourceTime); });
        if (covered)
            out << (r.name.empty() ? FrameInspector::tr("Redaction") : text(r.name)) + QStringLiteral(": ") +
                       (r.audio == RedactionAudio::Bleep ? FrameInspector::tr("bleep (1 kHz, -12 dBFS)")
                                                         : FrameInspector::tr("muted"));
    }
    return out;
}

void explainVideo(const ProjectState& project, const Sequence& sequence, Ticks time,
                  const playback::PreviewSettings& preview, const playback::CaptionSettings* captions,
                  const PreviewOverrides& overrides, FrameExplanation& out) {
    std::vector<playback::LayerSpec> all;
    playback::evaluateVideoLayers(project, sequence, time, all, /*cullOccluded*/ false, &preview, captions);
    std::vector<playback::LayerSpec> shown;
    playback::evaluateVideoLayers(project, sequence, time, shown, /*cullOccluded*/ true, &preview, captions);

    // 0.2: layers inside a matte group (a Track Matte Key's matte, a library effect's second picture) are not drawn;
    // they shape the layer right after the group.
    std::vector<QString> matteFor(all.size());
    std::vector<bool> hasTrackMatte(all.size(), false); // the layer has its Track Matte group before it
    for (std::size_t i = 0; i < all.size(); ++i) {
        const playback::LayerSpec& group = all[i];
        if (!group.nested || !group.matte)
            continue;
        const std::size_t head = i + 1 + static_cast<std::size_t>(std::max(0, group.nestedCount));
        if (head >= all.size())
            continue;
        const playback::LayerSpec& shaped = all[head];
        if (group.matteEffect < 0)
            hasTrackMatte[head] = true;
        const QString track = trackLabel(TrackKind::Video, static_cast<std::size_t>(std::max(0, shaped.track)));
        QString text = FrameInspector::tr("Track Matte of %1").arg(track);
        if (group.matteEffect >= 0) {
            const Clip* clip = findClip(sequence, TrackKind::Video, shaped.track, shaped.clip);
            QString effect = FrameInspector::tr("effect");
            if (clip) {
                // The matteEffect-th evaluated effect of the shaped layer: count the stack's library effects to find it.
                int seen = -1;
                for (const EffectInstance& instance : clip->effects()) {
                    if (!instance.enabled || !isEvaluable(instance, EffectKind::Video) || !renderTypeOf(instance.effect))
                        continue;
                    if (++seen == group.matteEffect) {
                        effect = effectName(instance.effect);
                        break;
                    }
                }
            }
            //: Explain this frame: a layer used by another layer's effect, "Second picture of V2's Gradient Wipe"
            text = FrameInspector::tr("Second picture of %1's %2").arg(track, effect);
        }
        for (std::size_t k = i + 1; k < head; ++k)
            matteFor[k] = text;
    }

    for (std::size_t i = 0; i < all.size(); ++i) {
        const playback::LayerSpec& spec = all[i];
        if (spec.caption || !spec.item)
            continue; // the caption has its own section
        const Clip* clip = findClip(sequence, TrackKind::Video, spec.track, spec.clip);
        if (!clip)
            continue;
        ExplainedVideoLayer layer;
        layer.clip = spec.clip;
        layer.track = sequence.videoTracks[static_cast<std::size_t>(spec.track)]->id;
        layer.trackName = trackLabel(TrackKind::Video, static_cast<std::size_t>(spec.track));
        layer.clipName = text(clip->name());
        layer.mediaName = text(spec.item->name);
        if (spec.title) {
            layer.source = ExplainedVideoLayer::Source::Generated;
        } else {
            const playback::ResolvedSource resolved = playback::resolveSource(*spec.item, preview);
            layer.source = !resolved.proxy             ? ExplainedVideoLayer::Source::Original
                           : resolved.originalOffline ? ExplainedVideoLayer::Source::ProxyForOffline
                                                      : ExplainedVideoLayer::Source::Proxy;
            if (resolved.path)
                layer.sourceFile = QFileInfo(text(*resolved.path)).fileName();
        }
        layer.sourceTime = spec.sourceTime;
        layer.curveTime = playback::curveTimeAround(*clip, time);
        Rate rate = sequence.frameRate;
        const auto streamIndex = static_cast<std::size_t>(spec.streamIndex);
        if (streamIndex < spec.item->info.video.size() && spec.item->info.video[streamIndex].frameRate.isValid())
            rate = spec.item->info.video[streamIndex].frameRate;
        layer.sourceFrame = ticksToFrames(spec.sourceTime, rate, Rounding::Floor).value_or(0);
        layer.sourceTimecode = text(ticksToTimecode(spec.sourceTime, rate, false));
        layer.mapping = mappingText(*clip, time, layer.remapped);

        // Transition: the group's first layer carries it, the second is its incoming side.
        const playback::LayerSpec* lead = nullptr;
        bool incoming = false;
        if (spec.transition) {
            lead = &spec;
            incoming = spec.transition->incomingOnly;
        } else if (i > 0 && all[i - 1].transition && all[i - 1].transition->layerCount == 2 && all[i - 1].track == spec.track) {
            lead = &all[i - 1];
            incoming = true;
        }
        if (lead) {
            const Track& track = *sequence.videoTracks[static_cast<std::size_t>(spec.track)];
            const Transition* model = transitionAt(track, spec.clip, time);
            layer.transition = model ? effectName(model->effect) : transitionKindName(lead->transition->kind);
            layer.transitionProgress = lead->transition->progress;
            layer.transitionSide = lead->transition->layerCount == 1 ? QStringLiteral("one-sided")
                                   : incoming                        ? QStringLiteral("incoming")
                                                                     : QStringLiteral("outgoing");
        }

        // Effects: the stack as the evaluator reads it, paired with the evaluated LayerEffects for grades and masks.
        layer.effects = explainStack(clip->effects(), EffectKind::Video, spec.clip.value, overrides);
        layer.evaluatedEffects = static_cast<int>(spec.effects.size());
        layer.bypassedEffects = spec.bypassedEffects;
        std::size_t cursor = 0;
        for (ExplainedEffect& e : layer.effects) {
            if (e.state != State::Applied && e.state != State::PreviewBypassed)
                continue;
            const auto type = renderTypeOf(e.effect);
            if (!type || cursor >= spec.effects.size() || spec.effects[cursor].type != *type)
                continue;
            const render::LayerEffect& evaluated = spec.effects[cursor++];
            e.detail = effectDetail(evaluated, hasTrackMatte[i]); // 0.2
            if (evaluated.type == render::EffectType::Grade && evaluated.grade && layer.grade.empty()) {
                layer.grade = gradeStages(*evaluated.grade);
                layer.missingLuts = evaluated.grade->missingLuts;
            }
        }
        layer.redactions = redactionNames(project, *clip, spec.sourceTime, false);
        layer.evaluatedRedactions = static_cast<int>(spec.redactions.size());
        layer.opacity = spec.opacity;
        if (spec.blend != BlendMode::Normal) {
            const std::string_view blend = displayName(spec.blend);
            layer.blendMode = QString::fromUtf8(blend.data(), static_cast<qsizetype>(blend.size()));
        }
        layer.matteFor = matteFor[i];
        layer.transform = spec.transform;
        layer.occluded = std::none_of(shown.begin(), shown.end(), [&](const playback::LayerSpec& s) {
            return s.clip == spec.clip && s.track == spec.track;
        });
        layer.soloed = overrides.soloClips.contains(spec.clip);
        out.video.push_back(std::move(layer));
    }
    std::reverse(out.video.begin(), out.video.end()); // top of the stack first
}

void explainAudio(const ProjectState& project, const Sequence& sequence, Ticks time, const PreviewOverrides& overrides,
                  FrameExplanation& out) {
    bool anySolo = false;
    for (const TrackPtr& t : sequence.audioTracks)
        anySolo = anySolo || t->solo;
    for (std::size_t i = 0; i < sequence.audioTracks.size(); ++i) {
        const Track& track = *sequence.audioTracks[i];
        const auto index = track.clipIndexAt(time);
        if (!index)
            continue;
        const Clip& clip = track.clips[*index];
        const MediaItem* item = project.findMedia(clip.media);
        ExplainedAudioLayer layer;
        layer.clip = clip.id;
        layer.track = track.id;
        layer.trackName = trackLabel(TrackKind::Audio, i);
        layer.clipName = text(clip.name());
        layer.mediaName = item ? text(item->name) : QString();
        layer.sourceTime = clip.sourceTimeAt(time);
        Rate rate = kRate48kHz;
        if (item && static_cast<std::size_t>(clip.stream.index) < item->info.audio.size())
            rate = item->info.audio[static_cast<std::size_t>(clip.stream.index)].sampleRate;
        layer.sampleRate = static_cast<int>(rate.num / std::max<std::int64_t>(1, rate.den));
        layer.sourceSample = ticksToFrames(layer.sourceTime, rate, Rounding::Floor).value_or(0);
        layer.sourceTimecode = text(ticksToTimecode(layer.sourceTime, sequence.frameRate, false));

        // Why it may be silent: the mixer's switches, then the clip, then the C.6 speed policy.
        const auto ranges = clip.unitySpeedRanges();
        const bool unity = std::any_of(ranges.begin(), ranges.end(), [&](const TimeRange& r) { return r.contains(time); });
        if (!track.enabled)
            layer.silentReason = FrameInspector::tr("Track output is off");
        else if (track.muted)
            layer.silentReason = FrameInspector::tr("Track is muted");
        else if (anySolo && !track.solo)
            layer.silentReason = FrameInspector::tr("Another track is soloed");
        else if (!clip.enabled)
            layer.silentReason = FrameInspector::tr("Clip is disabled");
        else if (!item || item->locator.offline)
            layer.silentReason = FrameInspector::tr("Media is offline");
        else if (!unity)
            layer.silentReason = FrameInspector::tr("Silent: audio plays only at 100 % forward speed");

        // The chain in the order the track renderer and the master bus apply it (A.3: track fader and pan after the
        // track's effects, the master fader after the master stack); curves at this timeline time.
        layer.gain.push_back({FrameInspector::tr("Clip volume"), decibels(clip.parameterAt(ParamId::VolumeDb, time))});
        layer.gain.push_back({FrameInspector::tr("Clip pan"), balance(clip.parameterAt(ParamId::Pan, time))});
        layer.gain.push_back({FrameInspector::tr("Track fader"),
                              track.muted ? FrameInspector::tr("Muted") : decibels(track.volume.valueAt(time))});
        layer.gain.push_back({FrameInspector::tr("Track pan"), balance(track.pan.valueAt(time))});
        layer.gain.push_back({FrameInspector::tr("Master fader"), decibels(sequence.masterVolume.valueAt(time))});
        layer.clipEffects = explainStack(clip.effects(), EffectKind::Audio, clip.id.value, overrides);
        layer.trackEffects = explainStack(track.effects, EffectKind::Audio, track.id.value, overrides);
        if (const Transition* t = transitionAt(track, clip.id, time))
            layer.transition = effectName(t->effect);
        layer.redactions = redactionNames(project, clip, layer.sourceTime, true);
        layer.soloed = overrides.soloClips.contains(clip.id);
        out.audio.push_back(std::move(layer));
    }
    out.masterEffects = explainStack(sequence.audioEffects, EffectKind::Audio, 0, overrides);
}

std::optional<ExplainedCaption> explainCaption(const Sequence& sequence, Ticks time,
                                               const playback::CaptionSettings* captions) {
    const playback::CaptionSettings fallback;
    const playback::CaptionSettings& settings = captions ? *captions : fallback;
    const CaptionTrack* track = playback::captionTrackFor(sequence, settings);
    if (!track)
        return std::nullopt;
    const auto cue = std::find_if(track->cues.begin(), track->cues.end(), [&](const CaptionCue& c) {
        return TimeRange{c.start, c.duration}.contains(time);
    });
    if (cue == track->cues.end())
        return std::nullopt;
    ExplainedCaption out;
    out.shown = captions != nullptr;
    out.trackName = text(track->name);
    out.text = text(cue->text);
    out.inText = text(ticksToTimecode(cue->start, sequence.frameRate, sequence.dropFrame));
    out.outText = text(ticksToTimecode(cue->end(), sequence.frameRate, sequence.dropFrame));
    if (!captions) {
        out.timing = FrameInspector::tr("Cue In/Out (captions are not burned into the program monitor)");
    } else if (!settings.karaoke) {
        out.timing = FrameInspector::tr("Cue In/Out of the caption track");
    } else {
        const bool words = settings.words &&
                           settings.words->cues.contains({track->id.value, cue->start.value});
        out.timing = words ? FrameInspector::tr("Karaoke: word times from the transcript")
                           : FrameInspector::tr("Karaoke: word times estimated from the text");
    }
    return out;
}

QString stateKey(State state) {
    switch (state) {
    case State::Disabled:
        return QStringLiteral("disabled");
    case State::Unsupported:
        return QStringLiteral("unsupported");
    case State::PreviewBypassed:
        return QStringLiteral("previewBypassed");
    case State::Applied:
        break;
    }
    return QStringLiteral("applied");
}

QString stateText(State state) {
    switch (state) {
    case State::Disabled:
        return FrameInspector::tr("Off in the project");
    case State::Unsupported:
        return FrameInspector::tr("Bypassed: this version cannot run it");
    case State::PreviewBypassed:
        return FrameInspector::tr("Bypassed for the preview only");
    case State::Applied:
        break;
    }
    return FrameInspector::tr("Applied");
}

QVariantList effectRows(const std::vector<ExplainedEffect>& effects, const QString& owner) {
    QVariantList out;
    for (const ExplainedEffect& e : effects) {
        out.append(QVariantMap{
            {QStringLiteral("key"), effectKey(e.id)},
            {QStringLiteral("owner"), owner},
            {QStringLiteral("name"), e.name},
            {QStringLiteral("state"), stateKey(e.state)},
            {QStringLiteral("stateText"), stateText(e.state)},
            {QStringLiteral("masks"), e.masks.join(QStringLiteral(", "))},
            {QStringLiteral("detail"), e.detail},
        });
    }
    return out;
}

/// The copy of `track` with `fn(Clip&)` applied to every clip it changes (shares the track when none changes).
template <class Fn>
TrackPtr mutateClips(const TrackPtr& track, Fn&& fn) {
    std::optional<Track> copy;
    for (std::size_t i = 0; i < track->clips.size(); ++i) {
        Clip probe = track->clips[i];
        if (!fn(probe))
            continue;
        if (!copy)
            copy.emplace(*track);
        copy->clips.mutableAt(i) = std::move(probe);
    }
    return copy ? std::make_shared<const Track>(std::move(*copy)) : track;
}

EffectStack bypassIn(const EffectStack& stack, std::uint64_t owner, const PreviewOverrides& overrides, bool& changed) {
    EffectStack out = stack;
    for (EffectInstance& e : out) {
        if (e.enabled && overrides.isBypassed(owner, e.id)) {
            e.enabled = false;
            changed = true;
        }
    }
    return out;
}

} // namespace

ProjectState applyPreviewOverrides(const ProjectState& project, SequenceId sequenceId, const PreviewOverrides& overrides) {
    const Sequence* sequence = project.findSequence(sequenceId);
    if (!sequence || overrides.empty())
        return project;
    Sequence next = *sequence;
    bool soloVideo = false;
    bool soloAudio = false;
    sequence->forEachTrack([&](const Track& t) {
        for (const ClipId id : overrides.soloClips) {
            if (t.findClip(id)) {
                (t.kind == TrackKind::Video ? soloVideo : soloAudio) = true;
            }
        }
    });
    const auto apply = [&](std::vector<TrackPtr>& tracks, bool solo) {
        for (TrackPtr& track : tracks) {
            TrackPtr changed = mutateClips(track, [&](Clip& clip) {
                bool dirty = false;
                if (solo && clip.enabled && !overrides.soloClips.contains(clip.id)) {
                    clip.enabled = false;
                    dirty = true;
                }
                bool effects = false;
                EffectStack stack = bypassIn(clip.effects(), clip.id.value, overrides, effects);
                if (effects) {
                    clip.setEffects(std::move(stack));
                    dirty = true;
                }
                return dirty;
            });
            bool trackEffects = false;
            EffectStack stack = bypassIn(changed->effects, changed->id.value, overrides, trackEffects);
            if (trackEffects) {
                Track copy = *changed;
                copy.effects = std::move(stack);
                changed = std::make_shared<const Track>(std::move(copy));
            }
            track = std::move(changed);
        }
    };
    apply(next.videoTracks, soloVideo);
    apply(next.audioTracks, soloAudio);
    bool master = false;
    EffectStack masterStack = bypassIn(next.audioEffects, 0, overrides, master);
    if (master)
        next.audioEffects = std::move(masterStack);
    return project.withSequence(std::make_shared<const Sequence>(std::move(next)));
}

FrameExplanation explainFrame(const ProjectState& project, const Sequence& sequence, Ticks time,
                              const playback::PreviewSettings& preview, const playback::CaptionSettings* captions,
                              const PreviewOverrides& overrides) {
    FrameExplanation out;
    out.time = time;
    explainVideo(project, sequence, time, preview, captions, overrides, out);
    explainAudio(project, sequence, time, overrides, out);
    out.caption = explainCaption(sequence, time, captions);
    return out;
}

// ---- FrameInspector ----------------------------------------------------------------------------------------------

FrameInspector::FrameInspector(QObject* parent) : QObject(parent) {}

FrameInspector::~FrameInspector() {
    // Point the players back at the project before the private session goes.
    if (m_retargeted && m_controller)
        retarget(&m_controller->session());
}

FrameInspector* FrameInspector::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* inspector = new FrameInspector;
    if (qmlEngine) {
        inspector->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        inspector->setPlayback(qmlEngine->singletonInstance<PlaybackBridge*>("Wabcut", "Playback"));
    }
    return inspector;
}

void FrameInspector::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    m_overrides = {};
    if (controller) {
        m_sequence = controller->currentSequenceId();
        connect(controller, &ProjectController::playheadChanged, this, &FrameInspector::scheduleRefresh);
        connect(controller, &ProjectController::changed, this, [this](const QVariantMap& change) {
            const QString kind = change.value(QStringLiteral("kind")).toString();
            if (kind == QStringLiteral("reset"))
                clearOverrides();
            else if (!m_overrides.empty())
                applyOverrides(); // the overrides follow the edit
            scheduleRefresh();
        });
        connect(controller, &ProjectController::sequenceChanged, this, [this] {
            if (m_controller && m_controller->currentSequenceId() != m_sequence) {
                m_sequence = m_controller->currentSequenceId();
                clearOverrides();
            }
            scheduleRefresh();
        });
        connect(controller, &ProjectController::sessionClosing, this, [this] {
            if (m_retargeted)
                retarget(m_controller ? &m_controller->session() : nullptr);
            m_overrides = {};
        });
    }
    scheduleRefresh();
}

void FrameInspector::setPlayback(PlaybackBridge* playback) {
    if (m_playback == playback)
        return;
    if (m_playback)
        disconnect(m_playback, nullptr, this, nullptr);
    m_playback = playback;
    if (playback) {
        connect(playback, &PlaybackBridge::captionsChanged, this, &FrameInspector::scheduleRefresh);
        // The audio player starts on the first play: point it at the preview state too.
        connect(playback, &PlaybackBridge::transportChanged, this, [this] {
            if (m_retargeted && m_previewSession)
                retarget(m_previewSession.get());
        });
    }
    scheduleRefresh();
}

void FrameInspector::setActive(bool active) {
    if (m_active == active)
        return;
    m_active = active;
    emit activeChanged();
    if (active)
        refresh();
}

int FrameInspector::overrideCount() const {
    return static_cast<int>(m_overrides.soloClips.size() + m_overrides.bypassed.size());
}

ProjectSnapshot FrameInspector::previewSnapshot() const {
    return m_retargeted && m_previewSession ? m_previewSession->snapshot() : nullptr;
}

void FrameInspector::scheduleRefresh() {
    if (!m_active || m_refreshQueued)
        return;
    m_refreshQueued = true;
    // Coalesce a burst (a scrub, an edit with its playhead move) into one evaluation.
    QTimer::singleShot(0, this, [this] {
        m_refreshQueued = false;
        refresh();
    });
}

void FrameInspector::refresh() {
    m_explanation = {};
    if (m_controller) {
        const ProjectSnapshot snap = m_controller->snapshot();
        const SequencePtr sequence = m_controller->currentSequence();
        if (snap && sequence) {
            const playback::PreviewSettings preview = m_playback ? m_playback->previewSettings() : playback::PreviewSettings{};
            const playback::CaptionSettings* captions =
                m_playback && m_playback->captionSettings() ? &*m_playback->captionSettings() : nullptr;
            m_explanation = explainFrame(*snap, *sequence, m_controller->playhead(), preview, captions, m_overrides);
        }
    }
    publish();
}

void FrameInspector::publish() {
    const FrameExplanation& e = m_explanation;
    m_timecode = m_controller ? m_controller->formatTimecode(e.time) : QString();
    m_layers.clear();
    for (const ExplainedVideoLayer& layer : e.video) {
        const QString key = clipKey(layer.clip);
        QString source;
        QString sourceKey;
        switch (layer.source) {
        case ExplainedVideoLayer::Source::Original:
            sourceKey = QStringLiteral("original");
            source = tr("Original media");
            break;
        case ExplainedVideoLayer::Source::Proxy:
            sourceKey = QStringLiteral("proxy");
            source = tr("Proxy");
            break;
        case ExplainedVideoLayer::Source::ProxyForOffline:
            sourceKey = QStringLiteral("proxyOffline");
            source = tr("Proxy (the original is offline)");
            break;
        case ExplainedVideoLayer::Source::Generated:
            sourceKey = QStringLiteral("generated");
            source = tr("Generated title (nothing decoded)");
            break;
        }
        QVariantList grade;
        QStringList activeStages;
        for (const GradeStage& stage : layer.grade) {
            grade.append(QVariantMap{{QStringLiteral("name"), stage.name}, {QStringLiteral("active"), stage.active}});
            if (stage.active)
                activeStages << stage.name;
        }
        QString gradeText;
        if (!layer.grade.empty()) {
            gradeText = activeStages.isEmpty() ? tr("Neutral grade") : activeStages.join(QStringLiteral(" → "));
            if (layer.missingLuts > 0)
                gradeText += tr(" (%n LUT(s) missing)", nullptr, layer.missingLuts);
        }
        QString transition;
        if (layer.transition) {
            //: "Cross Dissolve 40 %, outgoing"
            transition = tr("%1 %2, %3").arg(*layer.transition, percent(layer.transitionProgress),
                                             layer.transitionSide == QStringLiteral("incoming")  ? tr("incoming")
                                             : layer.transitionSide == QStringLiteral("outgoing") ? tr("outgoing")
                                                                                                    : tr("one-sided"));
        }
        m_layers.append(QVariantMap{
            {QStringLiteral("key"), key},
            {QStringLiteral("track"), layer.trackName},
            {QStringLiteral("clipName"), layer.clipName},
            {QStringLiteral("mediaName"), layer.mediaName},
            {QStringLiteral("source"), sourceKey},
            {QStringLiteral("sourceText"), source},
            {QStringLiteral("sourceFile"), layer.sourceFile},
            {QStringLiteral("sourceTimecode"), layer.sourceTimecode},
            {QStringLiteral("sourceFrame"), tr("frame %1").arg(layer.sourceFrame)},
            {QStringLiteral("mapping"), layer.mapping},
            {QStringLiteral("remapped"), layer.remapped},
            {QStringLiteral("transition"), transition},
            {QStringLiteral("effects"), effectRows(layer.effects, key)},
            {QStringLiteral("grade"), grade},
            {QStringLiteral("gradeText"), gradeText},
            {QStringLiteral("redactions"), layer.redactions.join(QStringLiteral(", "))},
            {QStringLiteral("opacityText"), percent(layer.opacity)},
            {QStringLiteral("motionText"),
             tr("Scale %1, position %2, %3, rotation %4°")
                 .arg(percent(layer.transform.scale))
                 .arg(std::lround(layer.transform.position.x))
                 .arg(std::lround(layer.transform.position.y))
                 .arg(QString::number(layer.transform.rotationDegrees, 'g', 4))},
            {QStringLiteral("blendText"), layer.blendMode},
            {QStringLiteral("matteText"), layer.matteFor},
            {QStringLiteral("occluded"), layer.occluded},
            {QStringLiteral("soloed"), layer.soloed},
        });
    }
    m_audio.clear();
    for (const ExplainedAudioLayer& layer : e.audio) {
        const QString key = clipKey(layer.clip);
        QVariantList gain;
        for (const ExplainedGain& g : layer.gain)
            gain.append(QVariantMap{{QStringLiteral("stage"), g.stage}, {QStringLiteral("value"), g.value}});
        QVariantList effects = effectRows(layer.clipEffects, key);
        effects.append(effectRows(layer.trackEffects, trackKey(layer.track)));
        m_audio.append(QVariantMap{
            {QStringLiteral("key"), key},
            {QStringLiteral("track"), layer.trackName},
            {QStringLiteral("clipName"), layer.clipName},
            {QStringLiteral("mediaName"), layer.mediaName},
            //: Audio source position: "sample 96000 at 48000 Hz"
            {QStringLiteral("sampleText"), tr("sample %1 at %2 Hz").arg(layer.sourceSample).arg(layer.sampleRate)},
            {QStringLiteral("sourceTimecode"), layer.sourceTimecode},
            {QStringLiteral("silentReason"), layer.silentReason},
            {QStringLiteral("gain"), gain},
            {QStringLiteral("effects"), effects},
            {QStringLiteral("transition"), layer.transition.value_or(QString())},
            {QStringLiteral("redactions"), layer.redactions.join(QStringLiteral(", "))},
            {QStringLiteral("soloed"), layer.soloed},
        });
    }
    m_master = effectRows(e.masterEffects, QStringLiteral("master"));
    m_caption.clear();
    if (e.caption) {
        m_caption = QVariantMap{
            {QStringLiteral("shown"), e.caption->shown},     {QStringLiteral("track"), e.caption->trackName},
            {QStringLiteral("text"), e.caption->text},       {QStringLiteral("inText"), e.caption->inText},
            {QStringLiteral("outText"), e.caption->outText}, {QStringLiteral("timing"), e.caption->timing},
        };
    }
    emit explanationChanged();
}

void FrameInspector::toggleSolo(const QString& key) {
    const auto clip = parseClipKey(key);
    if (!clip)
        return;
    if (!m_overrides.soloClips.erase(*clip))
        m_overrides.soloClips.insert(*clip);
    applyOverrides();
    refresh();
}

void FrameInspector::toggleBypass(const QString& owner, const QString& effect) {
    const auto id = parseEffectKey(effect);
    if (!id)
        return;
    std::uint64_t ownerValue = 0;
    if (const auto clip = parseClipKey(owner))
        ownerValue = clip->value;
    else if (const auto track = parseTrackKey(owner))
        ownerValue = track->value;
    else if (owner != QStringLiteral("master"))
        return;
    const std::pair<std::uint64_t, std::uint64_t> entry{ownerValue, id->value};
    if (!m_overrides.bypassed.erase(entry))
        m_overrides.bypassed.insert(entry);
    applyOverrides();
    refresh();
}

void FrameInspector::clearOverrides() {
    if (m_overrides.empty() && !m_retargeted)
        return;
    m_overrides = {};
    applyOverrides();
    refresh();
}

void FrameInspector::applyOverrides() {
    if (!m_controller)
        return;
    if (m_overrides.empty()) {
        if (m_retargeted)
            retarget(&m_controller->session());
        m_retargeted = false;
        emit overridesChanged();
        return;
    }
    const ProjectSnapshot snap = m_controller->snapshot();
    ProjectState state = applyPreviewOverrides(*snap, m_controller->currentSequenceId(), m_overrides);
    if (!m_previewSession) {
        EditSessionOptions options;
        options.undoLimit = 1;
        options.validateEdits = false; // a copy of a valid project with clips switched off
        options.deferRelease = false;
        m_previewSession = std::make_unique<EditSession>(std::move(state), options);
    } else {
        m_previewSession->reset(std::move(state));
    }
    if (!m_retargeted) {
        retarget(m_previewSession.get());
        m_retargeted = true;
    }
    emit overridesChanged();
}

void FrameInspector::retarget(const EditSession* session) {
    if (!m_playback || !session)
        return;
    editor::PlaybackSession* playback = m_playback->session();
    if (!playback)
        return;
    playback->video().setSession(session);
    if (playback::TimelineAudioPlayer* audio = playback->audioPlayer())
        audio->setSession(session);
}

} // namespace wabcut::app
