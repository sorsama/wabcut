#include "EffectControls.h"

#include "CommandPalette.h"
#include "EditingBridge.h"
#include "EffectPresetsBridge.h"
#include "ProjectController.h"

#include <wabcut/model/Animations.h>
#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/Clipboard.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/GraphicResolve.h>
#include <wabcut/model/MasterClipEffects.h>
#include <wabcut/model/TitleOps.h>

#include <QColor>
#include <QCoreApplication>
#include <QFontDatabase>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <tuple>

namespace wabcut::app {

namespace {

#define LABEL(text) QT_TRANSLATE_NOOP("wabcut::app::ProjectController", text)

QString tx(const char* text) {
    return QCoreApplication::translate("wabcut::app::EffectControlsModel", text);
}

QString fromView(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString interpolationName(Interpolation i) {
    return fromView(toString(i));
}

bool isTransitionKind(EffectKind kind) {
    return kind == EffectKind::VideoTransition || kind == EffectKind::AudioTransition;
}

/// 0.6 (card #71): a parameter's value as the clip plays it: ProjectController::parameterValue, or, for an effect of the
/// clip's media's Master Clip Effects (not in the clip's own stack), the value there.
ParamValue paramValueOf(const ProjectController* controller, const Clip& clip, const edit::ParamRef& ref) {
    if (!ref.intrinsic && ref.layer == 0 && controller &&
        std::none_of(clip.effects().begin(), clip.effects().end(), [&](const EffectInstance& e) { return e.id == ref.effect; })) {
        const ProjectSnapshot snapshot = controller->snapshot();
        if (const MediaItem* holder = snapshot ? masterEffectHolder(*snapshot, clip.media, ref.effect) : nullptr) {
            for (const EffectInstance& e : holder->masterEffects) {
                if (e.id != ref.effect)
                    continue;
                if (const ParamValue* value = e.find(ref.key))
                    return *value;
                if (const EffectDescriptor* d = EffectRegistry::builtin().find(e.effect)) {
                    if (const EffectParamInfo* info = d->param(ref.key))
                        return ParamValue(info->defaultValue);
                }
            }
        }
    }
    return ProjectController::parameterValue(clip, ref);
}

/// Runs edit steps one after the other on the evolving state, as one edit (changes against the first state).
edit::EditOutcome chainedEdit(const ProjectState& state, const std::vector<std::function<edit::EditOutcome(const ProjectState&)>>& steps) {
    std::optional<edit::EditResult> last;
    for (const auto& step : steps) {
        auto next = step(last ? last->state : state);
        if (!next)
            return next;
        std::vector<EffectInstanceId> created = last ? last->createdEffects : std::vector<EffectInstanceId>{};
        created.insert(created.end(), next->createdEffects.begin(), next->createdEffects.end());
        last = std::move(*next);
        last->createdEffects = std::move(created);
    }
    if (!last) {
        edit::EditResult unchanged;
        unchanged.state = state;
        return unchanged;
    }
    last->changes = diff(state, last->state);
    return std::move(*last);
}

/// Icon of a library category.
QString categoryIcon(std::string_view category, EffectKind kind) {
    if (isTransitionKind(kind))
        return category == "Wipe" ? QStringLiteral("fold-horizontal") : QStringLiteral("blend");
    // Premiere's bins (0.2).
    if (category == "Color Correction" || category == "Adjust" || category == "Image Control" || category == "Channel")
        return QStringLiteral("sun");
    if (category == "Blur & Sharpen" || category == "Noise & Grain")
        return QStringLiteral("droplet");
    if (category == "Transform" || category == "Distort")
        return QStringLiteral("crop");
    if (category == "Perspective")
        return QStringLiteral("square");
    if (category == "Amplitude")
        return QStringLiteral("volume-2");
    if (category == "Dynamics")
        return QStringLiteral("gauge");
    return kind == EffectKind::Audio ? QStringLiteral("audio-lines") : QStringLiteral("sparkles");
}

const EffectDescriptor* descriptor(std::string_view id) {
    return EffectRegistry::builtin().find(id);
}

/// The Essential Sound panel's record of a clip (audio::edit::kEssentialSoundEffect): it does nothing to the sound.
constexpr std::string_view kEssentialSoundRecord = "wabcut.audio.essentialSound";

/// The clips of `selection` (existing in `seq`) that take an item of `kind` (Video / Audio): each selected clip of that
/// kind, else its first linked partner of that kind; in selection order, once each.
std::vector<ClipId> clipsOfKind(const Sequence& seq, const std::vector<ClipId>& selection, StreamKind kind) {
    std::vector<ClipId> out;
    const auto add = [&](ClipId id) {
        if (std::find(out.begin(), out.end(), id) == out.end())
            out.push_back(id);
    };
    for (const ClipId id : selection) {
        const auto loc = seq.findClip(id);
        if (!loc)
            continue;
        if (loc->clip().stream.kind == kind) {
            add(id);
            continue;
        }
        const ClipId ids[] = {id};
        for (const ClipId partner : edit::withLinkedClips(seq, ids)) {
            const auto p = seq.findClip(partner);
            if (p && p->clip().stream.kind == kind) {
                add(partner);
                break;
            }
        }
    }
    return out;
}

/// 0.6: the selected clips (in `seq`) each with its linked partners, once each: several of them is Effect Controls'
/// multi mode.
std::vector<std::vector<ClipId>> multiUnits(const Sequence& seq, const std::vector<ClipId>& selection) {
    std::vector<std::vector<ClipId>> units;
    std::set<ClipId> seen;
    for (const ClipId id : selection) {
        if (seen.contains(id) || !seq.findClip(id))
            continue;
        std::vector<ClipId> unit{id};
        seen.insert(id);
        const ClipId ids[] = {id};
        for (const ClipId partner : edit::withLinkedClips(seq, ids)) {
            if (!seen.contains(partner) && seq.findClip(partner)) {
                seen.insert(partner);
                unit.push_back(partner);
            }
        }
        units.push_back(std::move(unit));
    }
    return units;
}

/// Runs several edits on one state (one undo step): each sees the previous one's result.
edit::EditOutcome chain(const ProjectState& state,
                        const std::vector<std::function<edit::EditOutcome(const ProjectState&)>>& steps) {
    std::optional<edit::EditResult> last;
    for (const auto& step : steps) {
        auto r = step(last ? last->state : state);
        if (!r)
            return r;
        last = std::move(*r);
    }
    if (!last)
        return std::unexpected(Error{ErrorCode::InvalidArgument, "nothing to change"});
    return std::move(*last);
}

Qt::KeyboardModifiers mods(int modifiers) {
    return Qt::KeyboardModifiers(modifiers);
}

} // namespace

bool isListedEffect(const EffectDescriptor& d) {
    return !d.intrinsic && d.id != edit::kTitleStyleEffect && d.id != kEssentialSoundRecord;
}

QString effectLibraryIcon(const EffectDescriptor& d) {
    if (d.id.starts_with(kPluginEffectPrefix))
        return QStringLiteral("sliders-horizontal");
    return categoryIcon(d.category, d.kind);
}

// ---- Rows -------------------------------------------------------------------------------------------

bool EffectControlsModel::Row::sameData(const Row& o) const {
    return key == o.key && kind == o.kind && section == o.section && sectionKind == o.sectionKind && title == o.title
        && icon == o.icon && unit == o.unit && text == o.text && description == o.description && stackKind == o.stackKind
        && from == o.from && to == o.to && step == o.step && decimals == o.decimals && origin == o.origin
        && value == o.value && defaultValue == o.defaultValue && animatable == o.animatable && animated == o.animated
        && keyAtPlayhead == o.keyAtPlayhead && hasPrev == o.hasPrev && hasNext == o.hasNext
        && effectEnabled == o.effectEnabled && collapsed == o.collapsed && removable == o.removable
        && movable == o.movable && resettable == o.resettable && known == o.known && stackIndex == o.stackIndex
        && keys == o.keys && options == o.options && graphOpen == o.graphOpen && graph == o.graph
        && sectionSelected == o.sectionSelected && mixed == o.mixed;
}

/// 0.4: what Copy / Cut put on the effects clipboard - a snapshot of the shown video and audio clips (their sections
/// as they were) and which of their attributes were selected, for pasteAttributes onto other clips.
struct EffectControlsModel::EffectsClipboard {
    std::optional<Clip> video, audio;
    AttributeSet videoAttributes, audioAttributes;
    int count = 0; ///< sections copied
};

EffectControlsModel::EffectControlsModel(ProjectController* controller)
    : QAbstractListModel(controller), m_controller(controller) {
    connect(controller, &ProjectController::revisionChanged, this, &EffectControlsModel::onRevision);
    connect(controller, &ProjectController::selectionChanged, this, &EffectControlsModel::refresh);
    connect(controller, &ProjectController::selectedTransitionChanged, this, &EffectControlsModel::refresh);
    connect(controller, &ProjectController::playheadChanged, this, &EffectControlsModel::refresh);
    // 0.4: a clip copy after an effects copy is the newest copy: Paste pastes clips again.
    connect(controller, &ProjectController::clipboardChanged, this, [this] {
        setEffectsPending(false);
        setKeysPending(false); // 0.6: the newest copy wins
    });
}

int EffectControlsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QHash<int, QByteArray> EffectControlsModel::roleNames() const {
    return {
        {KeyRole, "rowKey"},          {KindRole, "kind"},
        {SectionRole, "section"},     {SectionKindRole, "sectionKind"},
        {TitleRole, "title"},         {IconRole, "icon"},
        {UnitRole, "unit"},           {FromRole, "from"},
        {ToRole, "to"},               {StepRole, "step"},
        {DecimalsRole, "decimals"},   {OriginRole, "origin"},
        {ValueRole, "value"},         {DefaultValueRole, "defaultValue"},
        {AnimatableRole, "animatable"}, {AnimatedRole, "animated"},
        {KeyAtPlayheadRole, "keyAtPlayhead"}, {HasPreviousKeyRole, "hasPreviousKey"},
        {HasNextKeyRole, "hasNextKey"}, {KeysRole, "keys"},
        {EffectEnabledRole, "effectEnabled"}, {CollapsedRole, "collapsed"},
        {RemovableRole, "removable"}, {MovableRole, "movable"},
        {ResettableRole, "resettable"}, {StackIndexRole, "stackIndex"},
        {StackKindRole, "stackKind"}, {TextRole, "text"},
        {OptionsRole, "options"},     {DescriptionRole, "description"},
        {KnownRole, "known"},         {GraphOpenRole, "graphOpen"},
        {GraphRole, "graph"},         {SectionSelectedRole, "sectionSelected"},
        {MixedRole, "mixed"},
    };
}

QVariant EffectControlsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
        return {};
    const Row& r = m_rows[static_cast<std::size_t>(index.row())];
    switch (role) {
    case KeyRole: return r.key;
    case KindRole: return r.kind;
    case SectionRole: return r.section;
    case SectionKindRole: return r.sectionKind;
    case TitleRole: return r.title;
    case IconRole: return r.icon;
    case UnitRole: return r.unit;
    case FromRole: return r.from;
    case ToRole: return r.to;
    case StepRole: return r.step;
    case DecimalsRole: return static_cast<int>(r.decimals);
    case OriginRole: return r.origin;
    case ValueRole: return r.value;
    case DefaultValueRole: return r.defaultValue;
    case AnimatableRole: return r.animatable;
    case AnimatedRole: return r.animated;
    case KeyAtPlayheadRole: return r.keyAtPlayhead;
    case HasPreviousKeyRole: return r.hasPrev;
    case HasNextKeyRole: return r.hasNext;
    case KeysRole: return r.keys;
    case EffectEnabledRole: return r.effectEnabled;
    case CollapsedRole: return r.collapsed;
    case RemovableRole: return r.removable;
    case MovableRole: return r.movable;
    case ResettableRole: return r.resettable;
    case StackIndexRole: return r.stackIndex;
    case StackKindRole: return r.stackKind;
    case TextRole: return r.text;
    case OptionsRole: return r.options;
    case DescriptionRole: return r.description;
    case KnownRole: return r.known;
    case GraphOpenRole: return r.graphOpen;
    case GraphRole: return r.graph;
    case SectionSelectedRole: return r.sectionSelected;
    case MixedRole: return r.mixed;
    default: return {};
    }
}

int EffectControlsModel::rowOf(const QString& key) const {
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].key == key)
            return static_cast<int>(i);
    }
    return -1;
}

QVariantMap EffectControlsModel::row(const QString& key) const {
    const int i = rowOf(key);
    if (i < 0)
        return {};
    QVariantMap map;
    const QHash<int, QByteArray> names = roleNames();
    for (auto it = names.cbegin(); it != names.cend(); ++it)
        map.insert(QString::fromLatin1(it.value()), data(index(i), it.key()));
    return map;
}

QString EffectControlsModel::keyAt(int index) const {
    return index >= 0 && index < rowCount() ? m_rows[static_cast<std::size_t>(index)].key : QString();
}

int EffectControlsModel::stackIndexForRow(int index, const QString& stackKind) const {
    int first = -1, last = -1, count = 0;
    for (int i = 0; i < rowCount(); ++i) {
        const Row& r = m_rows[static_cast<std::size_t>(i)];
        if (r.kind != QLatin1String("header") || r.stackKind != stackKind)
            continue;
        if (first < 0)
            first = i;
        last = i;
        ++count;
    }
    if (count == 0)
        return 0;
    if (index < first)
        return 0;
    // The effect section that holds row `index` (its header is the last header at or above it).
    for (int i = std::min(index, rowCount() - 1); i >= 0; --i) {
        const Row& r = m_rows[static_cast<std::size_t>(i)];
        if (r.kind == QLatin1String("header")) {
            if (r.stackKind == stackKind)
                return r.stackIndex;
            break; // another section (intrinsic or the other kind's stack)
        }
    }
    return index > last ? count - 1 : 0;
}

const EffectControlsModel::Row* EffectControlsModel::findRow(const QString& key) const {
    const int i = rowOf(key);
    return i < 0 ? nullptr : &m_rows[static_cast<std::size_t>(i)];
}

const Clip* EffectControlsModel::clipOf(const Row& row) const {
    if (!m_controller || !row.clip)
        return nullptr;
    const SequencePtr seq = m_controller->currentSequence();
    const auto loc = seq ? seq->findClip(row.clip) : std::nullopt;
    // The sequence outlives this call: the session keeps the snapshot alive until the next edit.
    return loc ? &loc->clip() : nullptr;
}

std::optional<Ticks> EffectControlsModel::keyTimelineTime(const Clip& clip, Ticks curveTime) const {
    const auto t = clip.timelineTimeOfCurveTime(curveTime);
    if (!t || *t < m_lane.start || *t >= m_lane.end())
        return std::nullopt;
    return t;
}

std::vector<std::pair<std::size_t, Ticks>> EffectControlsModel::visibleKeys(const Row& row, const Curve& curve,
                                                                             const Clip& clip) const {
    std::vector<std::pair<std::size_t, Ticks>> out;
    for (std::size_t i = 0; i < curve.keys.size(); ++i) {
        if (row.ref.layer == 0) {
            if (const auto t = keyTimelineTime(clip, curve.keys[i].time))
                out.emplace_back(i, *t);
        } else if (const auto t = rowTimelineTime(row, clip, curve.keys[i].time);
                   t && *t >= m_lane.start && *t < m_lane.end()) {
            out.emplace_back(i, *t); // 0.6: a graphic layer's key (graphic time)
        }
    }
    return out;
}

// ---- 0.6: graphic time ------------------------------------------------------------------------------

Ticks EffectControlsModel::rowCurveTime(const Row& row, const Clip& clip, Ticks t) const {
    const Graphic* graphic = row.ref.layer != 0 ? clip.graphic() : nullptr;
    if (graphic && graphic->responsive)
        return graphicTimeAt(clip, *graphic, std::clamp(t, clip.start(), std::max(clip.start(), clip.end() - Ticks{1})));
    return clip.curveTimeAt(t);
}

Ticks EffectControlsModel::rowCurveTimeAtPlayhead(const Row& row, const Clip& clip) const {
    const Graphic* graphic = row.ref.layer != 0 ? clip.graphic() : nullptr;
    if (graphic && graphic->responsive)
        return rowCurveTime(row, clip, m_controller->playhead());
    return m_controller->curveTimeAtPlayhead(clip);
}

std::optional<Ticks> EffectControlsModel::rowTimelineTime(const Row& row, const Clip& clip, Ticks curveTime) const {
    const Graphic* graphic = row.ref.layer != 0 ? clip.graphic() : nullptr;
    if (!graphic || !graphic->responsive)
        return clip.timelineTimeOfCurveTime(curveTime);
    // Responsive time: design time never runs backwards along the clip, so the first timeline tick that reaches it.
    Ticks lo = clip.start();
    Ticks hi = clip.end() - Ticks{1};
    if (hi < lo || graphicTimeAt(clip, *graphic, lo) > curveTime || graphicTimeAt(clip, *graphic, hi) < curveTime)
        return std::nullopt;
    while (lo < hi) {
        const Ticks mid{lo.value + (hi.value - lo.value) / 2};
        if (graphicTimeAt(clip, *graphic, mid) < curveTime)
            lo = Ticks{mid.value + 1};
        else
            hi = mid;
    }
    return lo;
}

void EffectControlsModel::addParamRow(std::vector<Row>& rows, const Clip& clip, const QString& key,
                                      const QString& section, const QString& sectionKind, const edit::ParamRef& ref,
                                      const QString& title, const QString& icon, const UiRange& range,
                                      double defaultModel, bool animatable, bool enabled, std::string undoLabel) {
    Row r;
    r.key = key;
    r.kind = range.toggle ? QStringLiteral("toggle") : QStringLiteral("param");
    r.section = section;
    r.sectionKind = sectionKind;
    r.title = title;
    r.icon = icon;
    r.unit = unitText(range);
    r.range = range;
    r.frame = m_controller->propertyFrame(clip);
    const double origin = originOf(range.origin, r.frame);
    r.from = range.from + origin;
    r.to = range.to + origin;
    r.step = range.step;
    r.decimals = range.decimals;
    r.origin = range.bipolar ? 0.0 : r.from;
    r.ref = ref;
    const ParamValue value = paramValueOf(m_controller, clip, ref);
    const Ticks curveTime = rowCurveTimeAtPlayhead(r, clip);
    r.value = range.toUi(value.valueAt(curveTime), r.frame);
    r.defaultValue = range.toUi(defaultModel, r.frame);
    r.animatable = animatable;
    r.animated = value.isCurve();
    r.resettable = true;
    r.effectEnabled = enabled;
    r.clip = clip.id;
    r.ref = ref;
    r.undoLabel = std::move(undoLabel);
    if (const Curve* curve = value.curve()) {
        const Ticks playhead = m_controller->playhead();
        const bool inside = playhead >= clip.start() && playhead < clip.end();
        r.keyAtPlayhead = inside && curve->keyAt(curveTime).has_value();
        const double laneLength = static_cast<double>(std::max<std::int64_t>(1, m_lane.duration.value));
        for (const auto& [i, t] : visibleKeys(r, *curve, clip)) {
            const Keyframe& k = curve->keys[i];
            m_shownKeys.insert(KeyRef{key, k.time.value});
            r.keys.append(QVariantMap{
                {QStringLiteral("x"), static_cast<double>((t - m_lane.start).value) / laneLength},
                {QStringLiteral("selected"), m_selectedKeys.contains(KeyRef{key, k.time.value})},
                {QStringLiteral("shape"), interpolationName(k.interpolation)},
            });
            r.hasPrev = r.hasPrev || t < playhead;
            r.hasNext = r.hasNext || t > playhead;
        }
        r.graphOpen = m_graphRows.contains(key);
        if (r.graphOpen)
            r.graph = graphOf(r, *curve, clip);
    }
    rows.push_back(std::move(r));
}

