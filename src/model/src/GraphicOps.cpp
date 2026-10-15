// Editing motion graphics (C.13): GraphicOps.h.

#include <wabcut/model/GraphicOps.h>

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/GraphicResolve.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/TitleOps.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace wabcut::edit {

namespace {

constexpr std::size_t kMaxLoopCycles = 256;

// ---- Locating a graphic clip ------------------------------------------------------------------------------------------

struct Located {
    const Sequence* sequence = nullptr;
    TrackId track;
    std::size_t index = 0;
    const Clip* clip = nullptr;
};

std::expected<Located, Error> locate(const ProjectState& state, SequenceId sequenceId, ClipId clipId) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto found = (*seq)->findClip(clipId);
    if (!found) {
        return std::unexpected(detail::notFound("clip " + std::to_string(clipId.value) + " is not in sequence '" +
                                                (*seq)->name + "'"));
    }
    const Clip& clip = found->clip();
    if (!clip.graphic()) {
        return std::unexpected(detail::invalidArgument("clip '" + clip.name() + "' is not a graphic"));
    }
    if (found->track->locked) {
        return std::unexpected(detail::invalidArgument("track " + detail::trackLabel(**seq, found->track->id) + " is locked"));
    }
    return Located{*seq, found->track->id, found->index, &clip};
}

/// `state` with the clip's graphic replaced (checked first).
EditOutcome replaceGraphic(const ProjectState& state, const Located& at, Graphic graphic) {
    if (const auto problem = graphicProblem(graphic)) {
        return std::unexpected(detail::invalidArgument("the graphic " + *problem));
    }
    if (graphic == *at.clip->graphic()) {
        return detail::unchanged(state);
    }
    detail::SequenceEditor editor(state, *at.sequence);
    editor.mutableClip(at.track, at.index).setGraphic(std::make_shared<const Graphic>(std::move(graphic)));
    return editor.finish();
}

// ---- Layer ids --------------------------------------------------------------------------------------------------------

void collectIds(const std::vector<GraphicLayer>& layers, std::set<std::uint32_t>& ids) {
    for (const GraphicLayer& l : layers) {
        ids.insert(l.id);
        collectIds(l.children, ids);
    }
}

/// Gives every layer of the subtree whose id is 0 or taken a fresh one (above `next`), keeping pins inside the subtree
/// pointing at the same layers.
void renumber(std::vector<GraphicLayer>& layers, std::set<std::uint32_t>& taken, std::uint32_t& next,
              std::map<std::uint32_t, std::uint32_t>& renamed) {
    for (GraphicLayer& l : layers) {
        if (l.id == 0 || taken.contains(l.id)) {
            const std::uint32_t old = l.id;
            while (taken.contains(next) || next == 0) {
                ++next;
            }
            l.id = next++;
            if (old != 0) {
                renamed[old] = l.id;
            }
        }
        taken.insert(l.id);
        renumber(l.children, taken, next, renamed);
    }
}

void remapPins(std::vector<GraphicLayer>& layers, const std::map<std::uint32_t, std::uint32_t>& renamed) {
    for (GraphicLayer& l : layers) {
        if (l.pin) {
            if (const auto it = renamed.find(l.pin->target); it != renamed.end()) {
                l.pin->target = it->second;
            }
        }
        remapPins(l.children, renamed);
    }
}

void giveIds(Graphic& g) {
    // Ids already unique and non-zero stay; zeros and repeats get fresh ones. Pins keep pointing at the first layer
    // with an id (a repeat's new id is not remapped: which of the two a pin meant is unknowable).
    std::set<std::uint32_t> all;
    collectIds(g.layers, all);
    std::set<std::uint32_t> taken;
    std::uint32_t next = all.empty() ? 1 : *all.rbegin() + 1;
    std::map<std::uint32_t, std::uint32_t> renamed;
    renumber(g.layers, taken, next, renamed);
}

std::vector<GraphicLayer>* childrenOf(Graphic& g, std::uint32_t parent) {
    if (parent == 0) {
        return &g.layers;
    }
    GraphicLayer* p = g.findLayer(parent);
    return p && p->kind == LayerKind::Group ? &p->children : nullptr;
}

/// Removes layer `id` from wherever it is; the removed layer, or nullopt.
std::optional<GraphicLayer> extract(std::vector<GraphicLayer>& layers, std::uint32_t id) {
    for (auto it = layers.begin(); it != layers.end(); ++it) {
        if (it->id == id) {
            GraphicLayer removed = std::move(*it);
            layers.erase(it);
            return removed;
        }
        if (auto found = extract(it->children, id)) {
            return found;
        }
    }
    return std::nullopt;
}

void dropPinsTo(std::vector<GraphicLayer>& layers, const std::set<std::uint32_t>& gone) {
    for (GraphicLayer& l : layers) {
        if (l.pin && gone.contains(l.pin->target)) {
            l.pin.reset();
        }
        dropPinsTo(l.children, gone);
    }
}

// ---- Presets ----------------------------------------------------------------------------------------------------------

template <class E, std::size_t N>
std::optional<E> parseName(const std::array<std::pair<E, std::string_view>, N>& names, std::string_view text) {
    for (const auto& [v, n] : names) {
        if (n == text) {
            return v;
        }
    }
    return std::nullopt;
}

template <class E, std::size_t N>
std::string_view nameOf(const std::array<std::pair<E, std::string_view>, N>& names, E value) {
    for (const auto& [v, n] : names) {
        if (v == value) {
            return n;
        }
    }
    return names[0].second;
}

constexpr std::array<std::pair<PresetKind, std::string_view>, 7> kPresetKinds{
    {{PresetKind::Fade, "fade"},
     {PresetKind::Slide, "slide"},
     {PresetKind::ScalePop, "scalePop"},
     {PresetKind::Typewriter, "typewriter"},
     {PresetKind::WordByWord, "wordByWord"},
     {PresetKind::Wipe, "wipe"},
     {PresetKind::Bounce, "bounce"}}};
constexpr std::array<std::pair<PresetPhase, std::string_view>, 3> kPresetPhases{
    {{PresetPhase::In, "in"}, {PresetPhase::Out, "out"}, {PresetPhase::Loop, "loop"}}};
constexpr std::array<std::pair<PresetEasing, std::string_view>, 5> kPresetEasings{
    {{PresetEasing::Linear, "linear"},
     {PresetEasing::EaseIn, "easeIn"},
     {PresetEasing::EaseOut, "easeOut"},
     {PresetEasing::EaseInOut, "easeInOut"},
     {PresetEasing::Overshoot, "overshoot"}}};
constexpr std::array<std::pair<PresetDirection, std::string_view>, 4> kPresetDirections{
    {{PresetDirection::Left, "left"}, {PresetDirection::Right, "right"}, {PresetDirection::Up, "up"}, {PresetDirection::Down, "down"}}};

Keyframe key(Ticks t, double v, Interpolation i = Interpolation::Linear) {
    Keyframe k;
    k.time = t;
    k.value = v;
    k.interpolation = i;
    return k;
}

Ticks lerpTicks(Ticks a, Ticks b, double f) {
    return Ticks{a.value + static_cast<std::int64_t>(std::llround(static_cast<double>(b.value - a.value) * f))};
}

/// A move from `from` to `to` over [a, b] with the preset's easing (Interpolation's sides: a key's leaving side and the
/// next key's arriving side shape the segment).
std::vector<Keyframe> motion(Ticks a, double from, Ticks b, double to, PresetEasing easing, double overshoot) {
    switch (easing) {
    case PresetEasing::Linear:
        return {key(a, from), key(b, to)};
    case PresetEasing::EaseIn: // accelerating: a flat departure
        return {key(a, from, Interpolation::EaseOut), key(b, to)};
    case PresetEasing::EaseOut: // decelerating: a flat arrival
        return {key(a, from), key(b, to, Interpolation::EaseIn)};
    case PresetEasing::EaseInOut:
        return {key(a, from, Interpolation::EaseInOut), key(b, to, Interpolation::EaseInOut)};
    case PresetEasing::Overshoot:
        return {key(a, from), key(lerpTicks(a, b, 0.7), to + (to - from) * overshoot / 100.0, Interpolation::EaseInOut),
                key(b, to, Interpolation::EaseIn)};
    }
    return {key(a, from), key(b, to)};
}

/// Writes `keys` into `param`, replacing its keys inside [from, to]. A constant becomes a curve.
void setKeys(ParamValue& param, Ticks from, Ticks to, std::vector<Keyframe> keys) {
    Curve curve;
    if (const Curve* existing = param.curve()) {
        for (const Keyframe& k : existing->keys) {
            if (k.time < from || k.time > to) {
                curve.keys.push_back(k);
            }
        }
    }
    for (Keyframe& k : keys) {
        curve.keys.push_back(std::move(k));
    }
    std::stable_sort(curve.keys.begin(), curve.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    // Keys at the same time: the preset's (later in the list) win.
    std::vector<Keyframe> unique;
    for (const Keyframe& k : curve.keys) {
        if (!unique.empty() && unique.back().time == k.time) {
            unique.back() = k;
        } else {
            unique.push_back(k);
        }
    }
    curve.keys = std::move(unique);
    param = ParamValue(std::move(curve));
}

struct Window {
    Ticks from;
    Ticks to;
};

/// The windows a preset animates in: one for In / Out, one per cycle for Loop.
std::expected<std::vector<Window>, Error> windowsOf(const PresetOptions& o, Ticks start, Ticks end) {
    if (o.duration.value <= 0) {
        return std::unexpected(detail::invalidArgument("a preset needs a positive duration"));
    }
    if (end <= start) {
        return std::unexpected(detail::invalidArgument("the graphic has no time to animate in"));
    }
    const Ticks length{end.value - start.value};
    const Ticks d{std::min(o.duration.value, length.value)};
    switch (o.phase) {
    case PresetPhase::In:
        return std::vector<Window>{{start, Ticks{start.value + d.value}}};
    case PresetPhase::Out:
        return std::vector<Window>{{Ticks{end.value - d.value}, end}};
    case PresetPhase::Loop: {
        const std::int64_t cycles = std::max<std::int64_t>(1, length.value / o.duration.value);
        if (static_cast<std::size_t>(cycles) > kMaxLoopCycles) {
            return std::unexpected(detail::invalidArgument("a loop of more than " + std::to_string(kMaxLoopCycles) +
                                                           " cycles: use a longer duration"));
        }
        std::vector<Window> out;
        for (std::int64_t i = 0; i < cycles; ++i) {
            out.push_back({Ticks{start.value + i * d.value}, Ticks{start.value + (i + 1) * d.value}});
        }
        return out;
    }
    }
    return std::vector<Window>{};
}

/// Keys of a whole preset on one parameter: per window, from the resting value `base` (the value at the window's
/// resting end).
void animate(ParamValue& param, const std::vector<Window>& windows, PresetPhase phase,
             const std::function<std::vector<Keyframe>(Window, double base)>& keysFor) {
    for (const Window& w : windows) {
        const double base = param.valueAt(phase == PresetPhase::In ? w.to : w.from);
        setKeys(param, w.from, w.to, keysFor(w, base));
    }
}

void upsertAnimator(TextContent& text, TextAnimator animator) {
    const auto it = std::find_if(text.animators.begin(), text.animators.end(),
                                 [&](const TextAnimator& a) { return a.name == animator.name; });
    if (it != text.animators.end()) {
        *it = std::move(animator);
    } else {
        text.animators.push_back(std::move(animator));
    }
}

void upsertMask(std::vector<Mask>& masks, Mask mask) {
    const auto it = std::find_if(masks.begin(), masks.end(), [&](const Mask& m) { return m.name == mask.name; });
    if (it != masks.end()) {
        mask.id = it->id;
        *it = std::move(mask);
        return;
    }
    std::uint32_t id = 1;
    for (const Mask& m : masks) {
        id = std::max(id, m.id + 1);
    }
    mask.id = id;
    masks.push_back(std::move(mask));
}

bool horizontal(PresetDirection d) { return d == PresetDirection::Left || d == PresetDirection::Right; }
double sign(PresetDirection d) { return d == PresetDirection::Left || d == PresetDirection::Up ? -1.0 : 1.0; }

// ---- Template fields --------------------------------------------------------------------------------------------------

const TemplateField* findField(const Graphic& g, std::string_view id) {
    if (!g.instance) {
        return nullptr;
    }
    const auto& fields = g.instance->fields;
    const auto it = std::find_if(fields.begin(), fields.end(), [&](const TemplateField& f) { return f.id == id; });
    return it == fields.end() ? nullptr : &*it;
}

std::expected<void, Error> applyField(const ProjectState& state, Graphic& g, const TemplateField& field, const FieldValue& v) {
    if (field.kind == FieldKind::Duration && field.property == "clip") {
        return {}; // the clip's length: the caller trims
    }
    GraphicLayer* layer = g.findLayer(field.layer);
    if (!layer) {
        return std::unexpected(detail::invalidArgument("field " + field.id + ": layer " + std::to_string(field.layer) +
                                                       " does not exist"));
    }
    switch (field.kind) {
    case FieldKind::Text: {
        if (!isValidUtf8(v.text) || v.text.size() > kMaxGraphicTextBytes) {
            return std::unexpected(detail::invalidArgument("field " + field.id + ": text that is not UTF-8 or too long"));
        }
        const TextStyle style = layer->text.runs.empty() ? TextStyle{} : layer->text.runs.front().style;
        layer->text.runs = {TextRun{v.text, style}};
        break;
    }
    case FieldKind::Color:
        if (field.property == "fill") {
            if (layer->kind == LayerKind::Text) {
                for (TextRun& r : layer->text.runs) {
                    r.style.fill = v.color;
                }
            } else {
                layer->shape.fill.color = v.color;
            }
        } else if (field.property == "stroke") {
            layer->shape.stroke.color = v.color;
        } else if (field.property == "background") {
            layer->text.background.color = v.color;
        } else {
            layer->shadow.color = v.color;
        }
        break;
    case FieldKind::Font:
        for (TextRun& r : layer->text.runs) {
            if (!v.text.empty()) {
                r.style.fontFamily = v.text;
                r.style.brandFont.clear(); // a font chosen here wins over the brand's
            }
            r.style.fontStyle = v.fontStyle;
            if (v.fontWeight != 0) {
                r.style.fontWeight = v.fontWeight;
            }
        }
        break;
    case FieldKind::Image: {
        const MediaItem* media = state.findMedia(v.media);
        if (!media || media->info.video.empty()) {
            return std::unexpected(detail::invalidArgument("field " + field.id + ": media " + std::to_string(v.media.value) +
                                                           " is not a picture of this project"));
        }
        layer->image.media = v.media;
        layer->image.brandLogo.clear();
        break;
    }
    case FieldKind::Duration:
        if (v.duration <= layer->in) {
            return std::unexpected(detail::invalidArgument("field " + field.id + ": the layer must end after it starts"));
        }
        layer->out = v.duration;
        break;
    case FieldKind::Checkbox:
        layer->visible = v.flag;
        break;
    }
    return {};
}

Ticks defaultLength(const GraphicTemplate& t) {
    if (t.duration.value > 0) {
        return t.duration;
    }
    if (t.graphic.responsive) {
        return t.graphic.responsive->duration;
    }
    return Ticks::fromSeconds(5);
}

} // namespace

// ---- Names ------------------------------------------------------------------------------------------------------------

std::string_view toString(PresetKind v) { return nameOf(kPresetKinds, v); }
std::optional<PresetKind> presetKindFromString(std::string_view t) { return parseName(kPresetKinds, t); }
std::string_view toString(PresetPhase v) { return nameOf(kPresetPhases, v); }
std::optional<PresetPhase> presetPhaseFromString(std::string_view t) { return parseName(kPresetPhases, t); }
std::string_view toString(PresetEasing v) { return nameOf(kPresetEasings, v); }
std::optional<PresetEasing> presetEasingFromString(std::string_view t) { return parseName(kPresetEasings, t); }
std::string_view toString(PresetDirection v) { return nameOf(kPresetDirections, v); }
std::optional<PresetDirection> presetDirectionFromString(std::string_view t) { return parseName(kPresetDirections, t); }

// ---- Graphic clips ----------------------------------------------------------------------------------------------------

MediaItem graphicCanvasItem(std::int32_t width, std::int32_t height, Rate rate) {
    MediaItem item;
    // Named by its size and rate: pasting into another project matches generator items by name (Clipboard.cpp).
    std::string fps = std::to_string(std::round(static_cast<double>(rate.num) / rate.den * 100.0) / 100.0);
    fps.erase(fps.find_last_not_of('0') + 1);
    if (!fps.empty() && fps.back() == '.') {
        fps.pop_back();
    }
    item.name = "Graphics " + std::to_string(width) + "x" + std::to_string(height) + " " + fps + " fps";
    item.generator = GeneratorSpec{std::string(kGraphicGenerator), {}, {}, {}};
    item.info.isStillImage = true; // unbounded
    VideoStreamInfo video;
    video.index = 0;
    video.codec = std::string(kGraphicGenerator);
    video.width = width;
    video.height = height;
    video.frameRate = rate;
    video.alpha = AlphaMode::Straight;
    item.info.video.push_back(std::move(video));
    return item;
}

EditOutcome addGraphic(const ProjectState& state, SequenceId sequenceId, TrackId trackId, Ticks at, Ticks duration,
                       Graphic graphic, BinId bin) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const Track* track = (*seq)->findTrack(trackId);
    if (!track) {
        return std::unexpected(detail::notFound("track " + std::to_string(trackId.value) + " does not exist"));
    }
    if (track->kind != TrackKind::Video) {
        return std::unexpected(detail::invalidArgument("a graphic goes on a video track"));
    }
    if (track->locked) {
        return std::unexpected(detail::invalidArgument("track " + detail::trackLabel(**seq, trackId) + " is locked"));
    }
    if (duration.value <= 0 || at.value < 0 || duration.value >= (std::int64_t{1} << 60) || at.value >= (std::int64_t{1} << 60)) {
        return std::unexpected(detail::invalidArgument("a graphic needs a start >= 0 and a positive duration"));
    }
    giveIds(graphic);
    if (const auto problem = graphicProblem(graphic)) {
        return std::unexpected(detail::invalidArgument("the graphic " + *problem));
    }
    // One canvas item per frame size and rate, shared by the project's graphics.
    ProjectState base = state;
    MediaId canvas;
    for (const MediaPtr& m : state.media) {
        if (m->isGraphic() && m->info.video.size() == 1 && m->info.video[0].width == (*seq)->width &&
            m->info.video[0].height == (*seq)->height && m->info.video[0].frameRate == (*seq)->frameRate) {
            canvas = m->id;
            break;
        }
    }
    MediaId created;
    if (!canvas.isValid()) {
        MediaItem item = graphicCanvasItem((*seq)->width, (*seq)->height, (*seq)->frameRate);
        item.bin = bin;
        auto media = addMedia(state, std::move(item));
        if (!media) {
            return std::unexpected(media.error());
        }
        base = std::move(media->state);
        canvas = media->id;
        created = canvas;
    }
    const SourceClip source{canvas, TimeRange{Ticks{}, duration}, {SourcePatch{StreamRef{StreamKind::Video, 0}, trackId}}};
    auto placed = overwriteEdit(base, sequenceId, source, at);
    if (!placed) {
        return std::unexpected(placed.error());
    }
    if (placed->placed.size() != 1) {
        return std::unexpected(detail::invalidArgument("the graphic could not be placed"));
    }
    const ClipId clipId = placed->placed.front();
    const Sequence* placedSeq = placed->state.findSequence(sequenceId);
    const auto where = placedSeq->findClip(clipId);
    detail::SequenceEditor editor(placed->state, *placedSeq);
    Clip& clip = editor.mutableClip(where->track->id, where->index);
    ClipContent content = clip.content();
    content.name = graphic.name.empty() ? std::string("Graphic") : graphic.name;
    content.graphic = std::make_shared<const Graphic>(std::move(graphic));
    clip.setContent(std::move(content));
    EditResult finished = editor.finish();
    EditResult result = std::move(*placed);
    result.state = std::move(finished.state);
    result.changes = diff(state, result.state);
    result.createdMedia = created;
    return result;
}

