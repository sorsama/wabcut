// TimelineView's volume rubber band (M5g): audio clips show their volume (the intrinsic Volume, dB) as a line with
// its keyframes, editable in place with the Selection tool, as in Premiere:
//   click the line          adds a key there (at the volume it has there)
//   drag a key              moves it in time (between its neighbours, inside the clip) and level; Shift: level only
//   drag the line           a constant volume follows the pointer; an animated one moves the two keys around it
//   Alt-click a key         Bezier (smooth tangents) <-> linear
// The pen tool (P, 0.1.2) shows the band and edits it the same way, whether or not the band is shown for the selection
// tool. 0.2: the pen also shows video clips' Opacity as the same kind of band (100 % at the top, 0 % at the bottom,
// linear) with their opacity keyframes, edited the same way (Premiere's opacity rubber band with the Pen tool: click or
// Ctrl-click the line adds a key, drag a key moves it).
// Each drag is one undo step (merged edits, ended on release). Drawing is in TimelineView.cpp (buildTrack); the
// ghost curves of a preview (auto-duck) in TimelineViewPreview.cpp. The band runs under the clip's label, with a
// fader-like scale: +6 dB at the top, 0 dB a quarter down, -12 dB half way, -60 dB and quieter at the bottom.

#include "TimelineView.h"

#include <wabcut/model/FinishingOps.h>

#include <QGuiApplication>
#include <QStyleHints>

#include <algorithm>
#include <array>
#include <cmath>