QVariantMap EffectControlsModel::graphOf(const Row& r, const Curve& curve, const Clip& clip) const {
    constexpr int kSamples = 96;
    const double laneLength = static_cast<double>(std::max<std::int64_t>(1, m_lane.duration.value));
    const auto ui = [&](double model) { return r.range.toUi(model, r.frame); };
    const auto xOf = [&](Ticks curveTime) -> std::optional<double> {
        const auto t = rowTimelineTime(r, clip, curveTime);
        if (!t)
            return std::nullopt;
        return static_cast<double>((*t - m_lane.start).value) / laneLength;
    };
    const auto ticksOf = [](double seconds) {
        return Ticks{static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(kTicksPerSecond)))};
    };

    // The curve across the lane, then the keys and their handles (UI units). 0.5.1: the path covers the part of the
    // clip the lane shows (as finely zoomed as unzoomed); the value range is fitted to the whole clip, so it stays put
    // while the lane pans.
    const auto sampleAcross = [&](double from, double to) {
        std::vector<std::pair<double, double>> out;
        out.reserve(kSamples + 1);
        for (int i = 0; i <= kSamples; ++i) {
            const double x = from + (to - from) * static_cast<double>(i) / kSamples;
            const Ticks t{m_lane.start.value + static_cast<std::int64_t>(std::llround(x * laneLength))};
            out.emplace_back(x, ui(curve.valueAt(rowCurveTime(r, clip, t))));
        }
        return out;
    };
    const std::vector<std::pair<double, double>> samples = sampleAcross(0.0, 1.0);
    const std::vector<std::pair<double, double>> shown = zoomed() ? sampleAcross(m_viewStart, m_viewEnd) : samples;
    struct Point {
        std::size_t index;
        double x, v;
        const Keyframe* key;
        std::optional<std::pair<double, double>> in, out;
    };
    std::vector<Point> points;
    for (const auto& [i, t] : visibleKeys(r, curve, clip)) {
        const Keyframe& k = curve.keys[i];
        Point p{i, static_cast<double>((t - m_lane.start).value) / laneLength, ui(k.value), &k, {}, {}};
        if (k.interpolation == Interpolation::Bezier) {
            // The control points the evaluator uses: influence of the segment along, slope * that time up.
            if (i + 1 < curve.keys.size()) {
                const double dt = k.out.influence * (curve.keys[i + 1].time - k.time).toSeconds();
                if (const auto x = xOf(k.time + ticksOf(dt)))
                    p.out = std::make_pair(*x, ui(k.value + k.out.slope * dt));
            }
            if (i > 0) {
                const double dt = k.in.influence * (k.time - curve.keys[i - 1].time).toSeconds();
                if (const auto x = xOf(k.time - ticksOf(dt)))
                    p.in = std::make_pair(*x, ui(k.value - k.in.slope * dt));
            }
        }
        points.push_back(p);
    }

    // The value range: frozen while one of this row's handles is dragged, else fitted with a margin.
    double lo = 0;
    double hi = 1;
    if (m_handleDrag.active && m_handleDrag.row == r.key) {
        lo = m_handleDrag.min;
        hi = m_handleDrag.max;
    } else {
        lo = std::numeric_limits<double>::infinity();
        hi = -lo;
        const auto take = [&](double v) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        };
        for (const auto& sample : samples)
            take(sample.second);
        for (const Point& p : points) {
            take(p.v);
            if (p.in)
                take(p.in->second);
            if (p.out)
                take(p.out->second);
        }
        const double minSpan = std::max(std::abs(r.to - r.from) * 0.05, r.step);
        if (hi - lo < minSpan) {
            const double mid = (lo + hi) / 2;
            lo = mid - minSpan / 2;
            hi = mid + minSpan / 2;
        }
        const double pad = (hi - lo) * 0.12;
        lo -= pad;
        hi += pad;
    }
    const auto yOf = [&](double v) { return (hi - v) / (hi - lo); };

    QVariantList path;
    path.reserve(static_cast<qsizetype>(shown.size() * 2));
    for (const auto& sample : shown) {
        path.append(sample.first);
        path.append(yOf(sample.second));
    }
    QVariantList keys;
    for (const Point& p : points) {
        const Keyframe& k = *p.key;
        const bool both = p.in && p.out;
        const bool broken = both && std::abs(k.in.slope - k.out.slope) > 1e-9 * std::max(1.0, std::abs(k.out.slope));
        keys.append(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(p.index)},
            {QStringLiteral("x"), p.x},
            {QStringLiteral("y"), yOf(p.v)},
            {QStringLiteral("selected"), m_selectedKeys.contains(KeyRef{r.key, k.time.value})},
            {QStringLiteral("shape"), interpolationName(k.interpolation)},
            {QStringLiteral("hasIn"), p.in.has_value()},
            {QStringLiteral("inX"), p.in ? p.in->first : p.x},
            {QStringLiteral("inY"), p.in ? yOf(p.in->second) : yOf(p.v)},
            {QStringLiteral("hasOut"), p.out.has_value()},
            {QStringLiteral("outX"), p.out ? p.out->first : p.x},
            {QStringLiteral("outY"), p.out ? yOf(p.out->second) : yOf(p.v)},
            {QStringLiteral("broken"), broken},
        });
    }
    return {{QStringLiteral("path"), path}, {QStringLiteral("keys"), keys},
            {QStringLiteral("min"), lo},     {QStringLiteral("max"), hi}};
}

void EffectControlsModel::makeChoice(Row& r, const EffectParamInfo& p) {
    r.kind = QStringLiteral("choice");
    r.options.clear();
    const std::vector<std::string_view> labels = p.optionLabels();
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const double value = r.range.toUi(p.minValue + static_cast<double>(i), r.frame);
        r.options.append(QVariantMap{{QStringLiteral("value"), QString::number(value)},
                                     {QStringLiteral("label"), tx(std::string(labels[i]).c_str())}});
    }
    r.text = QString::number(std::round(r.value));
}

bool EffectControlsModel::isValueRow(const Row& r) {
    return r.kind == QLatin1String("param") || r.kind == QLatin1String("toggle") ||
           (r.kind == QLatin1String("choice") && (!r.ref.key.empty() || !r.textProp.isEmpty()));
}

void EffectControlsModel::addClipSections(std::vector<Row>& rows, const Clip& clip, bool video) {
    const auto header = [&](const QString& section, const QString& sectionKind, const QString& title,
                            const QString& icon) -> Row& {
        Row h;
        h.key = QStringLiteral("h:") + section;
        h.kind = QStringLiteral("header");
        h.section = section;
        h.sectionKind = sectionKind;
        h.title = title;
        h.icon = icon;
        h.collapsed = m_collapsed.contains(section);
        h.resettable = true;
        h.clip = clip.id;
        h.sectionSelected = m_selectedSections.contains(section);
        rows.push_back(std::move(h));
        return rows.back();
    };
    const auto intrinsic = [&](const QString& section, PropertyRole role) {
        if (m_collapsed.contains(section))
            return;
        for (const UiParameter& p : uiParameters()) {
            const ParameterInfo& info = parameterInfo(p.id);
            if (info.role != role)
                continue;
            addParamRow(rows, clip, QStringLiteral("p:") + QLatin1String(p.key), section, QStringLiteral("intrinsic"),
                        edit::ParamRef::of(p.id), QCoreApplication::translate("wabcut::app::ParameterUi", p.label),
                        QLatin1String(p.icon), p.range, info.defaultValue, true, true, p.undoLabel);
        }
    };
    if (video) {
        addTextSections(rows, clip); // 0.6: a graphic's text layers, above its Transform
        header(QStringLiteral("motion"), QStringLiteral("intrinsic"), tx("Transform"), QStringLiteral("scaling"));
        intrinsic(QStringLiteral("motion"), PropertyRole::Motion);
        header(QStringLiteral("opacity"), QStringLiteral("intrinsic"), tx("Opacity"), QStringLiteral("blend"));
        intrinsic(QStringLiteral("opacity"), PropertyRole::Opacity);
    } else {
        header(QStringLiteral("audio"), QStringLiteral("intrinsic"), tx("Audio"), QStringLiteral("volume-2"));
        intrinsic(QStringLiteral("audio"), PropertyRole::Audio);
    }

    // 0.6 (card #71): the media's Master Clip Effects, under a "Master Clip" caption above the clip's own effects (they
    // play first). Edits go through the clip's operations, which act on the master stack (FinishingOps.h).
    const ProjectSnapshot snapshot = m_controller ? m_controller->snapshot() : nullptr;
    const MediaItem* masterItem = snapshot && !clip.isNested() && clip.angles.empty() ? snapshot->findMedia(clip.media) : nullptr;
    if (masterItem) {
        const EffectKind kind = video ? EffectKind::Video : EffectKind::Audio;
        const EffectStack master = masterEffectsOf(*snapshot, clip.media, kind);
        if (!master.empty()) {
            const bool off = clip.content().masterEffectsOff;
            Row g;
            g.key = QStringLiteral("master:") + QString::number(clip.id.value);
            g.kind = QStringLiteral("group");
            g.section = QStringLiteral("master");
            g.sectionKind = QStringLiteral("master");
            g.title = off ? tx("Master Clip: %1 (off for this clip)").arg(QString::fromStdString(masterItem->name))
                          : tx("Master Clip: %1").arg(QString::fromStdString(masterItem->name));
            g.text = off ? QStringLiteral("masterOn") : QStringLiteral("masterOff");
            g.description = tx("Effects on the master clip play on every clip of %1, before the clip's own effects.")
                                .arg(QString::fromStdString(masterItem->name));
            g.clip = clip.id;
            g.effectEnabled = !off;
            rows.push_back(std::move(g));
            for (std::size_t i = 0; i < master.size(); ++i) {
                const EffectInstance& instance = master[i];
                const EffectDescriptor* d = descriptor(instance.effect);
                const QString section = effectKey(instance.id);
                Row& h = header(section, QStringLiteral("effect"), d ? tx(d->displayName.data()) : QString::fromStdString(instance.effect),
                                d ? categoryIcon(d->category, d->kind) : QStringLiteral("sparkles"));
                h.effectEnabled = instance.enabled && !off;
                h.removable = true;
                h.movable = true;
                h.resettable = d != nullptr;
                h.stackIndex = static_cast<int>(i);
                h.stackKind = QStringLiteral("master");
                h.known = isEvaluable(instance, kind);
                h.description = tx("Master clip effect: every clip of %1 has it.").arg(QString::fromStdString(masterItem->name));
                if (!d || m_collapsed.contains(section))
                    continue;
                for (const EffectParamInfo& p : d->params) {
                    addParamRow(rows, clip, section + QLatin1Char(':') + fromView(p.key), section, QStringLiteral("effect"),
                                edit::ParamRef::ofEffect(instance.id, std::string(p.key)), tx(p.displayName.data()), QString(),
                                uiRange(p), p.defaultValue, p.animatable, instance.enabled && !off,
                                std::string("Master Clip: ") + std::string(d->displayName) + ": " + std::string(p.displayName));
                    if (p.isChoice())
                        makeChoice(rows.back(), p);
                }
            }
        }
    }

    const EffectStack& stack = clip.effects();
    for (std::size_t i = 0; i < stack.size(); ++i) {
        const EffectInstance& instance = stack[i];
        const EffectDescriptor* d = descriptor(instance.effect);
        const QString section = effectKey(instance.id);
        const EffectKind kind = video ? EffectKind::Video : EffectKind::Audio;
        // 0.6: a title's style is its Text section (Premiere's Text in Effect Controls), with the title's text and font.
        const bool titleStyle = instance.effect == edit::kTitleStyleEffect;
        Row& h = header(section, QStringLiteral("effect"),
                        titleStyle ? tx("Text (%1)").arg(QString::fromStdString(clip.name()))
                        : d        ? tx(d->displayName.data())
                                   : QString::fromStdString(instance.effect),
                        titleStyle ? QStringLiteral("type") : d ? categoryIcon(d->category, d->kind) : QStringLiteral("sparkles"));
        h.effectEnabled = instance.enabled;
        h.removable = !titleStyle; // the title's look: without it the text would draw in the default style
        h.movable = true;
        h.resettable = d != nullptr;
        h.stackIndex = static_cast<int>(i);
        h.stackKind = video ? QStringLiteral("video") : QStringLiteral("audio");
        h.known = isEvaluable(instance, kind);
        h.description = d ? fromView(d->description) : tx("This effect is not available in this version; it is kept as it is.");
        if (!d || m_collapsed.contains(section))
            continue;
        if (titleStyle) {
            addTitleRows(rows, clip, section);
            continue;
        }
        const bool enabled = instance.enabled;
        for (const EffectParamInfo& p : d->params) {
            addParamRow(rows, clip, section + QLatin1Char(':') + fromView(p.key), section, QStringLiteral("effect"),
                        edit::ParamRef::ofEffect(instance.id, std::string(p.key)), tx(p.displayName.data()), QString(),
                        uiRange(p), p.defaultValue, p.animatable, enabled,
                        std::string(d->displayName) + ": " + std::string(p.displayName));
            if (p.isChoice())
                makeChoice(rows.back(), p);
        }
        if (instance.effect == "wabcut.video.simpleText") {
            // 0.2: Simple Text's text is effect data (not keyframable), edited in a text row.
            Row t;
            t.key = section + QStringLiteral(":data:text");
            t.kind = QStringLiteral("text");
            t.section = section;
            t.sectionKind = QStringLiteral("effect");
            t.title = tx("Text");
            const EffectData* data = instance.findData("text");
            t.text = data && data->text() ? QString::fromStdString(*data->text()) : QStringLiteral("Simple Text");
            t.clip = clip.id;
            t.ref.key = "text";
            t.undoLabel = "Simple Text: Text";
            rows.push_back(std::move(t));
        }
    }
}

void EffectControlsModel::addTransitionRows(std::vector<Row>& rows, const Track& track, const Transition& transition) {
    const EffectDescriptor* d = descriptor(transition.effect);
    Row h;
    h.key = QStringLiteral("h:transition");
    h.kind = QStringLiteral("header");
    h.section = QStringLiteral("transition");
    h.sectionKind = QStringLiteral("transition");
    h.title = d ? tx(d->displayName.data()) : QString::fromStdString(transition.effect);
    h.icon = d ? categoryIcon(d->category, d->kind) : QStringLiteral("blend");
    h.removable = true;
    h.resettable = d && !d->params.empty();
    h.description = d ? fromView(d->description) : QString();
    rows.push_back(h);

    Row duration;
    duration.key = QStringLiteral("t:duration");
    duration.kind = QStringLiteral("timecode");
    duration.section = h.section;
    duration.sectionKind = h.sectionKind;
    duration.title = tx("Duration");
    duration.icon = QStringLiteral("gauge");
    duration.text = m_controller->formatTimecode(transition.duration());
    rows.push_back(duration);

    const auto option = [](const QString& value, const QString& label) {
        return QVariantMap{{QStringLiteral("value"), value}, {QStringLiteral("label"), label}};
    };
    if (transition.outgoing && transition.incoming) {
        Row alignment;
        alignment.key = QStringLiteral("t:alignment");
        alignment.kind = QStringLiteral("choice");
        alignment.section = h.section;
        alignment.sectionKind = h.sectionKind;
        alignment.title = tx("Alignment");
        alignment.icon = QStringLiteral("fold-horizontal");
        const Ticks frame = m_controller->frameTicks();
        const Ticks diff = transition.before - transition.after;
        alignment.text = transition.before == Ticks{} ? QStringLiteral("startAtCut")
                       : transition.after == Ticks{} ? QStringLiteral("endAtCut")
                       : (diff <= frame && -diff <= frame) ? QStringLiteral("center")
                                                           : QStringLiteral("custom");
        alignment.options = {option(QStringLiteral("center"), tx("Center")),
                             option(QStringLiteral("startAtCut"), tx("Start at Cut")),
                             option(QStringLiteral("endAtCut"), tx("End at Cut"))};
        if (alignment.text == QLatin1String("custom"))
            alignment.options.append(option(QStringLiteral("custom"), tx("Custom")));
        rows.push_back(alignment);
    }

    Row effect;
    effect.key = QStringLiteral("t:effect");
    effect.kind = QStringLiteral("choice");
    effect.section = h.section;
    effect.sectionKind = h.sectionKind;
    effect.title = tx("Transition");
    effect.icon = QStringLiteral("blend");
    effect.text = QString::fromStdString(transition.effect);
    const EffectKind kind = track.kind == TrackKind::Video ? EffectKind::VideoTransition : EffectKind::AudioTransition;
    for (const EffectDescriptor& t : EffectRegistry::builtin().all()) {
        if (t.kind == kind)
            effect.options.append(option(fromView(t.id), tx(t.displayName.data())));
    }
    rows.push_back(effect);

    if (!d)
        return;
    for (const EffectParamInfo& p : d->params) {
        const UiRange range = uiRange(p);
        Row r;
        r.key = QStringLiteral("t:") + fromView(p.key);
        r.kind = range.toggle ? QStringLiteral("toggle") : QStringLiteral("param");
        r.section = h.section;
        r.sectionKind = h.sectionKind;
        r.title = tx(p.displayName.data());
        r.unit = unitText(range);
        r.range = range;
        r.from = range.from;
        r.to = range.to;
        r.step = range.step;
        r.decimals = range.decimals;
        r.origin = range.bipolar ? 0.0 : r.from;
        double model = p.defaultValue;
        for (const EffectParam& param : transition.params) {
            if (param.key == p.key)
                model = param.value.constant();
        }
        r.value = range.toUi(model, r.frame);
        r.defaultValue = range.toUi(p.defaultValue, r.frame);
        r.resettable = true;
        r.ref.key = std::string(p.key);
        r.undoLabel = std::string(d->displayName) + ": " + std::string(p.displayName);
        if (p.isChoice())
            makeChoice(r, p);
        rows.push_back(std::move(r));
    }
}

std::vector<EffectControlsModel::Row> EffectControlsModel::buildRows() {
    std::vector<Row> rows;
    QString mode, title, subtitle, trackName;
    int colorIndex = 0;
    int multiCount = 0;
    TimeRange lane;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (seq) {
        if (const TransitionId tid = m_controller->selectedTransitionId()) {
            if (const auto loc = m_controller->findTransition(tid)) {
                const Transition& t = *loc->transition;
                mode = QStringLiteral("transition");
                const EffectDescriptor* d = descriptor(t.effect);
                title = d ? tx(d->displayName.data()) : QString::fromStdString(t.effect);
                const ClipId any = t.outgoing ? t.outgoing : t.incoming;
                if (const auto clip = seq->findClip(any)) {
                    const Ticks cut = t.outgoing ? clip->clip().end() : clip->clip().start();
                    subtitle = m_controller->formatTimecode(cut - t.before) + QStringLiteral(" → ")
                             + m_controller->formatTimecode(cut + t.after);
                    colorIndex = clip->clip().colorIndex;
                }
                if (const auto pos = seq->trackPosition(loc->track->id))
                    trackName = (pos->kind == TrackKind::Video ? QStringLiteral("V") : QStringLiteral("A"))
                              + QString::number(pos->index + 1);
                addTransitionRows(rows, *loc->track, t);
            }
        } else if (m_controller->primaryClip() && multiUnits(*seq, m_controller->selection()).size() > 1) {
            // 0.6: several clips (each with its linked partners): their Transform, Opacity and Audio rows together.
            m_multiUnits = multiUnits(*seq, m_controller->selection());
            multiCount = static_cast<int>(m_multiUnits.size());
            mode = QStringLiteral("multi");
            title = tx("%1 clips").arg(multiCount);
            int videoCount = 0, audioCount = 0;
            for (const std::vector<ClipId>& unit : m_multiUnits) {
                for (const ClipId id : unit) {
                    if (const auto loc = seq->findClip(id))
                        ++(loc->clip().stream.kind == StreamKind::Video ? videoCount : audioCount);
                }
            }
            subtitle = tx("%1 video · %2 audio").arg(videoCount).arg(audioCount);
            addMultiRows(rows);
        } else if (const ClipId primary = m_controller->primaryClip()) {
            const auto video = m_controller->propertyTarget(StreamKind::Video);
            const auto audio = m_controller->propertyTarget(StreamKind::Audio);
            const auto laneClip = seq->findClip(video ? *video : audio ? *audio : primary);
            const auto primaryLoc = seq->findClip(primary);
            if (laneClip && primaryLoc) {
                mode = QStringLiteral("clip");
                const Clip& p = primaryLoc->clip();
                title = QString::fromStdString(p.name());
                // 0.4: "start → end · duration" (the timeline's clip tooltip shows the same facts).
                subtitle = m_controller->formatTimecode(p.start()) + QStringLiteral(" → ") + m_controller->formatTimecode(p.end())
                         + QStringLiteral(" · ") + m_controller->formatTimecode(p.end() - p.start());
                colorIndex = p.colorIndex;
                if (const auto pos = seq->trackPosition(primaryLoc->track->id))
                    trackName = (pos->kind == TrackKind::Video ? QStringLiteral("V") : QStringLiteral("A"))
                              + QString::number(pos->index + 1);
                lane = laneClip->clip().timelineRange;
                m_lane = lane;
                // 0.5.1: another clip in the lane shows whole; a shortened one keeps its view inside the zoom limits.
                const ClipId laneId = video ? *video : audio ? *audio : primary;
                if (laneId != m_viewClip) {
                    m_viewClip = laneId;
                    if (m_viewStart != 0.0 || m_viewEnd != 1.0) {
                        m_viewStart = 0.0;
                        m_viewEnd = 1.0;
                        m_viewChanged = true;
                    }
                } else if (m_viewEnd - m_viewStart < minLaneSpan()) {
                    const double span = minLaneSpan();
                    m_viewStart = span >= 1.0 ? 0.0 : std::clamp(m_viewStart, 0.0, 1.0 - span);
                    m_viewEnd = span >= 1.0 ? 1.0 : m_viewStart + span;
                    m_viewChanged = true;
                }
                if (video)
                    addClipSections(rows, seq->findClip(*video)->clip(), true);
                if (audio)
                    addClipSections(rows, seq->findClip(*audio)->clip(), false);
            }
        }
    }
    m_lane = lane;
    if (multiCount == 0)
        m_multiUnits.clear();
    if (mode != m_mode || title != m_title || subtitle != m_subtitle || trackName != m_trackName
        || colorIndex != m_colorIndex || multiCount != m_multiCount) {
        m_multiCount = multiCount;
        m_mode = mode;
        m_title = title;
        m_subtitle = subtitle;
        m_trackName = trackName;
        m_colorIndex = colorIndex;
        emit contextChanged();
    }
    QString laneStart, laneEnd;
    double playhead = -1;
    if (lane.duration > Ticks{}) {
        const auto at = [&](double x) {
            return lane.start + Ticks{static_cast<std::int64_t>(std::llround(x * static_cast<double>(lane.duration.value)))};
        };
        laneStart = m_controller->formatTimecode(zoomed() ? at(m_viewStart) : lane.start);
        laneEnd = m_controller->formatTimecode(zoomed() ? at(m_viewEnd) : lane.end());
        const Ticks p = m_controller->playhead();
        if (p >= lane.start && p <= lane.end())
            playhead = static_cast<double>((p - lane.start).value) / static_cast<double>(lane.duration.value);
    }
    if (laneStart != m_laneStartText || laneEnd != m_laneEndText || playhead != m_playheadPosition || m_viewChanged) {
        m_laneStartText = laneStart;
        m_laneEndText = laneEnd;
        m_playheadPosition = playhead;
        m_viewChanged = false;
        emit laneChanged();
    }
    return rows;
}