EditOutcome setGraphic(const ProjectState& state, SequenceId sequence, ClipId clip, Graphic graphic) {
    const auto at = locate(state, sequence, clip);
    if (!at) {
        return std::unexpected(at.error());
    }
    giveIds(graphic);
    return replaceGraphic(state, *at, std::move(graphic));
}

EditOutcome addGraphicLayer(const ProjectState& state, SequenceId sequence, ClipId clip, GraphicLayer layer,
                            std::uint32_t parent, std::optional<std::size_t> index) {
    const auto at = locate(state, sequence, clip);
    if (!at) {
        return std::unexpected(at.error());
    }
    Graphic g = *at->clip->graphic();
    std::vector<GraphicLayer>* list = childrenOf(g, parent);
    if (!list) {
        return std::unexpected(detail::notFound("the graphic has no group layer " + std::to_string(parent)));
    }
    std::set<std::uint32_t> taken;
    collectIds(g.layers, taken);
    std::uint32_t next = g.nextLayerId();
    std::map<std::uint32_t, std::uint32_t> renamed;
    std::vector<GraphicLayer> one{std::move(layer)};
    renumber(one, taken, next, renamed);
    remapPins(one, renamed);
    const std::uint32_t id = one.front().id;
    const std::size_t at2 = std::min(index.value_or(list->size()), list->size());
    list->insert(list->begin() + static_cast<std::ptrdiff_t>(at2), std::move(one.front()));
    auto result = replaceGraphic(state, *at, std::move(g));
    if (result) {
        result->createdLayer = id;
    }
    return result;
}