namespace wabcut::app {

namespace {

/// The band's scale: (dB, fraction from the top), linear in between.
constexpr std::array<std::pair<double, double>, 6> kScale{
    std::pair{6.0, 0.0}, std::pair{0.0, 0.25}, std::pair{-12.0, 0.5}, std::pair{-24.0, 0.7}, std::pair{-48.0, 0.9},
    std::pair{-60.0, 1.0}};
constexpr double kKeyReach = 5.0;  // px around a key
constexpr double kLineReach = 4.0; // px around the line

#define LABEL(text) QT_TRANSLATE_NOOP("wabcut::app::ProjectController", text)

/// The band's parameter: Volume on audio clips, Opacity on video clips (0.2).
ParamId bandParam(bool audio) { return audio ? ParamId::VolumeDb : ParamId::Opacity; }

const edit::ParamRef& bandRef(bool audio) {
    static const edit::ParamRef volume = edit::ParamRef::of(ParamId::VolumeDb);
    static const edit::ParamRef opacity = edit::ParamRef::of(ParamId::Opacity);
    return audio ? volume : opacity;
}

double clampBand(bool audio, double v) { return audio ? std::clamp(v, -144.0, 24.0) : std::clamp(v, 0.0, 1.0); }

/// Smooth tangents for a key turned Bezier: the slope between its neighbours (model units per second).
double smoothSlope(const Curve& curve, std::size_t i) {
    const std::size_t a = i > 0 ? i - 1 : i;
    const std::size_t b = i + 1 < curve.keys.size() ? i + 1 : i;
    const double span = (curve.keys[b].time - curve.keys[a].time).toSeconds();
    return span > 0 ? (curve.keys[b].value - curve.keys[a].value) / span : 0.0;
}

} // namespace

double TimelineView::volumeFraction(double db) {
    if (!std::isfinite(db) || db <= kScale.back().first)
        return 1.0;
    if (db >= kScale.front().first)
        return 0.0;
    for (std::size_t i = 1; i < kScale.size(); ++i) {
        const auto [db1, f1] = kScale[i];
        if (db >= db1) {
            const auto [db0, f0] = kScale[i - 1];
            return f0 + (db0 - db) / (db0 - db1) * (f1 - f0);
        }
    }
    return 1.0;
}

double TimelineView::volumeDbAtFraction(double fraction) {
    fraction = std::clamp(fraction, 0.0, 1.0);
    for (std::size_t i = 1; i < kScale.size(); ++i) {
        const auto [db1, f1] = kScale[i];
        if (fraction <= f1) {
            const auto [db0, f0] = kScale[i - 1];
            return db0 - (fraction - f0) / (f1 - f0) * (db0 - db1);
        }
    }
    return kScale.back().first;
}

double TimelineView::bandFraction(bool audio, double value) {
    return audio ? volumeFraction(value) : 1.0 - std::clamp(std::isfinite(value) ? value : 0.0, 0.0, 1.0);
}

double TimelineView::bandValueAtFraction(bool audio, double fraction) {
    return audio ? volumeDbAtFraction(fraction) : 1.0 - std::clamp(fraction, 0.0, 1.0);
}

QRectF TimelineView::volumeArea(const QRectF& clipRect) {
    // Under the clip's label (the waveform's band), a little above the bottom edge.
    const double top = std::min(16.0, clipRect.height() * 0.35);
    const double bottom = std::min(3.0, clipRect.height() / 8);
    return clipRect.adjusted(0, top, 0, -bottom);
}

void TimelineView::revealAudioTracks() {
    for (const Row& row : m_rows) {
        if (row.audio) {
            setScrollY(std::max(0.0, row.y - m_style.trackGap));
            return;
        }
    }
}

void TimelineView::setShowVolume(bool show) {
    if (m_showVolume == show)
        return;
    m_showVolume = show;
    m_pendingAll = true;
    emit showVolumeChanged();
    update();
}

TimelineView::Hit TimelineView::volumeHit(double x, double y, Hit hit) const {
    if (!volumeBandShown() || (m_tool != TimelineTool::Select && m_tool != TimelineTool::Pen) || !hit.clip || hit.row < 0)
        return hit;
    const Row& row = m_rows[static_cast<std::size_t>(hit.row)];
    if (!row.audio && m_tool != TimelineTool::Pen)
        return hit; // video clips' opacity band: the pen only (0.2)
    const auto index = row.track->indexOf(hit.clip);
    if (!index)
        return hit;
    const Clip& clip = row.track->clips[*index];
    const QRectF rect(xOfTicks(clip.start()), row.y - m_scrollY, xOfTicks(clip.end()) - xOfTicks(clip.start()), row.height);
    const QRectF area = volumeArea(rect);
    const auto yOf = [&](double v) { return area.top() + bandFraction(row.audio, v) * area.height(); };
    const ParamValue& volume = clip.properties()[bandParam(row.audio)];
    if (const Curve* curve = volume.curve()) {
        for (std::size_t i = 0; i < curve->keys.size(); ++i) {
            const auto t = clip.timelineTimeOfCurveTime(curve->keys[i].time);
            if (!t)
                continue;
            if (std::abs(xOfTicks(*t) - x) <= kKeyReach && std::abs(yOf(curve->keys[i].value) - y) <= kKeyReach) {
                hit.zone = Zone::VolumeKey;
                hit.volumeKey = static_cast<int>(i);
                return hit;
            }
        }
    }
    if (hit.zone != Zone::Body)
        return hit; // the trim edges keep their zones
    const Ticks t = std::clamp(ticksAtX(x), clip.start(), clip.end() - Ticks{1});
    if (std::abs(yOf(volume.valueAt(clip.curveTimeAt(t))) - y) <= kLineReach)
        hit.zone = Zone::VolumeLine;
    return hit;
}

void TimelineView::pressVolume(const Hit& hit, Qt::KeyboardModifiers mods) {
    const Row& row = m_rows[static_cast<std::size_t>(hit.row)];
    const auto index = row.track->indexOf(hit.clip);
    if (!index || !m_controller)
        return;
    const Clip& clip = row.track->clips[*index];
    if (!m_controller->isSelected(hit.clip))
        m_controller->selectClipById(hit.clip, ProjectController::Replace, true);
    const bool audio = row.audio;
    const ParamValue volume = clip.properties()[bandParam(audio)];
    const SequenceId seq = m_controller->currentSequenceId();
    const ClipId clipId = clip.id;

    // Alt-click on a key: Bezier (smooth tangents) <-> linear.
    if (hit.zone == Zone::VolumeKey && (mods & Qt::AltModifier) && volume.curve()) {
        Curve curve = *volume.curve();
        Keyframe& key = curve.keys[static_cast<std::size_t>(hit.volumeKey)];
        if (key.interpolation == Interpolation::Bezier) {
            key.interpolation = Interpolation::Linear;
        } else {
            const double slope = smoothSlope(curve, static_cast<std::size_t>(hit.volumeKey));
            key.interpolation = Interpolation::Bezier;
            key.in = BezierHandle{slope, 1.0 / 3.0};
            key.out = BezierHandle{slope, 1.0 / 3.0};
        }
        m_controller->applyEditFn(LABEL("Keyframe Interpolation"), [&](const ProjectState& state) {
            return edit::setParameter(state, seq, clipId, bandRef(audio), ParamValue(curve));
        });
        m_drag = Drag::None;
        return;
    }
    const Ticks t = std::clamp(ticksAtX(m_pressPos.x()), clip.start(), clip.end() - Ticks{1});
    m_volumeDrag = {clipId,
                    hit.zone == Zone::VolumeKey ? hit.volumeKey : -1,
                    volume,
                    t,
                    volume.valueAt(clip.curveTimeAt(t)),
                    false,
                    "volume:" + std::to_string(++m_volumeDragCounter)};
    m_drag = hit.zone == Zone::VolumeKey ? Drag::VolumeKey : Drag::VolumeLine;
}

void TimelineView::dragVolume(QPointF pos, Qt::KeyboardModifiers mods) {
    if (!m_controller)
        return;
    if (!m_volumeDrag.moved
        && (pos - m_pressPos).manhattanLength() < QGuiApplication::styleHints()->startDragDistance())
        return;
    m_volumeDrag.moved = true;
    const int rowIndex = m_pressHit.row;
    if (rowIndex < 0 || rowIndex >= static_cast<int>(m_rows.size()))
        return;
    const Row& row = m_rows[static_cast<std::size_t>(rowIndex)];
    const auto index = row.track->indexOf(m_volumeDrag.clip);
    if (!index)
        return;
    const Clip& clip = row.track->clips[*index];
    const QRectF rect(xOfTicks(clip.start()), row.y - m_scrollY, xOfTicks(clip.end()) - xOfTicks(clip.start()), row.height);
    const QRectF area = volumeArea(rect);
    const bool audio = row.audio;
    const double db = bandValueAtFraction(audio, (pos.y() - area.top()) / std::max(1.0, area.height()));
    const SequenceId seq = m_controller->currentSequenceId();
    const ClipId clipId = clip.id;
    ParamValue next;
    if (m_volumeDrag.key >= 0 && m_volumeDrag.original.curve()) {
        // A key: level, and time between its neighbours (frame-quantised) unless Shift keeps it.
        Curve curve = *m_volumeDrag.original.curve();
        const auto i = static_cast<std::size_t>(m_volumeDrag.key);
        curve.keys[i].value = db;
        if (!(mods & Qt::ShiftModifier)) {
            const Rate rate = editRate(std::span(&clipId, 1)); // samples on audio with audio time units (0.2)
            const Ticks frame = framesToTicks(1, rate);
            const Ticks snapped = snapToFrame(ticksAtX(pos.x()), rate);
            Ticks curveTime = clip.curveTimeAt(std::clamp(snapped, clip.start(), clip.end() - Ticks{1}));
            if (i > 0)
                curveTime = std::max(curveTime, curve.keys[i - 1].time + frame);
            if (i + 1 < curve.keys.size())
                curveTime = std::min(curveTime, curve.keys[i + 1].time - frame);
            curve.keys[i].time = curveTime;
        }
        next = ParamValue(curve);
    } else if (const Curve* original = m_volumeDrag.original.curve()) {
        // The line between two keys: both move by the same level.
        Curve curve = *original;
        const Ticks at = clip.curveTimeAt(m_volumeDrag.pressTime);
        const double delta = db - m_volumeDrag.pressDb;
        const std::size_t after = curve.insertionIndex(at);
        if (after > 0)
            curve.keys[after - 1].value += delta;
        if (after < curve.keys.size())
            curve.keys[after].value += delta;
        for (Keyframe& k : curve.keys)
            k.value = clampBand(audio, k.value);
        next = ParamValue(curve);
    } else {
        next = ParamValue(clampBand(audio, db));
    }
    const char* label = m_volumeDrag.key >= 0 ? (audio ? LABEL("Volume Keyframe") : LABEL("Opacity Keyframe"))
                                              : (audio ? LABEL("Volume") : LABEL("Opacity"));
    m_controller->applyEditFn(
        label, [&](const ProjectState& state) { return edit::setParameter(state, seq, clipId, bandRef(audio), next); },
        m_volumeDrag.mergeKey);
}

void TimelineView::releaseVolume() {
    if (!m_controller)
        return;
    if (m_volumeDrag.moved) {
        m_controller->endPropertyGesture(); // one undo step per drag
    } else if (m_drag == Drag::VolumeLine) {
        // A click on the line: a key there, at the volume the clip has there.
        const SequencePtr seqPtr = m_controller->currentSequence();
        const auto loc = seqPtr ? seqPtr->findClip(m_volumeDrag.clip) : std::nullopt;
        if (loc) {
            const Clip& clip = loc->clip();
            const bool audio = loc->track->kind == TrackKind::Audio;
            const Ticks t = std::clamp(snapToFrame(m_volumeDrag.pressTime, editRate(std::span(&clip.id, 1))), clip.start(),
                                       clip.end() - Ticks{1});
            Keyframe key;
            key.time = clip.curveTimeAt(t);
            key.value = m_volumeDrag.original.valueAt(key.time);
            const SequenceId seq = seqPtr->id;
            const ClipId clipId = clip.id;
            m_controller->applyEditFn(audio ? LABEL("Add Volume Keyframe") : LABEL("Add Opacity Keyframe"),
                                      [&](const ProjectState& state) {
                                          return edit::addKeyframe(state, seq, clipId, bandRef(audio), key);
                                      });
        }
    }
    m_volumeDrag = {};
}

} // namespace wabcut::app