void EffectControlsModel::applyRows(std::vector<Row> next) {
    const int before = rowCount();
    // 1. Rows that are gone.
    std::set<QString> keep;
    for (const Row& r : next)
        keep.insert(r.key);
    for (int i = rowCount() - 1; i >= 0; --i) {
        if (!keep.contains(m_rows[static_cast<std::size_t>(i)].key)) {
            beginRemoveRows({}, i, i);
            m_rows.erase(m_rows.begin() + i);
            endRemoveRows();
        }
    }
    // 2. In order: keep, move or insert each row, then update its data.
    for (std::size_t i = 0; i < next.size(); ++i) {
        const int at = static_cast<int>(i);
        if (i < m_rows.size() && m_rows[i].key == next[i].key) {
            // in place
        } else {
            int from = -1;
            for (std::size_t j = i + 1; j < m_rows.size(); ++j) {
                if (m_rows[j].key == next[i].key) {
                    from = static_cast<int>(j);
                    break;
                }
            }
            if (from >= 0) {
                beginMoveRows({}, from, from, {}, at);
                Row moved = std::move(m_rows[static_cast<std::size_t>(from)]);
                m_rows.erase(m_rows.begin() + from);
                m_rows.insert(m_rows.begin() + at, std::move(moved));
                endMoveRows();
            } else {
                beginInsertRows({}, at, at);
                m_rows.insert(m_rows.begin() + at, next[i]);
                endInsertRows();
                continue;
            }
        }
        const bool same = m_rows[i].sameData(next[i]);
        m_rows[i] = std::move(next[i]);
        if (!same)
            emit dataChanged(index(at), index(at));
    }
    if (rowCount() != before)
        emit countChanged();
}

void EffectControlsModel::onRevision() {
    if (!m_controller)
        return;
    const TimelineChange& change = m_controller->lastChange();
    const SequenceChange* s = change.sequenceChange();
    bool relevant = change.structural || !change.changes || (s && s->settingsChanged) || m_mode == QLatin1String("multi");
    if (!relevant && m_controller->selectedTransitionId()) {
        // A transition's rows read its track and the clips around it: any change to that track.
        const auto loc = m_controller->findTransition(m_controller->selectedTransitionId());
        relevant = !loc || (s && std::ranges::any_of(s->tracks, [&](const TrackChange& t) { return t.track == loc->track->id; }));
    } else if (!relevant && m_controller->primaryClip()) {
        std::vector<ClipId> clips{m_controller->primaryClip()};
        for (const StreamKind kind : {StreamKind::Video, StreamKind::Audio}) {
            if (const auto target = m_controller->propertyTarget(kind))
                clips.push_back(*target);
        }
        std::vector<MediaId> media;
        std::vector<TrackId> tracks;
        if (const SequencePtr seq = m_controller->currentSequence()) {
            for (ClipId id : clips) {
                if (const auto loc = seq->findClip(id)) {
                    media.push_back(loc->clip().media);
                    tracks.push_back(loc->track->id);
                }
            }
        }
        relevant = change.touchesAny(clips, media)
                || (s && std::ranges::any_of(s->tracks, [&](const TrackChange& t) {
                       return t.headerChanged && std::ranges::find(tracks, t.track) != tracks.end();
                   }));
    } else if (!relevant) {
        relevant = !m_rows.empty(); // nothing to show: an edit cannot add rows; clear any that are left
    }
    if (relevant)
        refresh();
}

void EffectControlsModel::refresh() {
    if (m_refreshing || !m_controller)
        return;
    m_refreshing = true;
    m_shownKeys.clear();
    std::vector<Row> rows = buildRows();
    // Selected keys that are no longer shown (deleted, another clip, collapsed) are dropped.
    std::set<KeyRef> kept;
    for (const KeyRef& s : m_selectedKeys) {
        if (m_shownKeys.contains(s))
            kept.insert(s);
    }
    const bool selectionChanged = kept.size() != m_selectedKeys.size();
    if (selectionChanged)
        m_selectedKeys = std::move(kept);
    // 0.4: selected sections that are no longer shown (removed, another primary clip, a transition) are dropped.
    QStringList sections;
    if (m_mode == QLatin1String("clip") && m_controller->primaryClip() == m_sectionsOf) {
        for (const Row& r : rows) {
            if (r.kind == QLatin1String("header") && r.sectionKind != QLatin1String("transition")
                && m_selectedSections.contains(r.section))
                sections << r.section;
        }
    }
    const bool sectionsChanged = sections != m_selectedSections;
    if (sectionsChanged) {
        m_selectedSections = sections;
        if (sections.isEmpty())
            m_sectionAnchor.clear();
        for (Row& r : rows) {
            if (r.kind == QLatin1String("header"))
                r.sectionSelected = sections.contains(r.section);
        }
    }
    applyRows(std::move(rows));
    m_refreshing = false;
    if (selectionChanged)
        emit keySelectionChanged();
    if (sectionsChanged)
        emit sectionSelectionChanged();
}

void EffectControlsModel::setNotice(const QString& notice) {
    if (notice == m_notice)
        return;
    m_notice = notice;
    emit noticeChanged();
}

// ---- Parameter rows ---------------------------------------------------------------------------------

void EffectControlsModel::setValue(const QString& rowKey, double value) {
    const Row* r = findRow(rowKey);
    if (!r || !m_controller || !std::isfinite(value) || !isValueRow(*r))
        return;
    const double model = r->range.toModel(std::clamp(value, std::min(r->from, r->to), std::max(r->from, r->to)), r->frame);
    if (r->sectionKind == QLatin1String("transition")) {
        const TransitionId id = m_controller->selectedTransitionId();
        const auto loc = m_controller->findTransition(id);
        if (!loc)
            return;
        std::vector<EffectParam> params = loc->transition->params;
        const auto it = std::find_if(params.begin(), params.end(), [&](const EffectParam& p) { return p.key == r->ref.key; });
        if (it != params.end())
            it->value = ParamValue(model);
        else
            params.push_back({r->ref.key, ParamValue(model)});
        std::sort(params.begin(), params.end(), [](const EffectParam& a, const EffectParam& b) { return a.key < b.key; });
        edit::TransitionChange change;
        change.params = std::move(params);
        const SequenceId seq = m_controller->currentSequenceId();
        const std::string mergeKey = "transition-param:" + rowKey.toStdString();
        m_controller->applyEditFn(
            r->undoLabel.c_str(),
            [&](const ProjectState& state) { return edit::setTransition(state, seq, id, change); }, mergeKey);
        return;
    }
    if (r->sectionKind == QLatin1String("multi")) {
        setMultiValue(*r, std::clamp(value, std::min(r->from, r->to), std::max(r->from, r->to))); // 0.6
        return;
    }
    if (!r->clip)
        return;
    const std::string mergeKey = "fx:" + std::to_string(r->clip.value) + ":" + rowKey.toStdString();
    if (r->ref.layer != 0) { // 0.6: a graphic layer's number
        setLayerParamAtPlayhead(*r, model, mergeKey);
        return;
    }
    if (!r->textProp.isEmpty()) { // 0.6: a text property that is not keyframable
        setTextProperty(*r, model, mergeKey);
        return;
    }
    const std::string label = r->undoLabel;
    m_controller->setParameterAtPlayhead(r->clip, r->ref, model, label.c_str(), mergeKey);
}

void EffectControlsModel::endEdit() {
    if (m_controller)
        m_controller->endPropertyGesture();
}

void EffectControlsModel::setEffectText(const QString& rowKey, const QString& text) {
    const Row* r = findRow(rowKey);
    if (!r || !m_controller || !r->clip || text == r->text)
        return;
    if (r->kind == QLatin1String("textarea") || r->kind == QLatin1String("font")) {
        // 0.6: Source Text and Font of a text layer (at the playhead: Source Text keys) or of a title.
        const QByteArray utf8 = text.toUtf8();
        if (static_cast<std::size_t>(utf8.size()) > kMaxGraphicTextBytes) {
            setNotice(tx("The text is longer than %1 bytes.").arg(static_cast<qulonglong>(kMaxGraphicTextBytes)));
            return;
        }
        const std::string value = utf8.toStdString();
        const bool font = r->kind == QLatin1String("font");
        if (font && text.trimmed().isEmpty())
            return;
        if (r->layer == 0) {
            const Clip* clip = clipOf(*r);
            if (!clip)
                return;
            const MediaId media = clip->media;
            setNotice({});
            const bool ok = font ? m_controller->applyEditFn(LABEL("Title Font"), [&](const ProjectState& state) {
                return edit::setTitleFont(state, media, value, std::nullopt);
            })
                                 : m_controller->applyEditFn(LABEL("Title Text"), [&](const ProjectState& state) {
                                       return edit::setTitleText(state, media, value);
                                   });
            if (!ok)
                setNotice(m_controller->lastError());
            return;
        }
        const QStringList styles = font ? QFontDatabase::styles(text) : QStringList();
        editTextLayer(*r, font ? LABEL("Font") : LABEL("Source Text"), [&](GraphicLayer& l, Ticks t) {
            return edit::editSourceRunsAt(l.text, t, [&](std::vector<TextRun>& runs) {
                if (font) {
                    bool changed = false;
                    for (TextRun& run : runs) {
                        const TextStyle before = run.style;
                        run.style.fontFamily = value;
                        run.style.brandFont.clear();
                        if (!run.style.fontStyle.empty() && !styles.contains(QString::fromStdString(run.style.fontStyle)))
                            run.style.fontStyle.clear();
                        changed = changed || !(before == run.style);
                    }
                    return changed;
                }
                std::string joined;
                for (const TextRun& run : runs)
                    joined += run.text;
                if (joined == value)
                    return false;
                // The new text in the first run's style (Effect Controls edits the layer's text as a whole).
                const TextStyle style = runs.empty() ? TextStyle{} : runs.front().style;
                runs = {TextRun{value, style}};
                return true;
            });
        });
        return;
    }
    if (r->kind != QLatin1String("text"))
        return;
    const auto effect = parseEffectKey(r->section);
    if (!effect)
        return;
    const QByteArray utf8 = text.toUtf8();
    if (static_cast<std::size_t>(utf8.size()) > kMaxEffectText) {
        setNotice(tx("The text is longer than %1 bytes.").arg(static_cast<qulonglong>(kMaxEffectText)));
        return;
    }
    const SequenceId seq = m_controller->currentSequenceId();
    const ClipId clip = r->clip;
    edit::EffectChange change;
    change.data = {EffectData{r->ref.key, utf8.toStdString()}};
    m_controller->applyEditFn(LABEL("Simple Text: Text"), [&](const ProjectState& state) {
        return edit::setEffect(state, seq, clip, *effect, change);
    });
}

void EffectControlsModel::resetValue(const QString& rowKey) {
    const Row* r = findRow(rowKey);
    if (!r)
        return;
    const double value = r->defaultValue;
    setValue(rowKey, value);
    endEdit();
}

void EffectControlsModel::setAnimated(const QString& rowKey, bool animated) {
    const Row* r = findRow(rowKey);
    const Clip* clip = r ? clipOf(*r) : nullptr;
    if (!clip || !r->animatable || r->animated == animated)
        return;
    const SequenceId seq = m_controller->currentSequenceId();
    const ClipId id = clip->id;
    const edit::ParamRef ref = r->ref;
    const Ticks at = rowCurveTimeAtPlayhead(*r, *clip);
    m_controller->applyEditFn(LABEL("Toggle Animation"), [&](const ProjectState& state) {
        return edit::setAnimated(state, seq, id, ref, animated, at);
    });
}

void EffectControlsModel::toggleKeyAtPlayhead(const QString& rowKey) {
    const Row* r = findRow(rowKey);
    const Clip* clip = r ? clipOf(*r) : nullptr;
    if (!clip || !r->animatable)
        return;
    const SequenceId seq = m_controller->currentSequenceId();
    const ClipId id = clip->id;
    const edit::ParamRef ref = r->ref;
    const Ticks at = rowCurveTimeAtPlayhead(*r, *clip);
    const ParamValue value = paramValueOf(m_controller, *clip, ref);
    const Curve* curve = value.curve();
    if (!curve) {
        m_controller->applyEditFn(LABEL("Add Keyframe"), [&](const ProjectState& state) {
            return edit::setAnimated(state, seq, id, ref, true, at);
        });
        return;
    }
    if (const auto index = curve->keyAt(at)) {
        const std::size_t i = *index;
        m_controller->applyEditFn(LABEL("Remove Keyframe"), [&](const ProjectState& state) {
            return edit::removeKeyframe(state, seq, id, ref, i);
        });
        return;
    }
    Keyframe key{at, value.valueAt(at)};
    if (const std::size_t i = curve->insertionIndex(at); i > 0)
        key.interpolation = curve->keys[i - 1].interpolation;
    m_controller->applyEditFn(LABEL("Add Keyframe"), [&](const ProjectState& state) {
        return edit::addKeyframe(state, seq, id, ref, key);
    });
}

void EffectControlsModel::goToKey(const QString& rowKey, int direction) {
    const Row* r = findRow(rowKey);
    const Clip* clip = r ? clipOf(*r) : nullptr;
    if (!clip)
        return;
    const ParamValue value = paramValueOf(m_controller, *clip, r->ref);
    const Curve* curve = value.curve();
    if (!curve)
        return;
    const Ticks playhead = m_controller->playhead();
    std::optional<Ticks> best;
    for (const auto& [i, t] : visibleKeys(*r, *curve, *clip)) {
        if (direction < 0 && t < playhead && (!best || t > *best))
            best = t;
        if (direction > 0 && t > playhead && (!best || t < *best))
            best = t;
    }
    if (best)
        m_controller->setPlayhead(*best);
}

// ---- Sections ---------------------------------------------------------------------------------------

void EffectControlsModel::setCollapsed(const QString& section, bool collapsed) {
    if (collapsed == m_collapsed.contains(section))
        return;
    if (collapsed)
        m_collapsed.insert(section);
    else
        m_collapsed.erase(section);
    refresh();
}

void EffectControlsModel::setEffectEnabled(const QString& section, bool enabled) {
    const Row* h = findRow(QStringLiteral("h:") + section);
    const auto effect = parseEffectKey(section);
    if (!h || !effect || !h->clip || h->effectEnabled == enabled)
        return;
    const SequenceId seq = m_controller->currentSequenceId();
    const ClipId clip = h->clip;
    edit::EffectChange change;
    change.enabled = enabled;
    m_controller->applyEditFn(enabled ? LABEL("Enable Effect") : LABEL("Disable Effect"), [&](const ProjectState& state) {
        return edit::setEffect(state, seq, clip, *effect, change);
    });
}

void EffectControlsModel::removeSection(const QString& section) {
    if (!m_controller)
        return;
    if (section == QLatin1String("transition")) {
        m_controller->deleteSelectedTransition();
        return;
    }
    const Row* h = findRow(QStringLiteral("h:") + section);
    const auto effect = parseEffectKey(section);
    if (!h || !effect || !h->clip)
        return;
    const SequenceId seq = m_controller->currentSequenceId();
    const ClipId clip = h->clip;
    m_controller->applyEditFn(LABEL("Remove Effect"), [&](const ProjectState& state) {
        return edit::removeEffect(state, seq, clip, *effect);
    });
}

namespace {

/// The intrinsic parameters a fixed section shows (and Reset / Remove reset): Transform, Opacity or Audio.
std::optional<PropertyRole> fixedSectionRole(const QString& section) {
    if (section == QLatin1String("motion"))
        return PropertyRole::Motion;
    if (section == QLatin1String("opacity"))
        return PropertyRole::Opacity;
    if (section == QLatin1String("audio"))
        return PropertyRole::Audio;
    return std::nullopt;
}

using EditStep = std::function<edit::EditOutcome(const ProjectState&)>;

/// Steps that set every intrinsic parameter of a fixed section to its default constant (keys deleted). From the
/// registry, not the rows, so a collapsed section (which shows no rows) resets too.
void addFixedResetSteps(std::vector<EditStep>& steps, SequenceId seq, ClipId clip, PropertyRole role) {
    for (const UiParameter& p : uiParameters()) {
        const ParameterInfo& info = parameterInfo(p.id);
        if (info.role != role)
            continue;
        const edit::ParamRef ref = edit::ParamRef::of(p.id);
        const double value = info.defaultValue;
        steps.push_back([=](const ProjectState& state) { return edit::setParameter(state, seq, clip, ref, ParamValue(value)); });
    }
}

} // namespace

void EffectControlsModel::resetSection(const QString& section) {
    if (!m_controller)
        return;
    const SequenceId seq = m_controller->currentSequenceId();
    if (section == QLatin1String("transition")) {
        const TransitionId id = m_controller->selectedTransitionId();
        edit::TransitionChange change;
        change.params = std::vector<EffectParam>{};
        m_controller->applyEditFn(LABEL("Reset Transition"), [&](const ProjectState& state) {
            return edit::setTransition(state, seq, id, change);
        });
        return;
    }
    // Every parameter of the section back to its default constant (keys deleted), as one step. From the registry and
    // the effect's descriptor (fix round 1: not the rows, so the header menu's Reset works on a collapsed section).
    const Row* h = findRow(QStringLiteral("h:") + section);
    const SequencePtr sequence = m_controller->currentSequence();
    if (!h || !h->clip || !sequence)
        return;
    const ClipId clip = h->clip;
    std::vector<EditStep> steps;
    if (const auto role = fixedSectionRole(section)) {
        addFixedResetSteps(steps, seq, clip, *role);
    } else if (const auto effect = parseEffectKey(section)) {
        const auto loc = sequence->findClip(clip);
        if (!loc)
            return;
        // 0.6: a master clip effect too (the clip's own stack, then its media's master clip effects).
        const ProjectSnapshot snapshot = m_controller->snapshot();
        EffectStack stack = loc->clip().effects();
        if (snapshot) {
            const EffectStack master = masterEffectsOf(*snapshot, loc->clip().media);
            stack.insert(stack.end(), master.begin(), master.end());
        }
        const auto it = std::find_if(stack.begin(), stack.end(), [&](const EffectInstance& e) { return e.id == *effect; });
        const EffectDescriptor* d = it != stack.end() ? descriptor(it->effect) : nullptr;
        if (!d)
            return;
        const EffectInstanceId id = *effect;
        for (const EffectParamInfo& p : d->params) {
            const edit::ParamRef ref = edit::ParamRef::ofEffect(id, std::string(p.key));
            const double value = p.defaultValue;
            steps.push_back([=](const ProjectState& state) { return edit::setParameter(state, seq, clip, ref, ParamValue(value)); });
        }
    }
    if (steps.empty())
        return;
    m_controller->applyEditFn(LABEL("Reset Effect"), [&](const ProjectState& state) { return chain(state, steps); });
}