EditOutcome updateGraphicLayer(const ProjectState& state, SequenceId sequence, ClipId clip, std::uint32_t layerId,
                               GraphicLayer layer) {
    const auto at = locate(state, sequence, clip);
    if (!at) {
        return std::unexpected(at.error());
    }
    Graphic g = *at->clip->graphic();
    GraphicLayer* target = g.findLayer(layerId);
    if (!target) {
        return std::unexpected(detail::notFound("the graphic has no layer " + std::to_string(layerId)));
    }
    layer.id = layerId;
    *target = std::move(layer);
    return replaceGraphic(state, *at, std::move(g));
}

EditOutcome removeGraphicLayer(const ProjectState& state, SequenceId sequence, ClipId clip, std::uint32_t layerId) {
    const auto at = locate(state, sequence, clip);
    if (!at) {
        return std::unexpected(at.error());
    }
    Graphic g = *at->clip->graphic();
    auto removed = extract(g.layers, layerId);
    if (!removed) {
        return std::unexpected(detail::notFound("the graphic has no layer " + std::to_string(layerId)));
    }
    std::set<std::uint32_t> gone;
    gone.insert(removed->id);
    collectIds(removed->children, gone);
    dropPinsTo(g.layers, gone);
    if (g.instance) {
        std::erase_if(g.instance->fields, [&](const TemplateField& f) { return gone.contains(f.layer); });
    }
    return replaceGraphic(state, *at, std::move(g));
}

