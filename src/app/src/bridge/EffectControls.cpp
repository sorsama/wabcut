#include "EffectControls.h"

#include "CommandPalette.h"
#include "EditingBridge.h"
#include "EffectPresetsBridge.h"
#include "ProjectController.h"

#include <wabcut/model/Animations.h>
#include <wabcut/model/Clipboard.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/TitleOps.h>

#include <QCoreApplication>
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
        && sectionSelected == o.sectionSelected;
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
    connect(controller, &ProjectController::clipboardChanged, this, [this] { setEffectsPending(false); });
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

std::vector<std::pair<std::size_t, Ticks>> EffectControlsModel::visibleKeys(const Row&, const Curve& curve,
                                                                             const Clip& clip) const {
    std::vector<std::pair<std::size_t, Ticks>> out;
    for (std::size_t i = 0; i < curve.keys.size(); ++i) {
        if (const auto t = keyTimelineTime(clip, curve.keys[i].time))
            out.emplace_back(i, *t);
    }
    return out;
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
    const ParamValue value = ProjectController::parameterValue(clip, ref);
    const Ticks curveTime = m_controller->curveTimeAtPlayhead(clip);
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
        const auto t = clip.timelineTimeOfCurveTime(curveTime);
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
            out.emplace_back(x, ui(curve.valueAt(clip.curveTimeAt(t))));
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
           (r.kind == QLatin1String("choice") && !r.ref.key.empty());
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
        header(QStringLiteral("motion"), QStringLiteral("intrinsic"), tx("Transform"), QStringLiteral("scaling"));
        intrinsic(QStringLiteral("motion"), PropertyRole::Motion);
        header(QStringLiteral("opacity"), QStringLiteral("intrinsic"), tx("Opacity"), QStringLiteral("blend"));
        intrinsic(QStringLiteral("opacity"), PropertyRole::Opacity);
    } else {
        header(QStringLiteral("audio"), QStringLiteral("intrinsic"), tx("Audio"), QStringLiteral("volume-2"));
        intrinsic(QStringLiteral("audio"), PropertyRole::Audio);
    }

    const EffectStack& stack = clip.effects();
    for (std::size_t i = 0; i < stack.size(); ++i) {
        const EffectInstance& instance = stack[i];
        const EffectDescriptor* d = descriptor(instance.effect);
        const QString section = effectKey(instance.id);
        const EffectKind kind = video ? EffectKind::Video : EffectKind::Audio;
        Row& h = header(section, QStringLiteral("effect"),
                        d ? tx(d->displayName.data()) : QString::fromStdString(instance.effect),
                        d ? categoryIcon(d->category, d->kind) : QStringLiteral("sparkles"));
        h.effectEnabled = instance.enabled;
        h.removable = true;
        h.movable = true;
        h.resettable = d != nullptr;
        h.stackIndex = static_cast<int>(i);
        h.stackKind = video ? QStringLiteral("video") : QStringLiteral("audio");
        h.known = isEvaluable(instance, kind);
        h.description = d ? fromView(d->description) : tx("This effect is not available in this version; it is kept as it is.");
        if (!d || m_collapsed.contains(section))
            continue;
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
    if (mode != m_mode || title != m_title || subtitle != m_subtitle || trackName != m_trackName
        || colorIndex != m_colorIndex) {
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
    bool relevant = change.structural || !change.changes || (s && s->settingsChanged);
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
    if (!r->clip)
        return;
    const std::string mergeKey = "fx:" + std::to_string(r->clip.value) + ":" + rowKey.toStdString();
    const std::string label = r->undoLabel;
    m_controller->setParameterAtPlayhead(r->clip, r->ref, model, label.c_str(), mergeKey);
}

void EffectControlsModel::endEdit() {
    if (m_controller)
        m_controller->endPropertyGesture();
}

void EffectControlsModel::setEffectText(const QString& rowKey, const QString& text) {
    const Row* r = findRow(rowKey);
    if (!r || !m_controller || r->kind != QLatin1String("text") || !r->clip || text == r->text)
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
    const Ticks at = m_controller->curveTimeAtPlayhead(*clip);
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
    const Ticks at = m_controller->curveTimeAtPlayhead(*clip);
    const ParamValue value = ProjectController::parameterValue(*clip, ref);
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
    const ParamValue value = ProjectController::parameterValue(*clip, r->ref);
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
        const EffectStack& stack = loc->clip().effects();
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
        if (r.kind == QLatin1String("header") && r.sectionKind != QLatin1String("transition") && r.clip.isValid())
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
        if (!snapshot)
            snapshot = clip;
        if (section == QLatin1String("motion")) {
            attributes.motion = true;
        } else if (section == QLatin1String("opacity")) {
            attributes.opacity = true;
        } else if (section == QLatin1String("audio")) {
            attributes.volume = true;
            attributes.panner = true;
        } else if (h->stackIndex >= 0) {
            attributes.effects.push_back(static_cast<std::size_t>(h->stackIndex));
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
        if (!sections)
            return false;
        copySelectedSections(commandId == QLatin1String("edit.cut"));
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
    if (id.startsWith(QLatin1String("effect:")))
        id = id.mid(7);

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
        const ParamValue value = ProjectController::parameterValue(*clip, r->ref);
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
        const ParamValue value = ProjectController::parameterValue(*clip, r->ref);
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
        const ParamValue value = ProjectController::parameterValue(*clip, r->ref);
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
        const ParamValue value = ProjectController::parameterValue(*clip, r->ref);
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
        if (!loc)
            return;
        const Clip& clip = loc->clip();
        for (Ticks t : g.moving) {
            const auto tl = clip.timelineTimeOfCurveTime(t);
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
            const auto tl = clip.timelineTimeOfCurveTime(k.time);
            if (!tl)
                continue;
            k.time = clip.curveTimeAt(*tl + delta);
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
            const ParamValue v = ProjectController::parameterValue(*clip, r->ref);
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
        const ParamValue v = ProjectController::parameterValue(*clip, r->ref);
        const Curve* curve = v.curve();
        if (!curve)
            continue;
        Curve rest = *curve;
        std::erase_if(rest.keys, [&](const Keyframe& k) { return std::find(times.begin(), times.end(), k.time) != times.end(); });
        // No key left: the parameter keeps the value it has at the playhead (Premiere).
        const ParamValue next = rest.keys.empty() ? ParamValue(v.valueAt(m_controller->curveTimeAtPlayhead(*clip)))
                                                  : ParamValue(rest);
        const ClipId id = clip->id;
        const edit::ParamRef ref = r->ref;
        steps.push_back([=](const ProjectState& state) { return edit::setParameter(state, seq, id, ref, next); });
    }
    m_selectedKeys.clear();
    if (!steps.empty())
        m_controller->applyEditFn(LABEL("Delete Keyframes"), [&](const ProjectState& state) { return chain(state, steps); });
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
    const ParamValue value = ProjectController::parameterValue(*clip, r->ref);
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
    const Ticks curveTime = clip->curveTimeAt(std::clamp(t, clip->start(), clip->end()));
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

} // namespace wabcut::app