void EffectControlsModel::moveEffect(const QString& section, int stackIndex) {
    const Row* h = findRow(QStringLiteral("h:") + section);
    const auto effect = parseEffectKey(section);
    if (!h || !effect || !h->clip || stackIndex < 0 || stackIndex == h->stackIndex)
        return;
    const SequenceId seq = m_controller->currentSequenceId();
    const ClipId clip = h->clip;
    const auto index = static_cast<std::size_t>(stackIndex);
    m_controller->applyEditFn(LABEL("Move Effect"), [&](const ProjectState& state) {
        return edit::moveEffect(state, seq, clip, *effect, index);
    });
}

// ---- Section selection and the effects clipboard (0.4) -----------------------------------------------

QStringList EffectControlsModel::selectableSections() const {
    QStringList out;
    if (m_mode != QLatin1String("clip"))
        return out;
    for (const Row& r : m_rows) {
        if (r.kind == QLatin1String("header") && r.sectionKind != QLatin1String("transition")
            && r.sectionKind != QLatin1String("text") && r.clip.isValid())
            out << r.section;
    }
    return out;
}

void EffectControlsModel::setSectionSelection(QStringList sections, const QString& anchor) {
    // Panel order, each once, only sections the panel shows.
    const QStringList shown = selectableSections();
    QStringList ordered;
    for (const QString& s : shown) {
        if (sections.contains(s))
            ordered << s;
    }
    m_sectionAnchor = ordered.isEmpty() ? QString() : anchor;
    if (ordered == m_selectedSections)
        return;
    m_selectedSections = ordered;
    m_sectionsOf = ordered.isEmpty() || !m_controller ? ClipId{} : m_controller->primaryClip();
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        Row& r = m_rows[i];
        if (r.kind != QLatin1String("header"))
            continue;
        const bool selected = ordered.contains(r.section);
        if (r.sectionSelected != selected) {
            r.sectionSelected = selected;
            const QModelIndex at = index(static_cast<int>(i));
            emit dataChanged(at, at, {SectionSelectedRole});
        }
    }
    if (!ordered.isEmpty() && !m_selectedKeys.empty()) {
        // Selecting a section clears the selected keyframes (and the other way round: pressKey).
        m_selectedKeys.clear();
        refresh();
        emit keySelectionChanged();
    }
    emit sectionSelectionChanged();
}

void EffectControlsModel::selectSection(const QString& section, int modifiers) {
    const QStringList shown = selectableSections();
    if (!shown.contains(section))
        return;
    const bool toggle = mods(modifiers).testAnyFlags(Qt::ControlModifier | Qt::MetaModifier);
    const bool range = mods(modifiers).testFlag(Qt::ShiftModifier);
    if (range && !m_sectionAnchor.isEmpty() && shown.contains(m_sectionAnchor)) {
        // Shift: from the anchor to this header in panel order (Ctrl+Shift adds the range to the selection).
        const qsizetype a = shown.indexOf(m_sectionAnchor);
        const qsizetype b = shown.indexOf(section);
        QStringList next = toggle ? m_selectedSections : QStringList();
        for (qsizetype i = std::min(a, b); i <= std::max(a, b); ++i) {
            if (!next.contains(shown[i]))
                next << shown[i];
        }
        setSectionSelection(next, m_sectionAnchor);
        return;
    }
    if (toggle) {
        QStringList next = m_selectedSections;
        if (!next.removeOne(section))
            next << section;
        setSectionSelection(next, section);
        return;
    }
    setSectionSelection({section}, section);
}

void EffectControlsModel::selectAllSections() {
    const QStringList shown = selectableSections();
    setSectionSelection(shown, shown.isEmpty() ? QString() : shown.front());
}

void EffectControlsModel::clearSectionSelection() {
    setSectionSelection({}, {});
}

std::vector<std::function<edit::EditOutcome(const ProjectState&)>>
EffectControlsModel::removeSteps(const QStringList& sections, int* removed, int* reset) const {
    std::vector<std::function<edit::EditOutcome(const ProjectState&)>> steps;
    int removedCount = 0, resetCount = 0;
    if (m_controller) {
        const SequenceId seq = m_controller->currentSequenceId();
        for (const QString& section : sections) {
            const Row* h = findRow(QStringLiteral("h:") + section);
            if (!h || !h->clip || h->sectionKind == QLatin1String("transition"))
                continue;
            const ClipId clip = h->clip;
            if (const auto role = fixedSectionRole(section)) {
                // Reset from the registry (not the rows), so a collapsed section resets too; keys are deleted.
                addFixedResetSteps(steps, seq, clip, *role);
                ++resetCount;
            } else if (const auto effect = parseEffectKey(section)) {
                const EffectInstanceId id = *effect;
                steps.push_back([=](const ProjectState& state) { return edit::removeEffect(state, seq, clip, id); });
                ++removedCount;
            }
        }
    }
    if (removed)
        *removed = removedCount;
    if (reset)
        *reset = resetCount;
    return steps;
}

int EffectControlsModel::removeSelectedSections() {
    if (!m_controller || m_selectedSections.isEmpty())
        return 0;
    int removed = 0, reset = 0;
    const auto steps = removeSteps(m_selectedSections, &removed, &reset);
    if (steps.empty())
        return 0;
    // Premiere: "Remove Effect" for one, "Remove Effects" for several; a fixed section alone is reset.
    const char* label = removed == 0 ? LABEL("Reset Effect")
                      : removed + reset == 1 ? LABEL("Remove Effect")
                                             : LABEL("Remove Effects");
    setNotice({});
    if (!m_controller->applyEditFn(label, [&](const ProjectState& state) { return chain(state, steps); })) {
        setNotice(m_controller->lastError());
        return 0;
    }
    clearSectionSelection();
    return removed + reset;
}

std::shared_ptr<const EffectControlsModel::EffectsClipboard> EffectControlsModel::snapshotSections() const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq || m_selectedSections.isEmpty())
        return nullptr;
    auto board = std::make_shared<EffectsClipboard>();
    for (const QString& section : m_selectedSections) {
        const Row* h = findRow(QStringLiteral("h:") + section);
        const auto loc = h && h->clip ? seq->findClip(h->clip) : std::nullopt;
        if (!loc)
            continue;
        const Clip& clip = loc->clip();
        const bool video = clip.stream.kind == StreamKind::Video;
        std::optional<Clip>& snapshot = video ? board->video : board->audio;
        AttributeSet& attributes = video ? board->videoAttributes : board->audioAttributes;
        // 0.6: the clip as it plays, its master clip effects first, so master sections copy too.
        const ProjectSnapshot state = m_controller->snapshot();
        const std::size_t masterCount =
            state && playsMasterEffects(*state, clip)
                ? masterEffectsOf(*state, clip.media, video ? EffectKind::Video : EffectKind::Audio).size()
                : 0;
        if (!snapshot) {
            snapshot = clip;
            if (masterCount > 0)
                snapshot->setEffects(playedEffects(*state, clip, video ? EffectKind::Video : EffectKind::Audio));
        }
        if (section == QLatin1String("motion")) {
            attributes.motion = true;
        } else if (section == QLatin1String("opacity")) {
            attributes.opacity = true;
        } else if (section == QLatin1String("audio")) {
            attributes.volume = true;
            attributes.panner = true;
        } else if (h->stackIndex >= 0) {
            const bool masterSection = h->stackKind == QLatin1String("master");
            attributes.effects.push_back(static_cast<std::size_t>(h->stackIndex) + (masterSection ? 0 : masterCount));
        } else {
            continue;
        }
        ++board->count;
    }
    if (board->count == 0)
        return nullptr;
    return board;
}

void EffectControlsModel::setEffectsPending(bool pending) {
    const bool before = effectsPastePending();
    m_effectsPending = pending && m_effects != nullptr;
    if (effectsPastePending() != before)
        emit effectsClipboardChanged();
}

int EffectControlsModel::copySelectedSections(bool cut) {
    if (!m_controller || m_selectedSections.isEmpty())
        return 0;
    auto board = snapshotSections();
    if (!board)
        return 0;
    const int count = board->count;
    if (cut) {
        int removed = 0, reset = 0;
        const auto steps = removeSteps(m_selectedSections, &removed, &reset);
        if (!steps.empty()
            && !m_controller->applyEditFn(LABEL("Cut Effects"), [&](const ProjectState& state) { return chain(state, steps); })) {
            setNotice(m_controller->lastError());
            return 0;
        }
    }
    m_effects = std::move(board);
    m_effectsPending = true;
    emit effectsClipboardChanged(); // a new copy (even when one was already pending)
    setKeysPending(false);          // 0.6: the newest copy wins
    if (cut)
        clearSectionSelection();
    return count;
}

QString EffectControlsModel::pasteEffects() {
    if (!m_controller)
        return tx("No project");
    const auto notify = [&](const QString& text) {
        emit m_controller->clipboardNotice(text);
        return text;
    };
    if (!m_effects)
        return notify(tx("Copy effects in Effect Controls first"));
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq)
        return notify(tx("No sequence"));
    std::vector<ClipId> selection;
    for (const ClipId id : m_controller->selection()) {
        if (seq->findClip(id))
            selection.push_back(id);
    }
    if (selection.empty())
        return notify(tx("Select the clips to paste the effects onto"));

    // Each part goes on the selected clips of its kind (a clip of the other kind stands for its linked partner);
    // clips on locked tracks are left out.
    int lockedSkipped = 0;
    const auto targetsOf = [&](StreamKind kind) {
        std::vector<ClipId> out;
        for (const ClipId id : clipsOfKind(*seq, selection, kind)) {
            const auto loc = seq->findClip(id);
            if (loc && loc->track->locked)
                ++lockedSkipped;
            else if (loc)
                out.push_back(id);
        }
        return out;
    };
    const EffectsClipboard& board = *m_effects;
    const std::vector<ClipId> videoTargets =
        board.video && !board.videoAttributes.empty() ? targetsOf(StreamKind::Video) : std::vector<ClipId>{};
    const std::vector<ClipId> audioTargets =
        board.audio && !board.audioAttributes.empty() ? targetsOf(StreamKind::Audio) : std::vector<ClipId>{};
    const QString lockedNote = lockedSkipped == 0 ? QString()
                             : lockedSkipped == 1 ? tx("1 clip on a locked track was skipped")
                                                  : tx("%1 clips on locked tracks were skipped").arg(lockedSkipped);
    if (videoTargets.empty() && audioTargets.empty()) {
        if (!lockedNote.isEmpty())
            return notify(lockedNote);
        return notify(board.video ? tx("Select video clips to paste the effects onto")
                                  : tx("Select audio clips to paste the effects onto"));
    }

    const SequenceId seqId = seq->id;
    std::set<ClipId> changed;
    const bool ok = m_controller->applyEditFn(LABEL("Paste Effects"), [&](const ProjectState& state) -> edit::EditOutcome {
        changed.clear();
        std::optional<edit::EditResult> last;
        const auto paste = [&](const Clip& source, const std::vector<ClipId>& targets, const AttributeSet& attributes)
            -> std::optional<Error> {
            if (targets.empty())
                return std::nullopt;
            auto r = pasteAttributes(last ? last->state : state, seqId, source, targets, attributes, false);
            if (!r)
                return r.error();
            for (const AttributeChange& c : r->changes) {
                if (!c.applied.empty())
                    changed.insert(c.clip);
            }
            last = std::move(r->edit);
            return std::nullopt;
        };
        if (board.video) {
            if (auto error = paste(*board.video, videoTargets, board.videoAttributes))
                return std::unexpected(*error);
        }
        if (board.audio) {
            if (auto error = paste(*board.audio, audioTargets, board.audioAttributes))
                return std::unexpected(*error);
        }
        if (!last)
            return std::unexpected(Error{ErrorCode::InvalidArgument, "nothing to paste"});
        return std::move(*last);
    });
    if (!ok)
        return notify(m_controller->lastError());
    const int clips = static_cast<int>(changed.size());
    const QString effects = board.count == 1 ? tx("1 effect") : tx("%1 effects").arg(board.count);
    QString text = clips == 1 ? tx("Pasted %1 to 1 clip").arg(effects) : tx("Pasted %1 to %2 clips").arg(effects).arg(clips);
    if (!lockedNote.isEmpty())
        text += QStringLiteral(" · ") + lockedNote;
    emit m_controller->clipboardNotice(text);
    return {};
}

bool EffectControlsModel::canMoveSection(const QString& section, int delta) const {
    const Row* h = findRow(QStringLiteral("h:") + section);
    if (!h || !h->movable || h->stackIndex < 0 || delta == 0)
        return false;
    int count = 0;
    for (const Row& r : m_rows) {
        if (r.kind == QLatin1String("header") && r.stackKind == h->stackKind && r.clip == h->clip && r.stackIndex >= 0)
            ++count;
    }
    const int to = h->stackIndex + delta;
    return to >= 0 && to < count;
}

void EffectControlsModel::moveSectionBy(const QString& section, int delta) {
    if (!canMoveSection(section, delta))
        return;
    moveEffect(section, findRow(QStringLiteral("h:") + section)->stackIndex + delta);
}

bool EffectControlsModel::handlesCommand(const QString& id) {
    return id == QLatin1String("effectControls.removeSelectedEffect");
}

bool EffectControlsModel::runCommand(const QString& commandId) {
    if (!m_controller)
        return false;
    const bool sections = !m_selectedSections.isEmpty();
    if (commandId == QLatin1String("effectControls.removeSelectedEffect")) {
        // Selected keys win (as the panel's Delete claim), then sections, then a shown transition.
        if (!m_selectedKeys.empty())
            deleteSelectedKeys();
        else if (sections)
            removeSelectedSections();
        else if (m_mode == QLatin1String("transition"))
            removeSection(QStringLiteral("transition"));
        else
            setNotice(tx("Select an effect first"));
        return true;
    }
    if (commandId == QLatin1String("edit.clear")) {
        if (!sections)
            return false;
        removeSelectedSections();
        return true;
    }
    if (commandId == QLatin1String("edit.copy") || commandId == QLatin1String("edit.cut")) {
        // 0.6: selected keys first, then sections, then the timeline.
        if (!m_selectedKeys.empty()) {
            copySelectedKeys(commandId == QLatin1String("edit.cut"));
            return true;
        }
        if (!sections)
            return false;
        copySelectedSections(commandId == QLatin1String("edit.cut"));
        return true;
    }
    if (commandId == QLatin1String("edit.paste")) {
        // 0.6: copied keyframes (the newest copy) go onto the clip shown; else the effects or clips paste as usual.
        if (!canPasteKeys() || m_mode != QLatin1String("clip"))
            return false;
        pasteKeys();
        return true;
    }
    if (commandId == QLatin1String("edit.selectAll")) {
        if (m_mode != QLatin1String("clip"))
            return false;
        selectAllSections();
        return true;
    }
    if (commandId == QLatin1String("edit.deselectAll")) {
        if (!sections)
            return false;
        clearSectionSelection();
        return true;
    }
    return false;
}

bool EffectControlsModel::effectsPastePending() const {
    return m_effects != nullptr && m_effectsPending;
}

bool EffectControlsModel::pasteEffectsIfPending() {
    if (!effectsPastePending())
        return false;
    pasteEffects();
    return true;
}

// ---- Adding effects ---------------------------------------------------------------------------------

QVariantList EffectControlsModel::library(const QString& query) const {
    struct Entry {
        int score = 0;
        QString categoryKey; ///< the untranslated category (the bin's stable key)
        QVariantMap item;
    };
    struct Group {
        EffectKind kind;
        QString title, icon;
        std::vector<Entry> items;
    };
    std::vector<Group> groups{
        {EffectKind::Video, tx("Video Effects"), QStringLiteral("sparkles"), {}},
        {EffectKind::VideoTransition, tx("Video Transitions"), QStringLiteral("blend"), {}},
        {EffectKind::Audio, tx("Audio Effects"), QStringLiteral("audio-lines"), {}},
        {EffectKind::AudioTransition, tx("Audio Transitions"), QStringLiteral("blend"), {}},
    };
    static const char* const kKinds[] = {"video", "audio", "videoTransition", "audioTransition"};
    const QString q = query.trimmed();
    for (const EffectDescriptor& d : EffectRegistry::builtin().all()) {
        // Intrinsic effects are every clip's; the title style comes with each title (New Title); the Essential Sound
        // record is the Essential Sound panel's (isListedEffect).
        if (!isListedEffect(d))
            continue;
        auto group = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.kind == d.kind; });
        if (group == groups.end())
            continue;
        const QString name = tx(d.displayName.data());
        const QString category = tx(d.category.data());
        int score = 1;
        if (!q.isEmpty()) {
            score = std::max({fuzzyScore(q, name) * 3, fuzzyScore(q, category), fuzzyScore(q, group->title)});
            if (score <= 0)
                continue;
        }
        group->items.push_back(Entry{score, fromView(d.category), QVariantMap{
            {QStringLiteral("id"), fromView(d.id)},
            {QStringLiteral("name"), name},
            {QStringLiteral("category"), category},
            {QStringLiteral("description"), fromView(d.description)},
            {QStringLiteral("icon"), categoryIcon(d.category, d.kind)},
            {QStringLiteral("kind"), QString::fromLatin1(kKinds[static_cast<int>(d.kind)])},
        }});
    }
    QVariantList out;
    for (Group& g : groups) {
        if (g.items.empty())
            continue;
        const QString groupKey = QString::fromLatin1(kKinds[static_cast<int>(g.kind)]);
        // 0.2: Premiere's bins (the descriptors' categories) in alphabetical order, effects alphabetical inside, as
        // Premiere's Effects panel lists them; a search keeps its matches in their bins.
        const auto text = [](const Entry& e, const char* key) { return e.item.value(QLatin1String(key)).toString(); };
        std::stable_sort(g.items.begin(), g.items.end(), [&](const Entry& a, const Entry& b) {
            const int bin = QString::compare(text(a, "category"), text(b, "category"), Qt::CaseInsensitive);
            return bin != 0 ? bin < 0 : QString::compare(text(a, "name"), text(b, "name"), Qt::CaseInsensitive) < 0;
        });
        QVariantList bins;
        QVariantList binItems;
        QString binTitle;
        QString binKey;
        const auto closeBin = [&] {
            if (!binItems.isEmpty()) {
                bins.append(QVariantMap{{QStringLiteral("title"), binTitle},
                                        {QStringLiteral("key"), groupKey + QLatin1Char('/') + binKey},
                                        {QStringLiteral("count"), static_cast<int>(binItems.size())},
                                        {QStringLiteral("items"), binItems}});
            }
            binItems.clear();
        };
        for (const Entry& entry : g.items) {
            const QString category = text(entry, "category");
            if (category != binTitle) {
                closeBin();
                binTitle = category;
                binKey = entry.categoryKey;
            }
            binItems.append(entry.item);
        }
        closeBin();
        if (!q.isEmpty())
            std::stable_sort(g.items.begin(), g.items.end(), [](const Entry& a, const Entry& b) { return a.score > b.score; });
        QVariantList items;
        for (const Entry& entry : g.items)
            items.append(entry.item);
        out.append(QVariantMap{{QStringLiteral("title"), g.title},
                               {QStringLiteral("icon"), g.icon},
                               {QStringLiteral("kind"), groupKey},
                               {QStringLiteral("key"), groupKey},
                               {QStringLiteral("count"), static_cast<int>(items.size())},
                               {QStringLiteral("items"), items},
                               {QStringLiteral("bins"), bins}});
    }
    return out;
}

std::optional<ClipId> EffectControlsModel::sectionClip(const QString& section) const {
    for (const Row& r : m_rows) {
        if (r.kind == QLatin1String("header") && r.section == section && r.clip.isValid())
            return r.clip;
    }
    return std::nullopt;
}

QStringList EffectControlsModel::sectionsOf(ClipId clip) const {
    QStringList out;
    for (const Row& r : m_rows) {
        if (r.kind == QLatin1String("header") && r.clip == clip && r.sectionKind != QLatin1String("transition"))
            out << r.section;
    }
    return out;
}

bool EffectControlsModel::isTransition(const QString& effectId) const {
    const EffectDescriptor* d = descriptor(effectId.toStdString());
    return d && isTransitionKind(d->kind);
}