// ---- Presets ----------------------------------------------------------------------------------------------------------

std::expected<GraphicLayer, Error> applyPresetToLayer(const GraphicLayer& source, const PresetOptions& o, Ticks start, Ticks end,
                                                      std::int32_t width, std::int32_t height) {
    const auto windows = windowsOf(o, start, end);
    if (!windows) {
        return std::unexpected(windows.error());
    }
    GraphicLayer layer = source;
    LayerTransform& t = layer.transform;
    const PresetPhase phase = o.phase;
    const double overshoot = std::clamp(o.overshoot, 0.0, 100.0);
    // Bounce always moves vertically; slides along their direction.
    const bool alongWidth = o.kind != PresetKind::Bounce && horizontal(o.direction);
    const double distance = o.distance > 0.0 ? o.distance : (alongWidth ? width : height) / 4.0;
    const bool in = phase == PresetPhase::In;
    switch (o.kind) {
    case PresetKind::Fade:
        animate(t.opacity, *windows, phase, [&](Window w, double base) {
            if (phase == PresetPhase::Loop) {
                return std::vector<Keyframe>{key(w.from, base, Interpolation::EaseInOut),
                                             key(lerpTicks(w.from, w.to, 0.5), base * 0.25, Interpolation::EaseInOut),
                                             key(w.to, base, Interpolation::EaseInOut)};
            }
            return in ? motion(w.from, 0.0, w.to, base, o.easing, 0.0) : motion(w.from, base, w.to, 0.0, o.easing, 0.0);
        });
        break;
    case PresetKind::Slide: {
        ParamValue& axis = horizontal(o.direction) ? t.x : t.y;
        const double offset = sign(o.direction) * distance;
        animate(axis, *windows, phase, [&](Window w, double base) {
            if (phase == PresetPhase::Loop) {
                return std::vector<Keyframe>{key(w.from, base, Interpolation::EaseInOut),
                                             key(lerpTicks(w.from, w.to, 0.5), base + offset, Interpolation::EaseInOut),
                                             key(w.to, base, Interpolation::EaseInOut)};
            }
            return in ? motion(w.from, base + offset, w.to, base, o.easing, overshoot)
                      : motion(w.from, base, w.to, base + offset, o.easing, overshoot);
        });
        break;
    }
    case PresetKind::ScalePop:
        for (ParamValue* scale : {&t.scaleX, &t.scaleY}) {
            animate(*scale, *windows, phase, [&](Window w, double base) {
                const double peak = base * (1.0 + overshoot / 100.0);
                if (phase == PresetPhase::Loop) {
                    return std::vector<Keyframe>{key(w.from, base, Interpolation::EaseInOut),
                                                 key(lerpTicks(w.from, w.to, 0.5), peak, Interpolation::EaseInOut),
                                                 key(w.to, base, Interpolation::EaseInOut)};
                }
                if (in) {
                    return std::vector<Keyframe>{key(w.from, 0.0), key(lerpTicks(w.from, w.to, 0.7), peak, Interpolation::EaseInOut),
                                                 key(w.to, base, Interpolation::EaseIn)};
                }
                return std::vector<Keyframe>{key(w.from, base, Interpolation::EaseOut),
                                             key(lerpTicks(w.from, w.to, 0.3), peak, Interpolation::EaseInOut), key(w.to, 0.0)};
            });
        }
        break;
    case PresetKind::Bounce: {
        // Up: drops in from above (or hops up on a loop); Down: comes up from below.
        const double s = o.direction == PresetDirection::Down ? 1.0 : -1.0;
        animate(t.y, *windows, phase, [&](Window w, double base) {
            const auto at = [&](double f) { return lerpTicks(w.from, w.to, f); };
            if (phase == PresetPhase::Loop) {
                return std::vector<Keyframe>{key(w.from, base, Interpolation::EaseOut),
                                             key(at(0.5), base + s * distance * 0.5, Interpolation::EaseInOut),
                                             key(w.to, base, Interpolation::EaseIn)};
            }
            if (in) {
                return std::vector<Keyframe>{key(w.from, base + s * distance, Interpolation::EaseOut),
                                             key(at(0.55), base, Interpolation::Linear),
                                             key(at(0.75), base + s * distance * 0.25, Interpolation::EaseInOut),
                                             key(at(0.9), base, Interpolation::Linear),
                                             key(at(0.96), base + s * distance * 0.06, Interpolation::EaseInOut),
                                             key(w.to, base, Interpolation::EaseIn)};
            }
            return std::vector<Keyframe>{key(w.from, base, Interpolation::EaseOut),
                                         key(at(0.3), base + s * distance * 0.25, Interpolation::EaseInOut),
                                         key(w.to, base - s * distance * 2.0, Interpolation::EaseIn)};
        });
        break;
    }
    case PresetKind::Typewriter:
    case PresetKind::WordByWord: {
        if (layer.kind != LayerKind::Text) {
            return std::unexpected(detail::invalidArgument("typewriter and word-by-word animate text layers"));
        }
        if (phase == PresetPhase::Loop) {
            return std::unexpected(detail::invalidArgument("typewriter and word-by-word have In and Out presets, no loop"));
        }
        const bool words = o.kind == PresetKind::WordByWord;
        TextAnimator a;
        a.name = std::string(words ? "Words " : "Typewriter ") + (in ? "In" : "Out");
        a.unit = words ? TextUnit::Word : TextUnit::Character;
        a.opacity = 0.0;
        a.shape = words ? TextSelectorShape::Ramp : TextSelectorShape::Square;
        a.softness = words ? 1.0 : 0.0;
        if (words) {
            a.offsetY = 0.02 * height; // words rise into place as they appear
        }
        const Window w = windows->front();
        if (in) {
            // Units from the selection's start on are hidden: moving the start from 0 to 100 % reveals them in order.
            setKeys(a.start, w.from, w.to, motion(w.from, 0.0, w.to, 100.0, o.easing, 0.0));
            a.end = ParamValue(100.0);
        } else {
            // The hidden range grows from the first unit to the last.
            a.start = ParamValue(0.0);
            setKeys(a.end, w.from, w.to, motion(w.from, 0.0, w.to, 100.0, o.easing, 0.0));
        }
        upsertAnimator(layer.text, std::move(a));
        break;
    }
    case PresetKind::Wipe: {
        if (phase == PresetPhase::Loop) {
            return std::unexpected(detail::invalidArgument("a wipe has In and Out presets, no loop"));
        }
        const Window w = windows->front();
        const Box b = layerBounds(layer, in ? w.to : w.from);
        const double W = width;
        const double H = height;
        // Mask parameters have ranges (centre -10..10, size 0..20 canvases): a layer far off the canvas is clamped.
        const double nw = std::clamp(b.width / W, 0.0, 20.0);
        const double nh = std::clamp(b.height / H, 0.0, 20.0);
        const double nx = std::clamp(b.x / W, -10.0, 10.0 - nw);
        const double ny = std::clamp(b.y / H, -10.0, 10.0 - nh);
        Mask mask;
        mask.name = "Wipe";
        mask.shape = MaskShape::Rectangle;
        ParamValue cx(nx + nw / 2.0);
        ParamValue cy(ny + nh / 2.0);
        ParamValue sx(nw);
        ParamValue sy(nh);
        // The visible part grows from the edge it comes from (In) or shrinks toward the edge it leaves by (Out).
        const bool alongX = horizontal(o.direction);
        const double lo = alongX ? nx : ny;
        const double size = alongX ? nw : nh;
        const bool fromLow = (o.direction == PresetDirection::Left || o.direction == PresetDirection::Up) == in;
        const double anchor = fromLow ? lo : lo + size;
        ParamValue& centre = alongX ? cx : cy;
        ParamValue& extent = alongX ? sx : sy;
        if (in) {
            setKeys(extent, w.from, w.to, motion(w.from, 0.0, w.to, size, o.easing, 0.0));
            setKeys(centre, w.from, w.to, motion(w.from, anchor, w.to, lo + size / 2.0, o.easing, 0.0));
        } else {
            setKeys(extent, w.from, w.to, motion(w.from, size, w.to, 0.0, o.easing, 0.0));
            setKeys(centre, w.from, w.to, motion(w.from, lo + size / 2.0, w.to, anchor, o.easing, 0.0));
        }
        mask.set("center.x", cx);
        mask.set("center.y", cy);
        mask.set("size.x", sx);
        mask.set("size.y", sy);
        upsertMask(layer.masks, std::move(mask));
        break;
    }
    }
    return layer;
}

EditOutcome applyGraphicPreset(const ProjectState& state, SequenceId sequence, ClipId clip, std::uint32_t layerId,
                               const PresetOptions& options) {
    const auto at = locate(state, sequence, clip);
    if (!at) {
        return std::unexpected(at.error());
    }
    Graphic g = *at->clip->graphic();
    GraphicLayer* layer = g.findLayer(layerId);
    if (!layer) {
        return std::unexpected(detail::notFound("the graphic has no layer " + std::to_string(layerId)));
    }
    Ticks start = at->clip->curveTimeAt(at->clip->start());
    Ticks end{start.value + at->clip->duration().value};
    if (g.responsive) {
        start = Ticks{};
        end = g.responsive->duration;
    }
    auto animated = applyPresetToLayer(*layer, options, start, end, g.width, g.height);
    if (!animated) {
        return std::unexpected(animated.error());
    }
    *layer = std::move(*animated);
    return replaceGraphic(state, *at, std::move(g));
}

// ---- Templates --------------------------------------------------------------------------------------------------------

FieldValue currentFieldValue(const Graphic& g, const TemplateField& field, Ticks clipDuration) {
    FieldValue v;
    if (field.kind == FieldKind::Duration && field.property == "clip") {
        v.duration = clipDuration;
        return v;
    }
    const GraphicLayer* l = g.findLayer(field.layer);
    if (!l) {
        return v;
    }
    switch (field.kind) {
    case FieldKind::Text:
        v.text = l->text.text();
        break;
    case FieldKind::Color:
        if (field.property == "fill") {
            v.color = l->kind == LayerKind::Text ? (l->text.runs.empty() ? GraphicColor{} : l->text.runs.front().style.fill)
                                                 : l->shape.fill.color;
        } else if (field.property == "stroke") {
            v.color = l->shape.stroke.color;
        } else if (field.property == "background") {
            v.color = l->text.background.color;
        } else {
            v.color = l->shadow.color;
        }
        break;
    case FieldKind::Font:
        if (!l->text.runs.empty()) {
            const TextStyle& s = l->text.runs.front().style;
            v.text = s.fontFamily;
            v.fontStyle = s.fontStyle;
            v.fontWeight = s.fontWeight;
        }
        break;
    case FieldKind::Image:
        v.media = l->image.media;
        break;
    case FieldKind::Duration:
        v.duration = l->out.value_or(Ticks{});
        break;
    case FieldKind::Checkbox:
        v.flag = l->visible;
        break;
    }
    return v;
}