QString EffectControlsModel::addEffect(const QString& effectId, int stackIndex) {
    if (!m_controller)
        return tx("No project");
    // 0.2: a preset (Effects panel > Presets) applies to the clip and its linked partner of the preset's kind.
    if (effectId.startsWith(QLatin1String("preset:"))) {
        if (!m_controller->primaryClip())
            return tx("Select a clip first");
        std::vector<ClipId> clips;
        for (const StreamKind kind : {StreamKind::Video, StreamKind::Audio}) {
            if (const auto target = m_controller->propertyTarget(kind))
                clips.push_back(*target);
        }
        return applyEffectPresetToClips(*m_controller, clips, effectId.mid(7));
    }
    const EffectDescriptor* d = descriptor(effectId.toStdString());
    if (!d || d->intrinsic || d->id == edit::kTitleStyleEffect)
        return tx("Unknown effect");
    if (isTransitionKind(d->kind))
        return tx("Drop transitions on an edit point in the timeline");
    if (!m_controller->primaryClip())
        return tx("Select a clip first");
    const bool video = d->kind == EffectKind::Video;
    const auto target = m_controller->propertyTarget(video ? StreamKind::Video : StreamKind::Audio);
    if (!target)
        return video ? tx("%1 is a video effect: select a video clip").arg(tx(d->displayName.data()))
                     : tx("%1 is an audio effect: select an audio clip").arg(tx(d->displayName.data()));
    const SequenceId seq = m_controller->currentSequenceId();
    const ClipId clips[] = {*target};
    const std::optional<std::size_t> index =
        stackIndex >= 0 ? std::optional<std::size_t>(static_cast<std::size_t>(stackIndex)) : std::nullopt;
    const bool ok = m_controller->applyEditFn(LABEL("Add Effect"), [&](const ProjectState& state) {
        return edit::addEffect(state, seq, clips, std::string(d->id), {}, index);
    });
    return ok ? QString() : m_controller->lastError();
}

QString EffectControlsModel::addEffectToClip(const QString& clipId, const QString& effectId) {
    if (!m_controller)
        return tx("No project");
    const auto clip = parseClipKey(clipId);
    const EffectDescriptor* d = descriptor(effectId.toStdString());
    const SequencePtr seq = m_controller->currentSequence();
    if (!clip || !seq || !seq->findClip(*clip))
        return tx("Drop the effect on a clip");
    if (effectId.startsWith(QLatin1String("preset:"))) {
        m_controller->selectClipById(*clip, ProjectController::Replace);
        return addEffect(effectId);
    }
    if (!d || d->intrinsic)
        return tx("Unknown effect");
    if (isTransitionKind(d->kind))
        return tx("Drop transitions on an edit point");
    m_controller->selectClipById(*clip, ProjectController::Replace);
    return addEffect(effectId);
}

QVariantMap EffectControlsModel::applyToSelectedClips(const QString& itemId) {
    const auto done = [](const QString& message, int clips) {
        return QVariantMap{{QStringLiteral("ok"), true},
                           {QStringLiteral("error"), QString()},
                           {QStringLiteral("message"), message},
                           {QStringLiteral("clips"), clips}};
    };
    const auto failed = [](const QString& error) {
        return QVariantMap{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"), error},
                           {QStringLiteral("message"), error},
                           {QStringLiteral("clips"), 0}};
    };
    if (!m_controller)
        return failed(tx("No project"));
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq)
        return failed(tx("No sequence"));
    std::vector<ClipId> selection;
    for (const ClipId id : m_controller->selection()) {
        if (seq->findClip(id))
            selection.push_back(id);
    }
    const SequenceId seqId = m_controller->currentSequenceId();
    QString id = itemId.trimmed();
    // 0.6 (card #71): "master:<item>" puts an effect (or a preset's stack effects) on the master clips of the selected
    // clips' media, one undo step.
    const bool toMaster = id.startsWith(QLatin1String("master:"));
    if (toMaster)
        id = id.mid(7);
    if (id.startsWith(QLatin1String("effect:")))
        id = id.mid(7);
    if (toMaster) {
        const ProjectSnapshot state = m_controller->snapshot();
        if (selection.empty() || !state)
            return failed(tx("Select a clip first"));
        const auto preset = id.startsWith(QLatin1String("preset:")) ? effectPresetForItem(m_controller, id) : std::nullopt;
        const EffectDescriptor* d = preset ? nullptr : descriptor(id.toStdString());
        if (!preset && (!d || !isListedEffect(*d) || isTransitionKind(d->kind) || d->id.starts_with(kPluginEffectPrefix)))
            return failed(tx("Only effects and presets go on a master clip"));
        const bool video = preset ? preset->kind() == EffectKind::Video : d->kind == EffectKind::Video;
        const std::vector<ClipId> clips = clipsOfKind(*seq, selection, video ? StreamKind::Video : StreamKind::Audio);
        // One representative clip per media item (nested and multicam clips have no master clip).
        std::vector<ClipId> representatives;
        std::vector<MediaId> seen;
        int skipped = 0;
        for (const ClipId c : clips) {
            const auto loc = seq->findClip(c);
            if (!loc || loc->clip().isNested() || !loc->clip().angles.empty() || !state->findMedia(loc->clip().media)) {
                ++skipped;
                continue;
            }
            if (std::find(seen.begin(), seen.end(), loc->clip().media) != seen.end())
                continue;
            seen.push_back(loc->clip().media);
            representatives.push_back(c);
        }
        if (representatives.empty())
            return failed(skipped > 0 ? tx("Nested sequences and multicam clips have no master clip effects yet")
                                      : video ? tx("Select a video clip") : tx("Select an audio clip"));
        std::vector<std::function<edit::EditOutcome(const ProjectState&)>> steps;
        if (preset) {
            // The preset's stack effects (keys timed on the representative clip, media time), moved onto its master clip.
            EffectPreset stackOnly = *preset;
            std::erase_if(stackOnly.effects, [](const EffectInstance& e) {
                return e.effect == kMotionEffect || e.effect == kOpacityEffect || e.effect == kVolumeEffect || e.effect == kPannerEffect;
            });
            if (stackOnly.effects.empty())
                return failed(tx("%1 has only Motion, Opacity or Volume: those stay each clip's").arg(id.mid(7)));
            for (const ClipId c : representatives) {
                steps.push_back([=](const ProjectState& s) -> edit::EditOutcome {
                    auto applied = edit::applyEffectPreset(s, seqId, std::vector<ClipId>{c}, stackOnly);
                    if (!applied)
                        return std::unexpected(applied.error());
                    std::vector<std::function<edit::EditOutcome(const ProjectState&)>> moves;
                    for (const EffectInstanceId e : applied->edit.createdEffects)
                        moves.push_back([=](const ProjectState& t) { return edit::moveEffectToMaster(t, seqId, c, e); });
                    auto moved = chainedEdit(applied->edit.state, moves);
                    if (!moved)
                        return moved;
                    moved->changes = diff(s, moved->state);
                    return moved;
                });
            }
        } else {
            const std::string effect(d->id);
            for (const MediaId m : seen) {
                steps.push_back([=](const ProjectState& s) {
                    return edit::addEffect(s, seqId, edit::EffectTarget::ofMedia(m), effect);
                });
            }
        }
        const bool ok = m_controller->applyEditFn(preset ? LABEL("Apply Preset to Master Clip") : LABEL("Add Master Clip Effect"),
                                                  [&](const ProjectState& s) { return chainedEdit(s, steps); });
        if (!ok)
            return failed(m_controller->lastError());
        const int n = static_cast<int>(seen.size());
        const QString name = preset ? id.mid(7) : tx(d->displayName.data());
        return done(n == 1 ? tx("Added %1 to the master clip").arg(name) : tx("Added %1 to %2 master clips").arg(name).arg(n), n);
    }

    // A preset: one "Apply Preset" step over every clip of its kind.
    if (id.startsWith(QLatin1String("preset:"))) {
        const QString name = id.mid(7);
        const auto preset = effectPresetForItem(m_controller, id);
        if (!preset)
            return failed(tx("There is no preset named %1").arg(name));
        if (selection.empty())
            return failed(tx("Select a clip first"));
        const bool video = preset->kind() == EffectKind::Video;
        const std::vector<ClipId> clips = clipsOfKind(*seq, selection, video ? StreamKind::Video : StreamKind::Audio);
        if (clips.empty())
            return failed(video ? tx("%1 is a video preset: select a video clip").arg(name)
                                : tx("%1 is an audio preset: select an audio clip").arg(name));
        if (const QString error = applyEffectPresetToClips(*m_controller, clips, name); !error.isEmpty())
            return failed(error);
        const int n = static_cast<int>(clips.size());
        return done(n == 1 ? tx("Applied %1").arg(name) : tx("Applied %1 to %2 clips").arg(name).arg(n), n);
    }

    const EffectDescriptor* d = descriptor(id.toStdString());
    if (!d || !isListedEffect(*d))
        return failed(tx("Unknown effect"));
    if (d->id.starts_with(kPluginEffectPrefix)) // PluginBridge::applyToSelectedClips keeps the plug-in's name with it
        return failed(tx("%1 is an audio plug-in: apply it from the Audio Plug-ins folder").arg(fromView(d->displayName)));
    const QString name = tx(d->displayName.data());

    // A transition: on both ends of each selected clip of its kind (Apply Default Transitions to Selection with it).
    if (isTransitionKind(d->kind)) {
        const bool video = d->kind == EffectKind::VideoTransition;
        // 0.6 (stream 1A): selected edit points (Trim) take it instead of the clips' ends, as in Premiere.
        if (!m_controller->trimEditPoints().empty()) {
            const int n = m_controller->applyTransitionAtEditPoints(!video, std::string(d->id), LABEL("Apply Transition"));
            if (n == 0)
                return failed(m_controller->lastError());
            return done(n == 1 ? tx("Added %1 to the edit point").arg(name) : tx("Added %1 to %2 edit points").arg(name).arg(n), n);
        }
        if (selection.empty())
            return failed(tx("Select clips, or drag %1 onto an edit point in the timeline").arg(name));
        const std::vector<ClipId> clips = clipsOfKind(*seq, selection, video ? StreamKind::Video : StreamKind::Audio);
        if (clips.empty())
            return failed(video ? tx("%1 is a video transition: select video clips, or drag it onto an edit point").arg(name)
                                : tx("%1 is an audio transition: select audio clips, or drag it onto an edit point").arg(name));
        edit::DefaultTransitions defaults = EditingBridge::defaultTransitionsSetting();
        (video ? defaults.video : defaults.audio) = std::string(d->id);
        const bool ok = m_controller->applyEditFn(LABEL("Apply Transition"), [&](const ProjectState& state) {
            return edit::applyDefaultTransitions(state, seqId, clips, defaults);
        });
        if (!ok)
            return failed(m_controller->lastError());
        const int n = static_cast<int>(clips.size());
        return done(n == 1 ? tx("Added %1 to the clip's ends").arg(name) : tx("Added %1 to the ends of %2 clips").arg(name).arg(n), n);
    }

    // An effect: one "Add Effect" step, a new instance at the end of each clip's stack.
    if (selection.empty())
        return failed(tx("Select a clip first"));
    const bool video = d->kind == EffectKind::Video;
    const std::vector<ClipId> clips = clipsOfKind(*seq, selection, video ? StreamKind::Video : StreamKind::Audio);
    if (clips.empty())
        return failed(video ? tx("%1 is a video effect: select a video clip").arg(name)
                            : tx("%1 is an audio effect: select an audio clip").arg(name));
    const std::string effect(d->id);
    const bool ok = m_controller->applyEditFn(LABEL("Add Effect"), [&](const ProjectState& state) {
        return edit::addEffect(state, seqId, std::span<const ClipId>(clips), effect);
    });
    if (!ok)
        return failed(m_controller->lastError());
    const int n = static_cast<int>(clips.size());
    return done(n == 1 ? tx("Added %1").arg(name) : tx("Added %1 to %2 clips").arg(name).arg(n), n);
}

// ---- Keyframe lane ----------------------------------------------------------------------------------

QString EffectControlsModel::selectedKeyInterpolation() const {
    if (m_selectedKeys.empty() || !m_controller)
        return {};
    std::optional<Interpolation> common;
    for (const KeyRef& k : m_selectedKeys) {
        const Row* r = findRow(k.row);
        const Clip* clip = r ? clipOf(*r) : nullptr;
        if (!clip)
            continue;
        const ParamValue value = paramValueOf(m_controller, *clip, r->ref);
        const Curve* curve = value.curve();
        const auto at = curve ? curve->keyAt(Ticks{k.curveTime}) : std::nullopt;
        if (!at)
            continue;
        const Interpolation i = curve->keys[*at].interpolation;
        if (common && *common != i)
            return {};
        common = i;
    }
    return common ? interpolationName(*common) : QString();
}

bool EffectControlsModel::pressKey(const QString& rowKey, double x, double tolerance, int modifiers) {
    const Row* r = findRow(rowKey);
    const Clip* clip = r ? clipOf(*r) : nullptr;
    const bool add = mods(modifiers).testFlag(Qt::ShiftModifier);
    const bool toggle = mods(modifiers).testAnyFlags(Qt::ControlModifier | Qt::MetaModifier);
    std::optional<KeyRef> hit;
    if (clip) {
        const ParamValue value = paramValueOf(m_controller, *clip, r->ref);
        if (const Curve* curve = value.curve()) {
            double best = tolerance;
            const double laneLength = static_cast<double>(std::max<std::int64_t>(1, m_lane.duration.value));
            for (const auto& [i, t] : visibleKeys(*r, *curve, *clip)) {
                const double kx = static_cast<double>((t - m_lane.start).value) / laneLength;
                if (std::abs(kx - x) <= best) {
                    best = std::abs(kx - x);
                    hit = KeyRef{rowKey, curve->keys[i].time.value};
                }
            }
        }
    }
    const std::size_t before = m_selectedKeys.size();
    if (hit) {
        if (toggle) {
            if (!m_selectedKeys.erase(*hit))
                m_selectedKeys.insert(*hit);
        } else if (add) {
            m_selectedKeys.insert(*hit);
        } else if (!m_selectedKeys.contains(*hit)) {
            m_selectedKeys = {*hit};
        }
    } else if (!add && !toggle) {
        m_selectedKeys.clear();
    }
    if (hit && !m_selectedKeys.empty())
        setSectionSelection({}, {}); // 0.4: selecting a keyframe clears the selected sections
    if (hit || before != m_selectedKeys.size()) {
        refresh();
        emit keySelectionChanged();
    }
    return hit.has_value();
}

void EffectControlsModel::selectKeysInRange(const QStringList& rows, double x0, double x1, int modifiers) {
    if (!mods(modifiers).testAnyFlags(Qt::ShiftModifier | Qt::ControlModifier | Qt::MetaModifier))
        m_selectedKeys.clear();
    const double lo = std::min(x0, x1);
    const double hi = std::max(x0, x1);
    const double laneLength = static_cast<double>(std::max<std::int64_t>(1, m_lane.duration.value));
    for (const QString& rowKey : rows) {
        const Row* r = findRow(rowKey);
        const Clip* clip = r ? clipOf(*r) : nullptr;
        if (!clip)
            continue;
        const ParamValue value = paramValueOf(m_controller, *clip, r->ref);
        if (const Curve* curve = value.curve()) {
            for (const auto& [i, t] : visibleKeys(*r, *curve, *clip)) {
                const double kx = static_cast<double>((t - m_lane.start).value) / laneLength;
                if (kx >= lo && kx <= hi)
                    m_selectedKeys.insert(KeyRef{rowKey, curve->keys[i].time.value});
            }
        }
    }
    if (!m_selectedKeys.empty())
        setSectionSelection({}, {}); // 0.4: selecting keyframes clears the selected sections
    refresh();
    emit keySelectionChanged();
}

void EffectControlsModel::clearKeySelection() {
    if (m_selectedKeys.empty())
        return;
    m_selectedKeys.clear();
    refresh();
    emit keySelectionChanged();
}

bool EffectControlsModel::beginKeyDrag(bool duplicate) {
    m_drag = {};
    if (m_selectedKeys.empty() || !m_controller)
        return false;
    std::map<QString, DragGroup> groups;
    for (const KeyRef& k : m_selectedKeys) {
        const Row* r = findRow(k.row);
        const Clip* clip = r ? clipOf(*r) : nullptr;
        if (!clip)
            continue;
        const ParamValue value = paramValueOf(m_controller, *clip, r->ref);
        const Curve* curve = value.curve();
        if (!curve || !curve->keyAt(Ticks{k.curveTime}))
            continue;
        DragGroup& g = groups[k.row];
        if (g.row.isEmpty()) {
            g.row = k.row;
            g.clip = clip->id;
            g.ref = r->ref;
            g.original = *curve;
        }
        g.moving.push_back(Ticks{k.curveTime});
    }
    if (groups.empty())
        return false;
    m_drag.active = true;
    m_drag.duplicate = duplicate;
    for (auto& [row, g] : groups)
        m_drag.groups.push_back(std::move(g));
    m_drag.mergeKey = "keyframe-drag:" + std::to_string(++m_dragCounter);
    return true;
}

void EffectControlsModel::dragKeys(double dx) {
    if (!m_drag.active || !m_controller || !std::isfinite(dx))
        return;
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq)
        return;
    const Ticks frame = m_controller->frameTicks();
    Ticks delta{static_cast<std::int64_t>(std::llround(dx * static_cast<double>(m_lane.duration.value)))};
    delta = Ticks{std::llround(static_cast<double>(delta.value) / static_cast<double>(frame.value)) * frame.value};

    // Keep every dragged key inside its clip.
    Ticks lo = Ticks::min(), hi = Ticks::max();
    for (const DragGroup& g : m_drag.groups) {
        const auto loc = seq->findClip(g.clip);
        const Row* row = findRow(g.row);
        if (!loc || !row)
            return;
        const Clip& clip = loc->clip();
        for (Ticks t : g.moving) {
            const auto tl = rowTimelineTime(*row, clip, t);
            if (!tl)
                continue;
            lo = std::max(lo, clip.start() - *tl);
            hi = std::min(hi, clip.end() - frame - *tl);
        }
    }
    delta = std::clamp(delta, std::min(lo, Ticks{}), std::max(hi, Ticks{}));

    struct Result {
        const DragGroup* group;
        Curve curve;
        std::vector<Ticks> times;
    };
    std::vector<Result> results;
    for (const DragGroup& g : m_drag.groups) {
        const Clip& clip = seq->findClip(g.clip)->clip();
        const Row& row = *findRow(g.row);
        Result res{&g, g.original, {}};
        if (delta == Ticks{} && m_drag.duplicate) {
            res.times = g.moving; // back to the originals: no copies yet
            results.push_back(std::move(res));
            continue;
        }
        std::vector<Keyframe> moved;
        for (Ticks t : g.moving) {
            if (const auto i = res.curve.keyAt(t))
                moved.push_back(res.curve.keys[*i]);
        }
        if (!m_drag.duplicate) {
            std::erase_if(res.curve.keys, [&](const Keyframe& k) {
                return std::find(g.moving.begin(), g.moving.end(), k.time) != g.moving.end();
            });
        }
        for (Keyframe k : moved) {
            const auto tl = rowTimelineTime(row, clip, k.time);
            if (!tl)
                continue;
            k.time = rowCurveTime(row, clip, *tl + delta);
            if (res.curve.keyAt(k.time))
                return; // onto another key: the keys stay where they were last
            res.curve.keys.insert(res.curve.keys.begin() + static_cast<std::ptrdiff_t>(res.curve.insertionIndex(k.time)), k);
            res.times.push_back(k.time);
        }
        results.push_back(std::move(res));
    }

    const SequenceId seqId = seq->id;
    std::vector<std::function<edit::EditOutcome(const ProjectState&)>> steps;
    for (const Result& res : results) {
        const ClipId clip = res.group->clip;
        const edit::ParamRef ref = res.group->ref;
        const Curve curve = res.curve;
        steps.push_back([=](const ProjectState& state) { return edit::setParameter(state, seqId, clip, ref, ParamValue(curve)); });
    }
    const bool ok = m_controller->applyEditFn(m_drag.duplicate ? LABEL("Duplicate Keyframes") : LABEL("Move Keyframes"),
                                              [&](const ProjectState& state) { return chain(state, steps); },
                                              m_drag.mergeKey);
    if (!ok)
        return;
    std::set<KeyRef> selection;
    for (const Result& res : results) {
        for (Ticks t : res.times)
            selection.insert(KeyRef{res.group->row, t.value});
    }
    if (selection != m_selectedKeys) {
        m_selectedKeys = std::move(selection);
        refresh();
        emit keySelectionChanged();
    }
}

void EffectControlsModel::endKeyDrag() {
    if (!m_drag.active)
        return;
    m_drag = {};
    endEdit();
}