std::expected<GraphicTemplate, Error> makeTemplate(const Graphic& graphic, std::string id, std::string name,
                                                   std::vector<TemplateField> fields, Ticks duration, std::string description) {
    GraphicTemplate t;
    t.id = std::move(id);
    t.name = std::move(name);
    t.description = std::move(description);
    t.graphic = graphic;
    t.graphic.instance.reset();
    if (t.graphic.name.empty()) {
        t.graphic.name = t.name;
    }
    t.duration = duration;
    for (TemplateField& f : fields) {
        f.value = currentFieldValue(t.graphic, f, duration);
    }
    t.fields = std::move(fields);
    if (const auto problem = templateProblem(t)) {
        return std::unexpected(detail::invalidArgument(*problem));
    }
    return t;
}

EditOutcome saveProjectTemplate(const ProjectState& state, GraphicTemplate tmpl) {
    if (const auto problem = templateProblem(tmpl)) {
        return std::unexpected(detail::invalidArgument(*problem));
    }
    EditResult result;
    result.state = state;
    std::vector<GraphicTemplate>& list = result.state.templates.edit();
    const auto it = std::lower_bound(list.begin(), list.end(), tmpl.id,
                                     [](const GraphicTemplate& t, const std::string& id) { return t.id < id; });
    if (it != list.end() && it->id == tmpl.id) {
        tmpl.version = std::max(tmpl.version, it->version + 1);
        *it = std::move(tmpl);
    } else {
        if (list.size() >= kMaxProjectTemplates) {
            return std::unexpected(detail::invalidArgument("a project holds at most " + std::to_string(kMaxProjectTemplates) +
                                                           " templates"));
        }
        list.insert(it, std::move(tmpl));
    }
    result.changes = diff(state, result.state);
    return result;
}

EditOutcome removeProjectTemplate(const ProjectState& state, std::string_view id) {
    if (!state.findTemplate(id)) {
        return std::unexpected(detail::notFound("the project has no template \"" + std::string(id) + "\""));
    }
    EditResult result;
    result.state = state;
    std::erase_if(result.state.templates.edit(), [&](const GraphicTemplate& t) { return t.id == id; });
    result.changes = diff(state, result.state);
    return result;
}

std::expected<Graphic, Error> withFieldValues(const ProjectState& state, Graphic g,
                                              const std::vector<std::pair<std::string, FieldValue>>& values) {
    if (!g.instance) {
        return std::unexpected(detail::invalidArgument("the graphic is not made from a template"));
    }
    for (const auto& [id, value] : values) {
        const TemplateField* field = findField(g, id);
        if (!field) {
            return std::unexpected(detail::invalidArgument("the template has no field \"" + id + "\""));
        }
        const TemplateField copy = *field;
        if (auto applied = applyField(state, g, copy, value); !applied) {
            return std::unexpected(applied.error());
        }
        for (TemplateField& f : g.instance->fields) {
            if (f.id == id) {
                f.value = value;
            }
        }
    }
    if (const auto problem = graphicProblem(g)) {
        return std::unexpected(detail::invalidArgument("the graphic " + *problem));
    }
    return g;
}