void EffectControlsModel::setKeyInterpolation(const QString& interpolation) {
    const auto value = interpolationFromString(interpolation.toStdString());
    if (!value || m_selectedKeys.empty() || !m_controller)
        return;
    const SequenceId seq = m_controller->currentSequenceId();
    std::map<QString, std::pair<const Row*, Curve>> curves;
    for (const KeyRef& k : m_selectedKeys) {
        const Row* r = findRow(k.row);
        const Clip* clip = r ? clipOf(*r) : nullptr;
        if (!clip)
            continue;
        auto it = curves.find(k.row);
        if (it == curves.end()) {
            const ParamValue v = paramValueOf(m_controller, *clip, r->ref);
            if (!v.curve())
                continue;
            it = curves.emplace(k.row, std::make_pair(r, *v.curve())).first;
        }
        Curve& curve = it->second.second;
        if (const auto i = curve.keyAt(Ticks{k.curveTime})) {
            Keyframe& key = curve.keys[*i];
            if (*value == Interpolation::Bezier && key.interpolation != Interpolation::Bezier) {
                // Smooth tangents (Premiere's Auto Bezier): the slope between the neighbours, a third of each segment.
                const std::size_t a = *i > 0 ? *i - 1 : *i;
                const std::size_t b = *i + 1 < curve.keys.size() ? *i + 1 : *i;
                const double span = (curve.keys[b].time - curve.keys[a].time).toSeconds();
                const double slope = span > 0 ? (curve.keys[b].value - curve.keys[a].value) / span : 0.0;
                key.in = BezierHandle{slope, 1.0 / 3.0};
                key.out = BezierHandle{slope, 1.0 / 3.0};
            }
            key.interpolation = *value;
        }
    }
    std::vector<std::function<edit::EditOutcome(const ProjectState&)>> steps;
    for (const auto& [row, entry] : curves) {
        const ClipId clip = entry.first->clip;
        const edit::ParamRef ref = entry.first->ref;
        const Curve curve = entry.second;
        steps.push_back([=](const ProjectState& state) { return edit::setParameter(state, seq, clip, ref, ParamValue(curve)); });
    }
    if (!steps.empty())
        m_controller->applyEditFn(LABEL("Keyframe Interpolation"), [&](const ProjectState& state) { return chain(state, steps); });
}

void EffectControlsModel::deleteSelectedKeys() {
    deleteKeys(LABEL("Delete Keyframes"));
}

void EffectControlsModel::deleteKeys(const char* label) {
    if (m_selectedKeys.empty() || !m_controller)
        return;
    const SequenceId seq = m_controller->currentSequenceId();
    std::vector<std::function<edit::EditOutcome(const ProjectState&)>> steps;
    std::map<QString, std::vector<Ticks>> byRow;
    for (const KeyRef& k : m_selectedKeys)
        byRow[k.row].push_back(Ticks{k.curveTime});
    for (const auto& [rowKey, times] : byRow) {
        const Row* r = findRow(rowKey);
        const Clip* clip = r ? clipOf(*r) : nullptr;
        if (!clip)
            continue;
        const ParamValue v = paramValueOf(m_controller, *clip, r->ref);
        const Curve* curve = v.curve();
        if (!curve)
            continue;
        Curve rest = *curve;
        std::erase_if(rest.keys, [&](const Keyframe& k) { return std::find(times.begin(), times.end(), k.time) != times.end(); });
        // No key left: the parameter keeps the value it has at the playhead (Premiere).
        const ParamValue next = rest.keys.empty() ? ParamValue(v.valueAt(rowCurveTimeAtPlayhead(*r, *clip)))
                                                  : ParamValue(rest);
        const ClipId id = clip->id;
        const edit::ParamRef ref = r->ref;
        steps.push_back([=](const ProjectState& state) { return edit::setParameter(state, seq, id, ref, next); });
    }
    m_selectedKeys.clear();
    if (!steps.empty())
        m_controller->applyEditFn(label, [&](const ProjectState& state) { return chain(state, steps); });
    emit keySelectionChanged();
}

void EffectControlsModel::seekLane(double x) {
    if (!m_controller || m_lane.duration <= Ticks{} || !std::isfinite(x))
        return;
    const double clamped = std::clamp(x, 0.0, 1.0);
    m_controller->setPlayhead(m_lane.start + Ticks{static_cast<std::int64_t>(std::llround(clamped * static_cast<double>(m_lane.duration.value)))});
}

// ---- Lane zoom (0.5.1) ------------------------------------------------------------------------------

double EffectControlsModel::minLaneSpan() const {
    const Ticks frame = m_controller ? m_controller->frameTicks() : Ticks{};
    if (m_lane.duration <= Ticks{} || frame <= Ticks{})
        return 1.0;
    return std::min(1.0, 5.0 * static_cast<double>(frame.value) / static_cast<double>(m_lane.duration.value));
}

void EffectControlsModel::zoomLaneAt(double factor, double at) {
    if (!std::isfinite(factor) || factor <= 0.0 || !std::isfinite(at))
        return;
    const double pointer = std::clamp(at, 0.0, 1.0);
    const double span = m_viewEnd - m_viewStart;
    const double next = std::clamp(span / factor, minLaneSpan(), 1.0);
    const double start = m_viewStart + pointer * (span - next);
    setLaneView(start, start + next);
}

void EffectControlsModel::panLane(double dx) {
    if (std::isfinite(dx))
        setLaneView(m_viewStart + dx, m_viewEnd + dx);
}

void EffectControlsModel::setLaneView(double start, double end) {
    if (!std::isfinite(start) || !std::isfinite(end))
        return;
    const double span = std::clamp(end - start, minLaneSpan(), 1.0);
    if (span >= 1.0 - 1e-9) {
        start = 0.0;
        end = 1.0;
    } else {
        start = std::clamp(start, 0.0, 1.0 - span);
        end = start + span;
    }
    if (start == m_viewStart && end == m_viewEnd)
        return;
    m_viewStart = start;
    m_viewEnd = end;
    m_viewChanged = true;
    if (m_refreshing) {
        // Asked from a laneChanged handler (the panel pages with the playhead during playback): the panel hears of the
        // view now, the timecodes and the value graphs follow once this rebuild is done.
        QMetaObject::invokeMethod(this, [this] { refresh(); }, Qt::QueuedConnection);
        emit laneChanged();
        return;
    }
    refresh(); // the lane's timecodes and the open value graphs
    if (m_viewChanged) { // no rebuild ran (no controller): the panel still hears of the new view
        m_viewChanged = false;
        emit laneChanged();
    }
}

void EffectControlsModel::resetLaneZoom() {
    setLaneView(0.0, 1.0);
}

// ---- Transition rows --------------------------------------------------------------------------------

// ---- Value graph and Bezier handles (M5g) -------------------------------------------------------------

void EffectControlsModel::setGraphOpen(const QString& rowKey, bool open) {
    if (open == m_graphRows.contains(rowKey))
        return;
    if (open)
        m_graphRows.insert(rowKey);
    else
        m_graphRows.erase(rowKey);
    refresh();
}

bool EffectControlsModel::pressHandle(const QString& rowKey, double x, double y, double toleranceX, double toleranceY) {
    const Row* r = findRow(rowKey);
    if (!r || !r->graphOpen || !m_controller)
        return false;
    for (const QVariant& value : r->graph.value(QStringLiteral("keys")).toList()) {
        const QVariantMap key = value.toMap();
        for (const bool out : {true, false}) {
            const QString side = out ? QStringLiteral("out") : QStringLiteral("in");
            if (!key.value(out ? QStringLiteral("hasOut") : QStringLiteral("hasIn")).toBool())
                continue;
            const double hx = key.value(side + QStringLiteral("X")).toDouble();
            const double hy = key.value(side + QStringLiteral("Y")).toDouble();
            if (std::abs(hx - x) > toleranceX || std::abs(hy - y) > toleranceY)
                continue;
            m_handleDrag.active = true;
            m_handleDrag.row = rowKey;
            m_handleDrag.index = static_cast<std::size_t>(key.value(QStringLiteral("index")).toInt());
            m_handleDrag.out = out;
            m_handleDrag.min = r->graph.value(QStringLiteral("min")).toDouble();
            m_handleDrag.max = r->graph.value(QStringLiteral("max")).toDouble();
            m_handleDrag.mergeKey = "keyframe-handle:" + std::to_string(++m_dragCounter);
            return true;
        }
    }
    return false;
}

void EffectControlsModel::dragHandle(double x, double y, bool broken) {
    if (!m_handleDrag.active || !m_controller)
        return;
    const Row* r = findRow(m_handleDrag.row);
    const Clip* clip = r ? clipOf(*r) : nullptr;
    if (!clip)
        return;
    const ParamValue value = paramValueOf(m_controller, *clip, r->ref);
    const Curve* curve = value.curve();
    const std::size_t i = m_handleDrag.index;
    if (!curve || i >= curve->keys.size())
        return;
    const Keyframe& k = curve->keys[i];
    const bool out = m_handleDrag.out;
    if (out ? i + 1 >= curve->keys.size() : i == 0)
        return;
    const double laneLength = static_cast<double>(std::max<std::int64_t>(1, m_lane.duration.value));
    const Ticks t{m_lane.start.value + static_cast<std::int64_t>(std::llround(x * laneLength))};
    const Ticks curveTime = rowCurveTime(*r, *clip, std::clamp(t, clip->start(), clip->end()));
    const double v = r->range.toModel(m_handleDrag.max - y * (m_handleDrag.max - m_handleDrag.min), r->frame);
    // The handle's time along its segment is the influence (at least 1 %), its height over that time the slope.
    const double segment = out ? (curve->keys[i + 1].time - k.time).toSeconds() : (k.time - curve->keys[i - 1].time).toSeconds();
    if (segment <= 0)
        return;
    const double along = std::clamp(out ? (curveTime - k.time).toSeconds() : (k.time - curveTime).toSeconds(),
                                     segment * 0.01, segment);
    const BezierHandle handle{out ? (v - k.value) / along : (k.value - v) / along, along / segment};
    edit::KeyframeChange change;
    if (out)
        change.out = handle;
    else
        change.in = handle;
    // Linked handles: the other side takes the same slope and keeps its length.
    if (!broken) {
        if (out && i > 0)
            change.in = BezierHandle{handle.slope, k.in.influence};
        if (!out && i + 1 < curve->keys.size())
            change.out = BezierHandle{handle.slope, k.out.influence};
    }
    const SequenceId seq = m_controller->currentSequenceId();
    const ClipId clipId = clip->id;
    const edit::ParamRef ref = r->ref;
    m_controller->applyEditFn(
        LABEL("Keyframe Handles"),
        [&](const ProjectState& state) { return edit::setKeyframe(state, seq, clipId, ref, i, change); },
        m_handleDrag.mergeKey);
}

void EffectControlsModel::endHandleDrag() {
    if (!m_handleDrag.active)
        return;
    m_handleDrag = {};
    endEdit();
    refresh();
}

bool EffectControlsModel::setTransitionSides(Ticks before, Ticks after, const char* label) {
    const TransitionId id = m_controller ? m_controller->selectedTransitionId() : TransitionId{};
    if (!id || before < Ticks{} || after < Ticks{} || before + after <= Ticks{})
        return false;
    setNotice({});
    const SequenceId seq = m_controller->currentSequenceId();
    edit::TransitionChange change;
    change.before = before;
    change.after = after;
    if (m_controller->applyEditFn(label, [&](const ProjectState& state) { return edit::setTransition(state, seq, id, change); }))
        return true;
    const QString error = m_controller->lastError();
    if (m_controller->applyEditFn(label, [&](const ProjectState& state) {
            return edit::setTransition(state, seq, id, change, edit::HandleFit::UseAvailable);
        })) {
        setNotice(tx("Shortened to the available media"));
        return true;
    }
    setNotice(error);
    return false;
}

void EffectControlsModel::setTransitionDuration(const QString& timecode) {
    const auto loc = m_controller ? m_controller->findTransition(m_controller->selectedTransitionId()) : std::nullopt;
    const auto length = m_controller ? m_controller->parseTimecodeText(timecode) : std::nullopt;
    if (!loc)
        return;
    if (!length || *length <= Ticks{}) {
        setNotice(tx("Type a length such as 00:00:01:00"));
        return;
    }
    const Transition& t = *loc->transition;
    const Ticks frame = m_controller->frameTicks();
    const Ticks d = std::max(frame, snapToFrame(*length, m_controller->currentSequence()->frameRate));
    Ticks before, after;
    if (!t.incoming || t.after == Ticks{}) {
        before = d;
    } else if (!t.outgoing || t.before == Ticks{}) {
        after = d;
    } else {
        const double share = static_cast<double>(t.before.value) / static_cast<double>(t.duration().value);
        before = Ticks{std::llround(share * static_cast<double>(d.value) / static_cast<double>(frame.value)) * frame.value};
        after = d - before;
    }
    setTransitionSides(before, after, LABEL("Transition Duration"));
}

void EffectControlsModel::setTransitionAlignment(const QString& alignment) {
    const auto loc = m_controller ? m_controller->findTransition(m_controller->selectedTransitionId()) : std::nullopt;
    if (!loc || !loc->transition->outgoing || !loc->transition->incoming)
        return;
    const Ticks d = loc->transition->duration();
    const Ticks frame = m_controller->frameTicks();
    Ticks before, after;
    if (alignment == QLatin1String("startAtCut")) {
        after = d;
    } else if (alignment == QLatin1String("endAtCut")) {
        before = d;
    } else if (alignment == QLatin1String("center")) {
        before = Ticks{(d.value / frame.value / 2) * frame.value};
        after = d - before;
    } else {
        return;
    }
    setTransitionSides(before, after, LABEL("Transition Alignment"));
}

void EffectControlsModel::setTransitionEffect(const QString& effectId) {
    const TransitionId id = m_controller ? m_controller->selectedTransitionId() : TransitionId{};
    const EffectDescriptor* d = descriptor(effectId.toStdString());
    if (!id || !d || !isTransitionKind(d->kind))
        return;
    setNotice({});
    const SequenceId seq = m_controller->currentSequenceId();
    edit::TransitionChange change;
    change.effect = std::string(d->id);
    change.params = std::vector<EffectParam>{};
    if (d->id == kSurpriseAnimation) {
        // 0.2 Surprise Me: a new pick each time it is chosen.
        PickedAnimation picked = surpriseAnimation(QRandomGenerator::global()->generate64());
        change.effect = std::move(picked.effect);
        change.params = std::move(picked.params);
    }
    if (!m_controller->applyEditFn(LABEL("Change Transition"), [&](const ProjectState& state) {
            return edit::setTransition(state, seq, id, change);
        }))
        setNotice(m_controller->lastError());
}


// ---- Text sections (0.6) ----------------------------------------------------------------------------

namespace {

UiRange uiOf(double from, double to, double step, int decimals, const char* unit, bool bipolar = false) {
    UiRange r;
    r.from = from;
    r.to = to;
    r.step = step;
    r.decimals = decimals;
    r.unit = unit;
    r.bipolar = bipolar;
    return r;
}

QString hexOf(const std::array<double, 4>& rgba) {
    return QColor::fromRgbF(static_cast<float>(std::clamp(rgba[0], 0.0, 1.0)), static_cast<float>(std::clamp(rgba[1], 0.0, 1.0)),
                            static_cast<float>(std::clamp(rgba[2], 0.0, 1.0)))
        .name(QColor::HexRgb)
        .toUpper();
}

/// "stroke2.width" -> 2 (-1: not a stroke row).
int strokeIndexOf(const QString& prop) {
    if (!prop.startsWith(QLatin1String("stroke")))
        return -1;
    const qsizetype dot = prop.indexOf(QLatin1Char('.'));
    bool ok = false;
    const int i = prop.mid(6, dot < 0 ? -1 : dot - 6).toInt(&ok);
    return ok ? i : -1;
}

/// The font styles of a family for the Font Style row ("Regular" when the database knows none), `current` among them.
QStringList stylesOf(const QString& family, const QString& current) {
    QStringList styles = QFontDatabase::styles(family);
    if (styles.isEmpty())
        styles << QStringLiteral("Regular");
    if (!current.isEmpty() && !styles.contains(current))
        styles.prepend(current);
    return styles;
}

QString defaultFamily() {
    return QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
}

} // namespace

void EffectControlsModel::addTextSections(std::vector<Row>& rows, const Clip& clip) {
    const Graphic* graphic = clip.graphic();
    if (!graphic || !m_controller)
        return;
    std::vector<std::uint32_t> ids = edit::textLayerIds(*graphic);
    std::reverse(ids.begin(), ids.end()); // the top layer first, as the Essential Graphics panel lists them
    const Ticks playhead = std::clamp(m_controller->playhead(), clip.start(), std::max(clip.start(), clip.end() - Ticks{1}));
    const Ticks time = graphicTimeAt(clip, *graphic, playhead);
    const ProjectSnapshot state = m_controller->snapshot();
    const BrandKit* kit = state && state->brandKit ? &*state->brandKit : nullptr;
    const PropertyFrame frame = m_controller->propertyFrame(clip);
    for (const std::uint32_t id : ids) {
        const GraphicLayer* layer = graphic->findLayer(id);
        if (!layer)
            continue;
        const QString section = QStringLiteral("g") + QString::number(id);
        Row h;
        h.key = QStringLiteral("h:") + section;
        h.kind = QStringLiteral("header");
        h.section = section;
        h.sectionKind = QStringLiteral("text");
        h.title = tx("Text (%1)").arg(layer->name.empty() ? tx("Text") : QString::fromStdString(layer->name));
        h.icon = QStringLiteral("type");
        h.collapsed = m_collapsed.contains(section);
        h.clip = clip.id;
        h.layer = id;
        h.effectEnabled = layer->visible;
        h.description = tx("The text layer's text, look and transform (the Essential Graphics panel edits the same layer)");
        const bool collapsed = h.collapsed;
        rows.push_back(std::move(h));
        if (collapsed)
            continue;

        const TextContent shown = textAt(layer->text, time); // Source Text and size keys at the playhead
        const TextStyle style = shown.runs.empty() ? TextStyle{} : shown.runs.front().style;
        const auto add = [&](const QString& key, const QString& kind, const QString& title) -> Row& {
            Row r;
            r.key = section + QLatin1Char(':') + key;
            r.kind = kind;
            r.section = section;
            r.sectionKind = QStringLiteral("text");
            r.title = title;
            r.clip = clip.id;
            r.layer = id;
            r.textProp = key;
            r.effectEnabled = layer->visible;
            r.frame = frame;
            rows.push_back(std::move(r));
            return rows.back();
        };
        const auto number = [&](const QString& key, const std::string& path, const QString& title, const UiRange& range,
                                 const char* undo) {
            const auto info = edit::layerParamInfo(*layer, path);
            addParamRow(rows, clip, section + QLatin1Char(':') + key, section, QStringLiteral("text"),
                        edit::ParamRef::ofLayer(id, path), title, QString(), range, info ? info->defaultValue : 0.0, true,
                        layer->visible, undo);
            rows.back().layer = id;
            rows.back().textProp = key;
        };
        const auto plain = [&](const QString& key, const QString& title, const UiRange& range, double value, double fallback) {
            Row& r = add(key, QStringLiteral("param"), title);
            r.range = range;
            r.unit = unitText(range);
            r.from = range.from;
            r.to = range.to;
            r.step = range.step;
            r.decimals = range.decimals;
            r.origin = range.bipolar ? 0.0 : r.from;
            r.value = value;
            r.defaultValue = fallback;
            r.resettable = true;
        };
        const auto toggle = [&](const QString& key, const QString& title, bool on) {
            Row& r = add(key, QStringLiteral("toggle"), title);
            r.range = uiOf(0.0, 1.0, 1.0, 0, "");
            r.from = 0.0;
            r.to = 1.0;
            r.value = on ? 1.0 : 0.0;
        };
        const auto choice = [&](const QString& key, const QString& title, const QStringList& labels, int current) {
            Row& r = add(key, QStringLiteral("choice"), title);
            for (qsizetype i = 0; i < labels.size(); ++i)
                r.options.append(QVariantMap{{QStringLiteral("value"), QString::number(i)}, {QStringLiteral("label"), labels[i]}});
            r.range = uiOf(0.0, std::max<double>(0.0, static_cast<double>(labels.size() - 1)), 1.0, 0, "");
            r.from = r.range.from;
            r.to = r.range.to;
            r.value = current;
            r.text = QString::number(current);
        };
        const auto color = [&](const QString& key, const QString& title, const GraphicColor& c) -> Row& {
            Row& r = add(key, QStringLiteral("color"), title);
            r.text = hexOf(resolveColor(c, kit));
            r.description = c.brand.empty() ? QString() : tx("Brand colour %1").arg(QString::fromStdString(c.brand));
            return r;
        };
        const auto group = [&](const QString& key, const QString& title, const QString& action = {}) {
            add(key, QStringLiteral("group"), title).text = action;
        };

        // ---- Source Text ----
        addParamRow(rows, clip, section + QStringLiteral(":text"), section, QStringLiteral("text"),
                    edit::ParamRef::ofLayer(id, "text.source"), tx("Source Text"), QString(),
                    uiOf(0.0, std::max(1.0, static_cast<double>(layer->text.sources.size())), 1.0, 0, ""), 0.0, true,
                    layer->visible, LABEL("Source Text"));
        {
            Row& r = rows.back();
            r.kind = QStringLiteral("textarea");
            r.layer = id;
            r.textProp = QStringLiteral("text");
            r.text = QString::fromStdString(shown.text());
            r.resettable = false;
            r.description = tx("Turn on the stopwatch to change the text over time (hold keyframes)");
        }
        QString family = QString::fromStdString(style.fontFamily);
        if (!style.brandFont.empty() && kit) {
            if (const BrandFont* f = kit->font(style.brandFont))
                family = QString::fromStdString(f->family);
        }
        if (family.isEmpty())
            family = defaultFamily();
        add(QStringLiteral("font"), QStringLiteral("font"), tx("Font")).text = family;
        const QStringList styles = stylesOf(family, QString::fromStdString(style.fontStyle));
        const QString currentStyle = style.fontStyle.empty() ? (styles.contains(QStringLiteral("Regular")) ? QStringLiteral("Regular") : styles.front())
                                                             : QString::fromStdString(style.fontStyle);
        choice(QStringLiteral("fontStyle"), tx("Font Style"), styles, static_cast<int>(std::max<qsizetype>(0, styles.indexOf(currentStyle))));
        number(QStringLiteral("size"), "text.fontSize", tx("Font Size"), uiOf(1.0, 2000.0, 1.0, 0, "px"), LABEL("Font Size"));
        choice(QStringLiteral("align"), tx("Alignment"), {tx("Left"), tx("Center"), tx("Right"), tx("Justify")},
               static_cast<int>(shown.align));
        if (shown.boxWidth > 0.0 && shown.boxHeight > 0.0 && !shown.path)
            choice(QStringLiteral("valign"), tx("Vertical Alignment"), {tx("Top"), tx("Center"), tx("Bottom")},
                   static_cast<int>(shown.verticalAlign));
        number(QStringLiteral("tracking"), "text.tracking", tx("Tracking"), uiOf(-1000.0, 2000.0, 1.0, 0, ""), LABEL("Tracking"));
        toggle(QStringLiteral("kerning"), tx("Kerning"), style.kerning);
        number(QStringLiteral("leading"), "text.leading", tx("Leading"), uiOf(0.0, 1000.0, 1.0, 0, "px"), LABEL("Leading"));
        number(QStringLiteral("baselineShift"), "text.baselineShift", tx("Baseline Shift"), uiOf(-500.0, 500.0, 1.0, 0, "px", true),
               LABEL("Baseline Shift"));
        choice(QStringLiteral("caps"), tx("Capitals"), {tx("Normal"), tx("All Caps"), tx("Small Caps")}, static_cast<int>(style.caps));
        toggle(QStringLiteral("fauxBold"), tx("Faux Bold"), style.fauxBold);
        toggle(QStringLiteral("fauxItalic"), tx("Faux Italic"), style.fauxItalic);
        toggle(QStringLiteral("underline"), tx("Underline"), style.underline);

        // ---- Appearance ----
        group(QStringLiteral("appearanceGroup"), tx("Appearance"));
        color(QStringLiteral("fill"), tx("Fill"), style.fill);
        group(QStringLiteral("strokes"), tx("Strokes"),
              layer->text.strokes.size() < kMaxTextStrokes ? QStringLiteral("addStroke") : QString());
        for (std::size_t i = 0; i < layer->text.strokes.size(); ++i) {
            const TextStroke& st = layer->text.strokes[i];
            const QString n = QString::number(i);
            Row& c = color(QStringLiteral("stroke") + n,
                           i == 0 ? tx("Stroke") : tx("Stroke %1").arg(static_cast<int>(i) + 1), st.color);
            c.removable = true;
            if (!st.enabled)
                c.title = tx("%1 (off)").arg(c.title);
            number(QStringLiteral("stroke") + n + QStringLiteral(".width"), "text.stroke" + std::to_string(i) + ".width",
                   tx("Stroke Width"), uiOf(0.0, 200.0, 0.5, 1, "px"), LABEL("Stroke Width"));
            choice(QStringLiteral("stroke") + n + QStringLiteral(".position"), tx("Stroke Position"),
                   {tx("Outside"), tx("Center"), tx("Inside")}, static_cast<int>(st.position));
        }
        const TextBackground& bg = layer->text.background;
        toggle(QStringLiteral("background"), tx("Background"), bg.enabled);
        if (bg.enabled) {
            color(QStringLiteral("background.color"), tx("Background Color"), bg.color);
            plain(QStringLiteral("background.opacity"), tx("Background Opacity"), uiOf(0.0, 100.0, 1.0, 0, "%"),
                  bg.color.rgba[3] * 100.0, 60.0);
            plain(QStringLiteral("background.padding"), tx("Background Padding"), uiOf(0.0, 500.0, 1.0, 0, "px"), bg.padding, 12.0);
            plain(QStringLiteral("background.radius"), tx("Corner Radius"), uiOf(0.0, 500.0, 1.0, 0, "px"), bg.radius, 0.0);
        }
        const DropShadow& sh = layer->shadow;
        toggle(QStringLiteral("shadow"), tx("Shadow"), sh.enabled);
        if (sh.enabled) {
            color(QStringLiteral("shadow.color"), tx("Shadow Color"), sh.color);
            plain(QStringLiteral("shadow.opacity"), tx("Shadow Opacity"), uiOf(0.0, 100.0, 1.0, 0, "%"), sh.opacity * 100.0, 75.0);
            plain(QStringLiteral("shadow.angle"), tx("Shadow Angle"), uiOf(-360.0, 360.0, 1.0, 0, "°", true), sh.angle, 135.0);
            plain(QStringLiteral("shadow.distance"), tx("Shadow Distance"), uiOf(0.0, 500.0, 0.5, 1, "px"), sh.distance, 6.0);
            plain(QStringLiteral("shadow.size"), tx("Shadow Size"), uiOf(0.0, 200.0, 0.5, 1, "px"), sh.size, 0.0);
            plain(QStringLiteral("shadow.softness"), tx("Shadow Blur"), uiOf(0.0, 500.0, 0.5, 1, "px"), sh.softness, 8.0);
        }

        // ---- Transform (the layer's, in the graphic's pixels) ----
        group(QStringLiteral("transformGroup"), tx("Transform"));
        number(QStringLiteral("x"), "transform.x", tx("Position X"), uiOf(-10000.0, 10000.0, 1.0, 0, "px", true), LABEL("Position"));
        number(QStringLiteral("y"), "transform.y", tx("Position Y"), uiOf(-10000.0, 10000.0, 1.0, 0, "px", true), LABEL("Position"));
        if (layer->transform.scaleX == layer->transform.scaleY) {
            number(QStringLiteral("scale"), "transform.scale", tx("Scale"), uiOf(0.0, 1000.0, 0.1, 1, "%"), LABEL("Scale"));
        } else {
            number(QStringLiteral("scaleX"), "transform.scaleX", tx("Scale Width"), uiOf(-1000.0, 1000.0, 0.1, 1, "%"), LABEL("Scale"));
            number(QStringLiteral("scaleY"), "transform.scaleY", tx("Scale Height"), uiOf(-1000.0, 1000.0, 0.1, 1, "%"), LABEL("Scale"));
        }
        number(QStringLiteral("rotation"), "transform.rotation", tx("Rotation"), uiOf(-360.0, 360.0, 0.1, 1, "°", true), LABEL("Rotation"));
        number(QStringLiteral("opacity"), "transform.opacity", tx("Opacity"), uiOf(0.0, 100.0, 0.1, 1, "%"), LABEL("Opacity"));
        number(QStringLiteral("anchorX"), "transform.anchorX", tx("Anchor X"), uiOf(-10000.0, 10000.0, 1.0, 0, "px", true), LABEL("Anchor Point"));
        number(QStringLiteral("anchorY"), "transform.anchorY", tx("Anchor Y"), uiOf(-10000.0, 10000.0, 1.0, 0, "px", true), LABEL("Anchor Point"));
    }
}

void EffectControlsModel::addTitleRows(std::vector<Row>& rows, const Clip& clip, const QString& section) {
    const auto effect = parseEffectKey(section);
    const EffectStack& stack = clip.effects();
    const auto it = std::find_if(stack.begin(), stack.end(), [&](const EffectInstance& e) { return effect && e.id == *effect; });
    const EffectDescriptor* d = descriptor(edit::kTitleStyleEffect);
    const ProjectSnapshot state = m_controller ? m_controller->snapshot() : nullptr;
    const MediaItem* media = state ? state->findMedia(clip.media) : nullptr;
    if (it == stack.end() || !d || !media || !media->generator)
        return;
    const EffectInstance& instance = *it;
    const bool enabled = instance.enabled;
    const auto add = [&](const QString& key, const QString& kind, const QString& title) -> Row& {
        Row r;
        r.key = section + QLatin1Char(':') + key;
        r.kind = kind;
        r.section = section;
        r.sectionKind = QStringLiteral("effect");
        r.title = title;
        r.clip = clip.id;
        r.textProp = key;
        r.effectEnabled = enabled;
        rows.push_back(std::move(r));
        return rows.back();
    };
    const auto param = [&](std::string_view key) {
        const EffectParamInfo* p = d->param(key);
        if (!p)
            return;
        addParamRow(rows, clip, section + QLatin1Char(':') + fromView(key), section, QStringLiteral("effect"),
                    edit::ParamRef::ofEffect(instance.id, std::string(key)), tx(p->displayName.data()), QString(), uiRange(*p),
                    p->defaultValue, p->animatable, enabled, std::string(d->displayName) + ": " + std::string(p->displayName));
        if (p->isChoice())
            makeChoice(rows.back(), *p);
    };
    // A colour from three constant parameters is one colour row; animated ones keep their own rows (their keys).
    const auto colorRow = [&](const std::string& prefix, const QString& title) {
        std::array<double, 4> rgba{0.0, 0.0, 0.0, 1.0};
        bool constant = true;
        const char* const parts[] = {".red", ".green", ".blue"};
        for (int i = 0; i < 3; ++i) {
            const ParamValue v = paramValueOf(m_controller, clip, edit::ParamRef::ofEffect(instance.id, prefix + parts[i]));
            constant = constant && v.isConstant();
            rgba[static_cast<std::size_t>(i)] = v.constant();
        }
        if (!constant) {
            for (const char* part : parts)
                param(prefix + part);
            return;
        }
        Row& r = add(QStringLiteral("color:") + QString::fromStdString(prefix), QStringLiteral("color"), title);
        r.text = hexOf(rgba);
    };
    const GeneratorSpec& g = *media->generator;
    Row& text = add(QStringLiteral("titleText"), QStringLiteral("textarea"), tx("Source Text"));
    text.text = QString::fromStdString(g.text);
    text.description = tx("Every clip of this title shows the text");
    const QString family = g.fontFamily.empty() ? defaultFamily() : QString::fromStdString(g.fontFamily);
    add(QStringLiteral("titleFont"), QStringLiteral("font"), tx("Font")).text = family;
    const QStringList styles = stylesOf(family, QString::fromStdString(g.fontStyle));
    const QString currentStyle = g.fontStyle.empty() ? styles.front() : QString::fromStdString(g.fontStyle);
    Row& styleRow = add(QStringLiteral("titleFontStyle"), QStringLiteral("choice"), tx("Font Style"));
    for (qsizetype i = 0; i < styles.size(); ++i)
        styleRow.options.append(QVariantMap{{QStringLiteral("value"), QString::number(i)}, {QStringLiteral("label"), styles[i]}});
    styleRow.range = uiOf(0.0, static_cast<double>(std::max<qsizetype>(0, styles.size() - 1)), 1.0, 0, "");
    styleRow.from = 0.0;
    styleRow.to = styleRow.range.to;
    styleRow.value = static_cast<double>(std::max<qsizetype>(0, styles.indexOf(currentStyle)));
    styleRow.text = QString::number(static_cast<int>(styleRow.value));

    param("size");
    param("alignment");
    {
        // Left / Center / Right as a choice (the parameter's whole numbers).
        Row& a = rows.back();
        if (a.ref.key == "alignment" && a.kind != QLatin1String("choice")) {
            a.kind = QStringLiteral("choice");
            a.options.clear();
            const QString labels[] = {tx("Left"), tx("Center"), tx("Right")};
            for (int i = 0; i < 3; ++i)
                a.options.append(QVariantMap{{QStringLiteral("value"), QString::number(a.range.toUi(i, a.frame))},
                                             {QStringLiteral("label"), labels[i]}});
            a.text = QString::number(std::round(a.value));
        }
    }
    param("lineSpacing");
    param("tracking");
    colorRow("fill", tx("Fill"));
    param("fill.alpha");
    colorRow("stroke", tx("Stroke"));
    param("stroke.width");
    param("stroke.alpha");
    colorRow("shadow", tx("Shadow Color"));
    for (const char* key : {"shadow.opacity", "shadow.angle", "shadow.distance", "shadow.softness"})
        param(key);
}

bool EffectControlsModel::editTextLayer(const Row& row, const char* label,
                                        const std::function<bool(GraphicLayer&, Ticks)>& change, const std::string& mergeKey) {
    const Clip* clip = clipOf(row);
    const Graphic* graphic = clip ? clip->graphic() : nullptr;
    const GraphicLayer* layer = graphic ? graphic->findLayer(row.layer) : nullptr;
    if (!layer)
        return false;
    GraphicLayer next = *layer;
    const Ticks playhead = std::clamp(m_controller->playhead(), clip->start(), std::max(clip->start(), clip->end() - Ticks{1}));
    if (!change(next, graphicTimeAt(*clip, *graphic, playhead)) || next == *layer)
        return false;
    const SequenceId seq = m_controller->currentSequenceId();
    const ClipId id = clip->id;
    const std::uint32_t layerId = row.layer;
    setNotice({});
    if (!m_controller->applyEditFn(
            label, [&](const ProjectState& state) { return edit::updateGraphicLayer(state, seq, id, layerId, next); }, mergeKey)) {
        setNotice(m_controller->lastError());
        return false;
    }
    return true;
}

void EffectControlsModel::setLayerParamAtPlayhead(const Row& row, double model, const std::string& mergeKey) {
    const Clip* clip = clipOf(row);
    if (!clip)
        return;
    const ParamValue current = paramValueOf(m_controller, *clip, row.ref);
    ParamValue next(model);
    if (const Curve* curve = current.curve()) {
        // Animated: the value goes into the key at the playhead (its graphic time), as for any parameter.
        const Ticks t = rowCurveTimeAtPlayhead(row, *clip);
        Curve keys = *curve;
        if (const auto at = keys.keyAt(t)) {
            if (keys.keys[*at].value == model)
                return;
            keys.keys[*at].value = model;
        } else {
            const std::size_t insertAt = keys.insertionIndex(t);
            Keyframe key{t, model};
            if (insertAt > 0)
                key.interpolation = keys.keys[insertAt - 1].interpolation;
            keys.keys.insert(keys.keys.begin() + static_cast<std::ptrdiff_t>(insertAt), key);
        }
        next = ParamValue(std::move(keys));
    } else if (current.constant() == model) {
        return;
    }
    const SequenceId seq = m_controller->currentSequenceId();
    const ClipId id = clip->id;
    const edit::ParamRef ref = row.ref;
    setNotice({});
    if (!m_controller->applyEditFn(
            row.undoLabel.c_str(), [&](const ProjectState& state) { return edit::setParameter(state, seq, id, ref, next); },
            mergeKey))
        setNotice(m_controller->lastError());
}

void EffectControlsModel::setTextProperty(const Row& row, double value, const std::string& mergeKey) {
    const bool on = value >= 0.5;
    const int index = static_cast<int>(std::lround(value));
    const QString& p = row.textProp;
    const auto optionLabel = [&]() -> QString {
        return index >= 0 && index < row.options.size() ? row.options[index].toMap().value(QStringLiteral("label")).toString() : QString();
    };
    if (row.layer == 0) {
        // A title clip's Font Style.
        const Clip* clip = clipOf(row);
        const QString style = optionLabel();
        if (!clip || p != QLatin1String("titleFontStyle") || style.isEmpty() || index == static_cast<int>(row.value))
            return;
        const MediaId media = clip->media;
        const std::string name = style.toStdString();
        setNotice({});
        if (!m_controller->applyEditFn(LABEL("Title Font"), [&](const ProjectState& state) {
                return edit::setTitleFont(state, media, std::nullopt, name);
            }))
            setNotice(m_controller->lastError());
        return;
    }
    // Run styles follow Source Text (the version at the playhead); the rest is the layer's.
    const auto runs = [&](const char* label, const std::function<void(TextStyle&)>& set) {
        editTextLayer(row, label, [&](GraphicLayer& l, Ticks t) {
            return edit::editSourceRunsAt(l.text, t, [&](std::vector<TextRun>& list) {
                bool changed = false;
                for (TextRun& r : list) {
                    const TextStyle before = r.style;
                    set(r.style);
                    changed = changed || !(before == r.style);
                }
                return changed;
            });
        }, mergeKey);
    };
    if (p == QLatin1String("kerning")) {
        runs(LABEL("Kerning"), [&](TextStyle& s) { s.kerning = on; });
    } else if (p == QLatin1String("caps")) {
        runs(LABEL("Capitals"), [&](TextStyle& s) { s.caps = static_cast<TextCaps>(std::clamp(index, 0, 2)); });
    } else if (p == QLatin1String("fauxBold")) {
        runs(LABEL("Faux Bold"), [&](TextStyle& s) { s.fauxBold = on; });
    } else if (p == QLatin1String("fauxItalic")) {
        runs(LABEL("Faux Italic"), [&](TextStyle& s) { s.fauxItalic = on; });
    } else if (p == QLatin1String("underline")) {
        runs(LABEL("Underline"), [&](TextStyle& s) { s.underline = on; });
    } else if (p == QLatin1String("fontStyle")) {
        const std::string style = optionLabel().toStdString();
        if (!style.empty())
            runs(LABEL("Font Style"), [&](TextStyle& s) { s.fontStyle = style; });
    } else {
        const int stroke = strokeIndexOf(p);
        editTextLayer(row, LABEL("Text Property"), [&](GraphicLayer& l, Ticks) {
            TextContent& t = l.text;
            if (p == QLatin1String("align"))
                t.align = static_cast<TextAlign>(std::clamp(index, 0, 3));
            else if (p == QLatin1String("valign"))
                t.verticalAlign = static_cast<TextVerticalAlign>(std::clamp(index, 0, 2));
            else if (p == QLatin1String("background"))
                t.background.enabled = on;
            else if (p == QLatin1String("background.opacity"))
                t.background.color.rgba[3] = std::clamp(value / 100.0, 0.0, 1.0);
            else if (p == QLatin1String("background.padding"))
                t.background.padding = std::clamp(value, 0.0, 10000.0);
            else if (p == QLatin1String("background.radius"))
                t.background.radius = std::clamp(value, 0.0, 10000.0);
            else if (p == QLatin1String("shadow"))
                l.shadow.enabled = on;
            else if (p == QLatin1String("shadow.opacity"))
                l.shadow.opacity = std::clamp(value / 100.0, 0.0, 1.0);
            else if (p == QLatin1String("shadow.angle"))
                l.shadow.angle = value;
            else if (p == QLatin1String("shadow.distance"))
                l.shadow.distance = std::clamp(value, 0.0, 10000.0);
            else if (p == QLatin1String("shadow.size"))
                l.shadow.size = std::clamp(value, 0.0, 1000.0);
            else if (p == QLatin1String("shadow.softness"))
                l.shadow.softness = std::clamp(value, 0.0, 10000.0);
            else if (stroke >= 0 && static_cast<std::size_t>(stroke) < t.strokes.size() && p.endsWith(QLatin1String(".position"))) {
                t.strokes[static_cast<std::size_t>(stroke)].position = static_cast<StrokePosition>(std::clamp(index, 0, 2));
                t.strokes[static_cast<std::size_t>(stroke)].enabled = true;
            } else
                return false;
            return true;
        }, mergeKey);
    }
}