EditOutcome instantiateTemplate(const ProjectState& state, SequenceId sequence, TrackId track, Ticks at,
                                std::optional<Ticks> duration, const GraphicTemplate& tmpl,
                                const std::vector<std::pair<std::string, FieldValue>>& values, BinId bin) {
    if (const auto problem = templateProblem(tmpl)) {
        return std::unexpected(detail::invalidArgument(*problem));
    }
    Graphic g = tmpl.graphic;
    g.instance = TemplateInstance{tmpl.id, tmpl.version, tmpl.fields};
    if (g.name.empty()) {
        g.name = tmpl.name;
    }
    auto filled = withFieldValues(state, std::move(g), values);
    if (!filled) {
        return std::unexpected(filled.error());
    }
    Ticks length = duration.value_or(defaultLength(tmpl));
    for (const auto& [id, value] : values) {
        const TemplateField* f = findField(*filled, id);
        if (f && f->kind == FieldKind::Duration && f->property == "clip" && value.duration.value > 0) {
            length = value.duration;
        }
    }
    return addGraphic(state, sequence, track, at, length, std::move(*filled), bin);
}

EditOutcome setTemplateFields(const ProjectState& state, SequenceId sequence, ClipId clip,
                              const std::vector<std::pair<std::string, FieldValue>>& values) {
    const auto at = locate(state, sequence, clip);
    if (!at) {
        return std::unexpected(at.error());
    }
    auto filled = withFieldValues(state, *at->clip->graphic(), values);
    if (!filled) {
        return std::unexpected(filled.error());
    }
    std::optional<Ticks> length;
    for (const auto& [id, value] : values) {
        const TemplateField* f = findField(*filled, id);
        if (f && f->kind == FieldKind::Duration && f->property == "clip") {
            if (value.duration.value <= 0) {
                return std::unexpected(detail::invalidArgument("field " + id + ": a clip needs a positive length"));
            }
            length = value.duration;
        }
    }
    auto replaced = replaceGraphic(state, *at, std::move(*filled));
    if (!replaced || !length || *length == at->clip->duration()) {
        return replaced;
    }
    auto trimmed = trimEdge(replaced->state, sequence, clip, Edge::Tail, Ticks{length->value - at->clip->duration().value},
                            LinkMode::FollowLinks);
    if (!trimmed) {
        return std::unexpected(trimmed.error());
    }
    trimmed->changes = diff(state, trimmed->state);
    return trimmed;
}

// ---- Brand kit --------------------------------------------------------------------------------------------------------

EditOutcome setBrandKit(const ProjectState& state, std::optional<BrandKit> kit) {
    if (kit) {
        if (const auto problem = brandKitProblem(*kit)) {
            return std::unexpected(detail::invalidArgument("the brand kit " + *problem));
        }
        for (const BrandLogo& logo : kit->logos) {
            const MediaItem* media = state.findMedia(logo.media);
            if (!media || media->info.video.empty()) {
                return std::unexpected(detail::invalidArgument("brand logo '" + logo.name + "': media " +
                                                               std::to_string(logo.media.value) + " is not a picture of this project"));
            }
        }
    }
    if (state.brandKit == kit) {
        return detail::unchanged(state);
    }
    EditResult result;
    result.state = state;
    result.state.brandKit = std::move(kit);
    result.changes = diff(state, result.state);
    return result;
}

} // namespace wabcut::edit