void EffectControlsModel::setColor(const QString& rowKey, const QString& color) {
    const Row* r = findRow(rowKey);
    const QColor c = QColor::fromString(color.trimmed());
    if (!r || !m_controller || r->kind != QLatin1String("color") || !c.isValid())
        return;
    const std::array<double, 3> rgb{c.redF(), c.greenF(), c.blueF()};
    const std::string mergeKey = "color:" + std::to_string(r->clip.value) + ":" + rowKey.toStdString();
    const QString& p = r->textProp;
    if (r->layer == 0) {
        // A title: the style effect's red, green and blue as one step.
        const auto effect = parseEffectKey(r->section);
        const Clip* clip = clipOf(*r);
        if (!effect || !clip || !p.startsWith(QLatin1String("color:")))
            return;
        const std::string prefix = p.mid(6).toStdString();
        const SequenceId seq = m_controller->currentSequenceId();
        const ClipId id = clip->id;
        const EffectInstanceId instance = *effect;
        std::vector<std::function<edit::EditOutcome(const ProjectState&)>> steps;
        const char* const parts[] = {".red", ".green", ".blue"};
        for (int i = 0; i < 3; ++i) {
            const edit::ParamRef ref = edit::ParamRef::ofEffect(instance, prefix + parts[i]);
            const double v = rgb[static_cast<std::size_t>(i)];
            steps.push_back([=](const ProjectState& state) { return edit::setParameter(state, seq, id, ref, ParamValue(v)); });
        }
        setNotice({});
        if (!m_controller->applyEditFn(LABEL("Title Color"), [&](const ProjectState& state) { return chain(state, steps); }, mergeKey))
            setNotice(m_controller->lastError());
        return;
    }
    const auto paint = [&](GraphicColor& g) {
        const GraphicColor before = g;
        g.rgba = {rgb[0], rgb[1], rgb[2], g.rgba[3]};
        g.brand.clear();
        return !(before == g);
    };
    const int stroke = strokeIndexOf(p);
    editTextLayer(*r, LABEL("Text Color"), [&](GraphicLayer& l, Ticks) {
        if (p == QLatin1String("fill")) {
            bool changed = false;
            edit::editAllRuns(l.text, [&](TextRun& run) { changed = paint(run.style.fill) || changed; });
            return changed;
        }
        if (p == QLatin1String("background.color"))
            return paint(l.text.background.color);
        if (p == QLatin1String("shadow.color"))
            return paint(l.shadow.color);
        if (stroke >= 0 && static_cast<std::size_t>(stroke) < l.text.strokes.size()) {
            TextStroke& st = l.text.strokes[static_cast<std::size_t>(stroke)];
            const bool enabling = !st.enabled;
            st.enabled = true;
            return paint(st.color) || enabling;
        }
        return false;
    }, mergeKey);
}

QStringList EffectControlsModel::fontFamilies() const {
    static const QStringList families = QFontDatabase::families();
    return families;
}

void EffectControlsModel::rowAction(const QString& rowKey, const QString& action) {
    const Row* r = findRow(rowKey);
    if (!r || !m_controller)
        return;
    // 0.6 (card #71): Master Clip Effects.
    if (action == QLatin1String("masterOff") || action == QLatin1String("masterOn")) {
        const SequenceId seq = m_controller->currentSequenceId();
        const ClipId clip = r->clip;
        const bool off = action == QLatin1String("masterOff");
        m_controller->applyEditFn(off ? LABEL("Turn Off Master Clip Effects") : LABEL("Turn On Master Clip Effects"),
                                  [&](const ProjectState& state) {
                                      return edit::setMasterEffectsOff(state, seq, std::span<const ClipId>(&clip, 1), off);
                                  });
        return;
    }
    if (action == QLatin1String("toMaster") || action == QLatin1String("toClip")) {
        const auto effect = parseEffectKey(r->section);
        if (!effect || !r->clip)
            return;
        const SequenceId seq = m_controller->currentSequenceId();
        const ClipId clip = r->clip;
        const bool toMaster = action == QLatin1String("toMaster");
        const bool ok = m_controller->applyEditFn(toMaster ? LABEL("Move to Master Clip") : LABEL("Move to Clip"),
                                                  [&](const ProjectState& state) {
                                                      return toMaster ? edit::moveEffectToMaster(state, seq, clip, *effect)
                                                                      : edit::moveEffectToClip(state, seq, clip, *effect);
                                                  });
        if (!ok)
            setNotice(m_controller->lastError());
        return;
    }
    if (r->layer == 0)
        return;
    if (action == QLatin1String("addStroke")) {
        editTextLayer(*r, LABEL("Add Stroke"), [](GraphicLayer& l, Ticks) {
            if (l.text.strokes.size() >= kMaxTextStrokes)
                return false;
            TextStroke st;
            if (!l.text.strokes.empty()) {
                // A next stroke frames the ones before it: wider, in white.
                st.width = ParamValue(l.text.strokes.back().width.constant() + 4.0);
                st.color = GraphicColor{{1.0, 1.0, 1.0, 1.0}, {}};
            }
            l.text.strokes.push_back(std::move(st));
            return true;
        });
    } else if (action == QLatin1String("remove")) {
        const int stroke = strokeIndexOf(r->textProp);
        if (stroke < 0)
            return;
        editTextLayer(*r, LABEL("Remove Stroke"), [&](GraphicLayer& l, Ticks) {
            if (static_cast<std::size_t>(stroke) >= l.text.strokes.size())
                return false;
            l.text.strokes.erase(l.text.strokes.begin() + stroke);
            return true;
        });
    }
}

// ---- Multi-clip editing (0.6) -----------------------------------------------------------------------

void EffectControlsModel::addMultiRows(std::vector<Row>& rows) {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return;
    std::vector<const Clip*> video, audio;
    for (const std::vector<ClipId>& unit : m_multiUnits) {
        for (const ClipId id : unit) {
            if (const auto loc = seq->findClip(id))
                (loc->clip().stream.kind == StreamKind::Video ? video : audio).push_back(&loc->clip());
        }
    }
    const auto section = [&](const QString& key, const QString& title, const QString& icon, PropertyRole role,
                             const std::vector<const Clip*>& of) {
        if (of.empty())
            return;
        Row h;
        h.key = QStringLiteral("h:") + key;
        h.kind = QStringLiteral("header");
        h.section = key;
        h.sectionKind = QStringLiteral("multi");
        h.title = title;
        h.icon = icon;
        h.collapsed = m_collapsed.contains(key);
        h.clip = of.front()->id;
        const bool collapsed = h.collapsed;
        rows.push_back(std::move(h));
        if (collapsed)
            return;
        for (const UiParameter& p : uiParameters()) {
            const ParameterInfo& info = parameterInfo(p.id);
            if (info.role != role)
                continue;
            addParamRow(rows, *of.front(), QStringLiteral("m:") + QLatin1String(p.key), key, QStringLiteral("multi"),
                        edit::ParamRef::of(p.id), QCoreApplication::translate("wabcut::app::ParameterUi", p.label),
                        QLatin1String(p.icon), p.range, info.defaultValue, false, true, p.undoLabel);
            Row& r = rows.back();
            // No keys or lane here; the value is the first clip's, "mixed" when another clip's differs.
            r.animated = false;
            r.keyAtPlayhead = r.hasPrev = r.hasNext = false;
            r.keys.clear();
            r.graphOpen = false;
            r.graph.clear();
            const double tolerance = std::pow(10.0, -r.decimals) / 2.0;
            for (const Clip* c : of) {
                const double v = r.range.toUi(paramValueOf(m_controller, *c, r.ref).valueAt(m_controller->curveTimeAtPlayhead(*c)),
                                              m_controller->propertyFrame(*c));
                if (std::abs(v - r.value) > tolerance)
                    r.mixed = true;
            }
        }
    };
    section(QStringLiteral("multi-motion"), tx("Transform"), QStringLiteral("scaling"), PropertyRole::Motion, video);
    section(QStringLiteral("multi-opacity"), tx("Opacity"), QStringLiteral("blend"), PropertyRole::Opacity, video);
    section(QStringLiteral("multi-audio"), tx("Audio"), QStringLiteral("volume-2"), PropertyRole::Audio, audio);
}

void EffectControlsModel::setMultiValue(const Row& row, double uiValue) {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq || !row.ref.intrinsic)
        return;
    const PropertyRole role = parameterInfo(*row.ref.intrinsic).role;
    const StreamKind kind = role == PropertyRole::Audio ? StreamKind::Audio : StreamKind::Video;
    std::vector<std::function<edit::EditOutcome(const ProjectState&)>> steps;
    int set = 0, otherKind = 0, locked = 0;
    const SequenceId seqId = seq->id;
    const edit::ParamRef ref = row.ref;
    for (const std::vector<ClipId>& unit : m_multiUnits) {
        bool any = false;
        for (const ClipId id : unit) {
            const auto loc = seq->findClip(id);
            if (!loc || loc->clip().stream.kind != kind)
                continue;
            any = true;
            if (loc->track->locked) {
                ++locked;
                continue;
            }
            const Clip& c = loc->clip();
            const double model = row.range.toModel(uiValue, m_controller->propertyFrame(c));
            const ParamValue current = paramValueOf(m_controller, c, ref);
            ParamValue next(model);
            if (const Curve* curve = current.curve()) {
                // Animated on this clip: a key at the playhead (as Effect Controls does for one clip).
                const Ticks t = m_controller->curveTimeAtPlayhead(c);
                Curve keys = *curve;
                if (const auto at = keys.keyAt(t)) {
                    keys.keys[*at].value = model;
                } else {
                    const std::size_t insertAt = keys.insertionIndex(t);
                    Keyframe key{t, model};
                    if (insertAt > 0)
                        key.interpolation = keys.keys[insertAt - 1].interpolation;
                    keys.keys.insert(keys.keys.begin() + static_cast<std::ptrdiff_t>(insertAt), key);
                }
                next = ParamValue(std::move(keys));
            }
            ++set;
            if (next == current)
                continue;
            steps.push_back([=](const ProjectState& state) { return edit::setParameter(state, seqId, id, ref, next); });
        }
        if (!any)
            ++otherKind;
    }
    QString notice = set == 1 ? tx("%1 set on 1 clip").arg(row.title) : tx("%1 set on %2 clips").arg(row.title).arg(set);
    QStringList skipped;
    if (otherKind > 0)
        skipped << (kind == StreamKind::Video ? tx("%1 audio skipped").arg(otherKind) : tx("%1 video skipped").arg(otherKind));
    if (locked > 0)
        skipped << tx("%1 on locked tracks skipped").arg(locked);
    if (!skipped.isEmpty())
        notice += QStringLiteral(" (") + skipped.join(QStringLiteral(", ")) + QLatin1Char(')');
    if (!steps.empty()
        && !m_controller->applyEditFn(row.undoLabel.c_str(), [&](const ProjectState& state) { return chain(state, steps); },
                                      "multi:" + row.key.toStdString())) {
        setNotice(m_controller->lastError());
        return;
    }
    setNotice(notice);
}

// ---- Keyframe clipboard (0.6) -----------------------------------------------------------------------

/// What Copy put on the keyframe clipboard: per parameter, its keys with their times after the earliest copied key
/// (timeline ticks, so keys of graphic layers and of clips paste alike).
struct EffectControlsModel::KeyClipboard {
    struct Entry {
        edit::ParamRef ref;
        std::string effect; ///< an effect parameter: the effect's id and which of that effect's instances it was
        int effectIndex = 0;
        QString title;
        std::vector<Keyframe> keys;
        /// Fix F1A: Source Text keys (text.source) hold an index into their layer's text versions; each key's text goes
        /// with it (texts[i] for keys[i]), so it pastes as that text on any layer.
        std::vector<std::vector<TextRun>> texts;
    };
    std::vector<Entry> entries;
    int count = 0;
};

namespace {

constexpr std::string_view kSourceTextKey = "text.source";

/// Fix F1A (card #71): the effects Effect Controls lists for a clip - its media's Master Clip Effects (a subclip's master
/// item's first), then its own - where the keyframe clipboard counts "the n-th instance of an effect".
EffectStack listedEffects(const ProjectController* controller, const Clip& clip) {
    const ProjectSnapshot snapshot = controller ? controller->snapshot() : nullptr;
    EffectStack out = snapshot && clip.media.isValid() ? masterEffectsOf(*snapshot, clip.media) : EffectStack{};
    out.insert(out.end(), clip.effects().begin(), clip.effects().end());
    return out;
}

} // namespace

void EffectControlsModel::setKeysPending(bool pending) {
    const bool before = canPasteKeys();
    m_keysPending = pending && m_keyClipboard != nullptr;
    if (canPasteKeys() != before)
        emit keyClipboardChanged();
}

int EffectControlsModel::copySelectedKeys(bool cut) {
    if (m_selectedKeys.empty() || !m_controller)
        return 0;
    auto board = std::make_shared<KeyClipboard>();
    std::map<QString, std::size_t> byRow;
    std::optional<Ticks> earliest;
    for (const KeyRef& k : m_selectedKeys) {
        const Row* r = findRow(k.row);
        const Clip* clip = r ? clipOf(*r) : nullptr;
        if (!clip)
            continue;
        const ParamValue value = paramValueOf(m_controller, *clip, r->ref);
        const Curve* curve = value.curve();
        const auto at = curve ? curve->keyAt(Ticks{k.curveTime}) : std::nullopt;
        const auto timeline = at ? rowTimelineTime(*r, *clip, Ticks{k.curveTime}) : std::nullopt;
        if (!timeline)
            continue;
        auto found = byRow.find(k.row);
        if (found == byRow.end()) {
            KeyClipboard::Entry e;
            e.ref = r->ref;
            e.title = r->title;
            if (!r->ref.intrinsic && r->ref.layer == 0) {
                // Which instance of its effect it is (the first Gaussian Blur, the second ...), Master Clip Effects first.
                const EffectStack stack = listedEffects(m_controller, *clip);
                const auto self = std::find_if(stack.begin(), stack.end(),
                                               [&](const EffectInstance& instance) { return instance.id == r->ref.effect; });
                if (self != stack.end()) {
                    e.effect = self->effect;
                    e.effectIndex = static_cast<int>(std::count_if(stack.begin(), self, [&](const EffectInstance& instance) {
                        return instance.effect == e.effect;
                    }));
                }
            }
            board->entries.push_back(std::move(e));
            found = byRow.emplace(k.row, board->entries.size() - 1).first;
        }
        Keyframe key = curve->keys[*at];
        key.time = *timeline;
        board->entries[found->second].keys.push_back(key);
        if (r->ref.layer != 0 && r->ref.key == kSourceTextKey) {
            // Fix F1A: the text this Source Text key shows.
            const GraphicLayer* layer = clip->graphic() ? clip->graphic()->findLayer(r->ref.layer) : nullptr;
            board->entries[found->second].texts.push_back(layer ? sourceRunsAt(layer->text, Ticks{k.curveTime})
                                                                : std::vector<TextRun>{});
        }
        earliest = earliest ? std::min(*earliest, *timeline) : *timeline;
        ++board->count;
    }
    if (board->count == 0)
        return 0;
    for (KeyClipboard::Entry& e : board->entries) {
        for (Keyframe& key : e.keys)
            key.time = key.time - *earliest;
        // In time order; a Source Text key's text stays with it.
        std::vector<std::size_t> order(e.keys.size());
        for (std::size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return e.keys[a].time < e.keys[b].time; });
        std::vector<Keyframe> keys;
        std::vector<std::vector<TextRun>> texts;
        for (const std::size_t i : order) {
            keys.push_back(e.keys[i]);
            if (i < e.texts.size())
                texts.push_back(std::move(e.texts[i]));
        }
        e.keys = std::move(keys);
        e.texts = texts.size() == e.keys.size() ? std::move(texts) : std::vector<std::vector<TextRun>>{};
    }
    const int count = board->count;
    if (cut)
        deleteKeys(LABEL("Cut Keyframes"));
    m_keyClipboard = std::move(board);
    m_keysPending = true;
    emit keyClipboardChanged(); // a new copy (even when one was already pending)
    setEffectsPending(false);   // the newest copy wins
    emit m_controller->clipboardNotice(count == 1 ? tx("Copied 1 keyframe") : tx("Copied %1 keyframes").arg(count));
    return count;
}

QString EffectControlsModel::pasteKeys() {
    if (!m_controller)
        return tx("No project");
    const auto fail = [&](const QString& text) {
        setNotice(text);
        return text;
    };
    if (!m_keyClipboard)
        return fail(tx("Copy keyframes in Effect Controls first"));
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq || m_mode != QLatin1String("clip"))
        return fail(tx("Select a clip to paste the keyframes onto"));
    const Ticks playhead = m_controller->playhead();
    const SequenceId seqId = seq->id;
    std::vector<std::function<edit::EditOutcome(const ProjectState&)>> steps;
    int pasted = 0;
    QStringList missing;
    for (const KeyClipboard::Entry& e : m_keyClipboard->entries) {
        // The same parameter on the clip shown (its video or audio part).
        const Clip* target = nullptr;
        edit::ParamRef ref = e.ref;
        for (const StreamKind kind : {StreamKind::Video, StreamKind::Audio}) {
            const auto id = m_controller->propertyTarget(kind);
            const auto loc = id ? seq->findClip(*id) : std::nullopt;
            if (!loc || target)
                continue;
            const Clip& c = loc->clip();
            if (e.ref.layer != 0) {
                const GraphicLayer* layer = c.graphic() ? c.graphic()->findLayer(e.ref.layer) : nullptr;
                if (layer && edit::layerParamInfo(*layer, e.ref.key))
                    target = &c;
            } else if (e.ref.intrinsic) {
                const PropertyRole role = parameterInfo(*e.ref.intrinsic).role;
                if ((role == PropertyRole::Audio) == (kind == StreamKind::Audio))
                    target = &c;
            } else {
                int nth = 0;
                for (const EffectInstance& instance : listedEffects(m_controller, c)) { // fix F1A: master ones first
                    if (instance.effect != e.effect)
                        continue;
                    if (nth++ == e.effectIndex) {
                        ref = edit::ParamRef::ofEffect(instance.id, e.ref.key);
                        target = &c;
                        break;
                    }
                }
            }
        }
        if (!target) {
            missing << e.title;
            continue;
        }
        Row probe;
        probe.ref = ref;
        if (ref.layer != 0 && ref.key == kSourceTextKey && e.texts.size() == e.keys.size()) {
            // Fix F1A: Source Text keys paste as the text they showed (versions of the target layer), never as bare
            // indices into the target's own versions.
            std::vector<Keyframe> keys;
            for (Keyframe key : e.keys) {
                key.time = rowCurveTime(probe, *target, playhead + key.time);
                keys.push_back(key);
            }
            pasted += static_cast<int>(keys.size());
            const ClipId clip = target->id;
            const std::uint32_t layerId = ref.layer;
            steps.push_back([=, texts = e.texts](const ProjectState& state) -> edit::EditOutcome {
                const Sequence* sequence = state.findSequence(seqId);
                const auto loc = sequence ? sequence->findClip(clip) : std::nullopt;
                const Graphic* graphic = loc ? loc->clip().graphic() : nullptr;
                const GraphicLayer* layer = graphic ? graphic->findLayer(layerId) : nullptr;
                if (!layer)
                    return std::unexpected(Error{ErrorCode::NotFound, "the text layer is not on this clip"});
                GraphicLayer changed = *layer;
                if (!edit::pasteSourceTextKeys(changed.text, keys, texts))
                    return std::unexpected(Error{ErrorCode::InvalidArgument, "the text layer has too many Source Text versions"});
                return edit::updateGraphicLayer(state, seqId, clip, layerId, std::move(changed));
            });
            continue;
        }
        const ParamValue current = paramValueOf(m_controller, *target, ref);
        Curve curve = current.curve() ? *current.curve() : Curve{};
        for (Keyframe key : e.keys) {
            key.time = rowCurveTime(probe, *target, playhead + key.time);
            if (const auto at = curve.keyAt(key.time))
                curve.keys[*at] = key; // a key at the same time is replaced
            else
                curve.keys.insert(curve.keys.begin() + static_cast<std::ptrdiff_t>(curve.insertionIndex(key.time)), key);
            ++pasted;
        }
        const ClipId clip = target->id;
        steps.push_back([=](const ProjectState& state) { return edit::setParameter(state, seqId, clip, ref, ParamValue(curve)); });
    }
    if (steps.empty())
        return fail(tx("This clip has none of the copied parameters (%1)").arg(missing.join(QStringLiteral(", "))));
    if (!m_controller->applyEditFn(LABEL("Paste Keyframes"), [&](const ProjectState& state) { return chain(state, steps); }))
        return fail(m_controller->lastError());
    clearKeySelection();
    QString message = pasted == 1 ? tx("Pasted 1 keyframe") : tx("Pasted %1 keyframes").arg(pasted);
    if (!missing.isEmpty())
        message += QStringLiteral(" · ") + tx("not on this clip: %1").arg(missing.join(QStringLiteral(", ")));
    setNotice(missing.isEmpty() ? QString() : message);
    emit m_controller->clipboardNotice(message);
    return {};
}

} // namespace wabcut::app
