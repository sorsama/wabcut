#include <wabcut/playback/video/Evaluate.h>

#include <wabcut/color/Looks.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/RedactionMedia.h>
#include <wabcut/model/Resolve.h>
#include <wabcut/model/Synthetic.h>
#include <wabcut/model/GraphicResolve.h>
#include <wabcut/model/TitleOps.h>

#include <QDateTime>
#include <QFileInfo>
#include <QImageReader>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace wabcut::playback {

bool coversCanvas(const render::LayerTransform& transform, double frameWidth, double frameHeight,
                  double pixelAspect, render::CanvasSize canvas) {
    const double turns = transform.rotationDegrees / 90.0;
    if (turns != std::floor(turns) || frameWidth <= 0 || frameHeight <= 0) {
        return false; // only axis-aligned layers can cover the canvas exactly
    }
    const render::Affine2D m = render::layerToCanvas(transform, frameWidth, frameHeight, pixelAspect, canvas);
    const render::Vec2 a = m.map({0, 0});
    const render::Vec2 b = m.map({frameWidth, frameHeight});
    constexpr double kEps = 1e-6;
    return std::min(a.x, b.x) <= kEps && std::min(a.y, b.y) <= kEps &&
           std::max(a.x, b.x) >= canvas.width - kEps && std::max(a.y, b.y) >= canvas.height - kEps;
}

Ticks sourceTimeAround(const Clip& clip, Ticks t) {
    if (clip.isRemapped()) {
        return clip.sourceTimeAt(t); // the remap also defines transition handles outside the clip
    }
    if (clip.isFreeze()) {
        return clip.sourceIn;
    }
    if (clip.timelineRange.contains(t)) {
        return clip.sourceTimeAt(t);
    }
    // Outside the clip (a transition's handles): the same mapping, unclamped. Exact rational time (floor).
    const Ticks offset = toSourceTicks(t - clip.start(), clip.speed);
    const Ticks source = clip.reversed ? clip.sourceOut() - Ticks{1} - offset : clip.sourceIn + offset;
    return Ticks{std::max<std::int64_t>(0, source.value)};
}

Ticks curveTimeAround(const Clip& clip, Ticks t) {
    return clip.isFreeze() || clip.isRemapped() ? clip.curveTimeAt(t) : sourceTimeAround(clip, t);
}

namespace {

struct EffectMapping {
    std::string_view id;
    render::EffectType type;
    std::array<std::string_view, 20> keys;
};

// The registry's video stack effects the compositor implements, with the order of LayerEffect::params.
constexpr std::array<EffectMapping, 11> kEffects{{
    {"wabcut.mask", render::EffectType::OpacityMask, {}},
    {"wabcut.mosaic", render::EffectType::Mosaic, {"cellSize"}},
    {"wabcut.crop", render::EffectType::Crop, {"left", "top", "right", "bottom", "feather"}},
    {"wabcut.blur.gaussian", render::EffectType::GaussianBlur, {"radius", "repeatEdges"}},
    {"wabcut.color.basic", render::EffectType::BasicCorrection,
     {"exposure", "contrast", "saturation", "temperature", "tint"}},
    {"wabcut.vignette", render::EffectType::Vignette, {"amount", "midpoint", "roundness", "feather"}},
    {"wabcut.sharpen", render::EffectType::Sharpen, {"amount", "radius"}},
    {"wabcut.dropShadow", render::EffectType::DropShadow,
     {"opacity", "angle", "distance", "softness", "red", "green", "blue"}},
    {"wabcut.border", render::EffectType::Border, {"width", "color.r", "color.g", "color.b", "opacity"}}, // C.15 layouts
    {"wabcut.key.ultra", render::EffectType::UltraKey, // C.15 keyer (GPU in 0.1.1)
     {"output", "keyColor.r", "keyColor.g", "keyColor.b", "transparency", "highlight", "shadow", "tolerance", "pedestal",
      "choke", "soften", "contrast", "midPoint", "desaturate", "range", "spill", "luma", "saturation", "hue", "luminance"}},
    // C.15 Track Matte Key (GPU in 0.1.2); params[3], whether the matte track is valid, is set by the level's evaluation.
    {"wabcut.key.trackMatte", render::EffectType::TrackMatte, {"matteTrack", "compositeUsing", "reverse"}},
}};

/// The clip and the transition covering `time` on `track`, if any. The clip at `time` is the transition's A
/// (time before the cut) or B (at or after it); its neighbour is the other side.
struct TransitionHit {
    const Transition* transition = nullptr;
    const Clip* outgoing = nullptr;
    const Clip* incoming = nullptr;
    Ticks cut{};
};

std::optional<TransitionHit> transitionAt(const Track& track, Ticks time) {
    if (track.transitions.empty()) {
        return std::nullopt;
    }
    const auto index = track.clipIndexAt(time);
    if (!index) {
        return std::nullopt;
    }
    const Clip& here = track.clips[*index];
    for (const Transition& tr : track.transitions) {
        TransitionHit hit;
        hit.transition = &tr;
        if (tr.outgoing == here.id) {
            hit.outgoing = &here;
            hit.cut = here.end();
            if (tr.incoming.isValid() && *index + 1 < track.clips.size() &&
                track.clips[*index + 1].id == tr.incoming) {
                hit.incoming = &track.clips[*index + 1];
            }
        } else if (tr.incoming == here.id) {
            hit.incoming = &here;
            hit.cut = here.start();
            if (tr.outgoing.isValid() && *index > 0 && track.clips[*index - 1].id == tr.outgoing) {
                hit.outgoing = &track.clips[*index - 1];
            }
        } else {
            continue;
        }
        if (time >= hit.cut - tr.before && time < hit.cut + tr.after) {
            return hit;
        }
    }
    return std::nullopt;
}

std::optional<render::LayerTransition> transitionMix(const Transition& tr, Ticks cut, Ticks time) {
    render::LayerTransition mix;
    if (tr.effect == "crossDissolve") {
        mix.kind = render::TransitionKind::CrossDissolve;
    } else if (tr.effect == "dipToBlack" || tr.effect == "dipToWhite") {
        mix.kind = render::TransitionKind::DipToColor;
        const double c = tr.effect == "dipToWhite" ? 1.0 : 0.0;
        mix.color = {c, c, c};
    } else if (tr.effect == "wipe") {
        mix.kind = render::TransitionKind::Wipe;
        for (const EffectParam& p : tr.params) {
            if (p.key == "angle") {
                mix.angleDegrees = p.value.valueAt(Ticks{});
            } else if (p.key == "feather") {
                mix.feather = std::clamp(p.value.valueAt(Ticks{}), 0.0, 1.0);
            }
        }
    } else {
        return std::nullopt; // unknown transition: a plain cut
    }
    const Ticks length = tr.before + tr.after;
    const double progress = length.value > 0 ? static_cast<double>((time - (cut - tr.before)).value) /
                                                   static_cast<double>(length.value)
                                             : 1.0;
    mix.progress = std::clamp(progress, 0.0, 1.0);
    return mix;
}

std::uint64_t graphicMix(std::uint64_t key, std::uint64_t value) {
    return (key ^ (value + 0x9E3779B97F4A7C15ull + (key << 6) + (key >> 2))) * 1099511628211ull;
}

/// Pictures of a graphic's image items (C.13): each media item's file, read once (QImageReader: stills such as brand
/// logos; a file Qt cannot read draws nothing) and kept by path, size and modification time.
class FilePictures final : public render::GraphicImageSource {
public:
    explicit FilePictures(std::vector<std::pair<MediaId, std::string>> files) : m_files(std::move(files)) {}

    QImage picture(MediaId media) const override {
        for (const auto& [id, path] : m_files) {
            if (id == media) {
                return cached(path);
            }
        }
        return {};
    }

private:
    static QImage cached(const std::string& path) {
        struct Entry {
            std::string path;
            std::int64_t size = -1;
            std::int64_t mtime = 0;
            QImage image;
        };
        static std::mutex mutex;
        static std::list<Entry> entries; // most recent first, at most 32 pictures
        const QFileInfo info(QString::fromStdString(path));
        const std::int64_t size = info.exists() ? info.size() : -1;
        const std::int64_t mtime = info.exists() ? info.lastModified().toMSecsSinceEpoch() : 0;
        {
            std::lock_guard lock(mutex);
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if (it->path == path && it->size == size && it->mtime == mtime) {
                    entries.splice(entries.begin(), entries, it);
                    return entries.front().image;
                }
            }
        }
        QImageReader reader(QString::fromStdString(path));
        reader.setAutoTransform(true);
        const QImage image = size >= 0 ? reader.read() : QImage();
        std::lock_guard lock(mutex);
        std::erase_if(entries, [&](const Entry& e) { return e.path == path; });
        entries.push_front(Entry{path, size, mtime, image});
        if (entries.size() > 32) {
            entries.pop_back();
        }
        return image;
    }

    std::vector<std::pair<MediaId, std::string>> m_files;
};

std::shared_ptr<const render::GraphicImageSource> graphicPicturesOf(const ProjectState& project, const DrawList& list) {
    std::vector<std::pair<MediaId, std::string>> files;
    for (const DrawItem& d : list.items) {
        if (d.kind != DrawKind::Image) {
            continue;
        }
        const MediaItem* media = project.findMedia(d.image.media);
        if (media && !media->isGenerator() && !media->locator.offline && !media->locator.path.empty()) {
            files.emplace_back(d.image.media, media->locator.path);
        }
    }
    return files.empty() ? nullptr : std::make_shared<const FilePictures>(std::move(files));
}

/// The layer of `clip` (a media, title or graphic clip) at `time` (inside or around its range); nullopt when it shows
/// nothing.
/// `path`: the nested clips leading to it (C.8).
std::optional<LayerSpec> layerOf(const ProjectState& project, const Clip& clip, Ticks time, int track,
                                 render::CanvasSize canvas, const PreviewSettings* preview, const color::LutResolver* luts,
                                 std::span<const ClipId> path) {
    if (!clip.enabled || clip.stream.kind != StreamKind::Video) {
        return std::nullopt;
    }
    const MediaItem* item = project.findMedia(clip.media);
    if (item && item->isTitle()) {
        // A generated title: no decode, drawn by the renderer at the curve time.
        const Ticks curveTime = curveTimeAround(clip, time);
        const ClipProperties& p = clip.properties();
        LayerSpec spec;
        spec.clip = clip.id;
        spec.media = clip.media;
        spec.item = item;
        spec.sourceTime = curveTime;
        spec.transform.position = {p.value(ParamId::PositionX, curveTime), p.value(ParamId::PositionY, curveTime)};
        spec.transform.scale = p.value(ParamId::Scale, curveTime);
        spec.transform.rotationDegrees = p.value(ParamId::Rotation, curveTime);
        spec.transform.anchor = {p.value(ParamId::AnchorX, curveTime), p.value(ParamId::AnchorY, curveTime)};
        spec.opacity = std::clamp(p.value(ParamId::Opacity, curveTime), 0.0, 1.0);
        spec.blend = clip.blendMode();
        spec.track = track;
        if (spec.transform.scale == 0.0 || item->generator->text.empty()) {
            return std::nullopt;
        }
        evaluateEffects(clip, curveTime, spec.effects, spec.bypassedEffects, luts,
                        static_cast<double>(canvas.width) / std::max(1, canvas.height));
        spec.title = evaluateTitle(*item, clip, curveTime, canvas);
        return spec;
    }
    if (item && item->isGraphic()) {
        // A motion graphic (C.13): its draw list at this time, rasterised by the renderer; nothing to decode.
        const Graphic* graphic = clip.graphic();
        if (!graphic || item->info.video.empty()) {
            return std::nullopt;
        }
        const Ticks curveTime = curveTimeAround(clip, time);
        const ClipProperties& p = clip.properties();
        LayerSpec spec;
        spec.clip = clip.id;
        spec.media = clip.media;
        spec.item = item;
        spec.sourceTime = curveTime;
        spec.transform.position = {p.value(ParamId::PositionX, curveTime), p.value(ParamId::PositionY, curveTime)};
        spec.transform.scale = p.value(ParamId::Scale, curveTime);
        spec.transform.rotationDegrees = p.value(ParamId::Rotation, curveTime);
        spec.transform.anchor = {p.value(ParamId::AnchorX, curveTime), p.value(ParamId::AnchorY, curveTime)};
        spec.opacity = std::clamp(p.value(ParamId::Opacity, curveTime), 0.0, 1.0);
        spec.blend = clip.blendMode();
        spec.track = track;
        if (spec.transform.scale == 0.0) {
            return std::nullopt;
        }
        ResolveOptions options;
        options.measurer = &render::graphicTextMeasurer(); // the rasteriser's layout: pins hug what is drawn
        options.lineBreaker = &render::qtLineBreaker();
        auto list = std::make_shared<const DrawList>(resolveGraphicClip(project, clip, time, options));
        if (list->items.empty()) {
            return std::nullopt;
        }
        const VideoStreamInfo& frame = item->info.video.front();
        spec.graphicSize = {frame.width > 0 ? frame.width : canvas.width, frame.height > 0 ? frame.height : canvas.height};
        spec.graphicImages = graphicPicturesOf(project, *list);
        std::uint64_t key = graphicMix(0x6A0F'1C00ull, clip.id.value);
        for (const ClipId c : path) {
            key = graphicMix(key, c.value);
        }
        for (const DrawItem& d : list->items) { // a relinked picture is a new raster
            if (d.kind == DrawKind::Image) {
                key = graphicMix(key, reinterpret_cast<std::uintptr_t>(project.findMedia(d.image.media)));
            }
        }
        spec.graphicKey = key;
        spec.graphic = std::move(list);
        evaluateEffects(clip, curveTime, spec.effects, spec.bypassedEffects, luts,
                        static_cast<double>(canvas.width) / std::max(1, canvas.height));
        return spec;
    }
    if (item && item->isGenerator()) {
        // C.15 synthetic media: a colour matte, black or transparent video is one fill of the item's frame size, placed
        // and processed like a picture. Other generators (adjustment layers: not drawn yet) are left out.
        const SyntheticDescriptor* synthetic = syntheticOf(*item);
        if (!synthetic || item->info.video.empty()) {
            return std::nullopt;
        }
        const Ticks curveTime = curveTimeAround(clip, time);
        const ClipProperties& p = clip.properties();
        LayerSpec spec;
        spec.clip = clip.id;
        spec.media = clip.media;
        spec.item = item;
        spec.sourceTime = curveTime;
        spec.transform.position = {p.value(ParamId::PositionX, curveTime), p.value(ParamId::PositionY, curveTime)};
        spec.transform.scale = p.value(ParamId::Scale, curveTime);
        spec.transform.rotationDegrees = p.value(ParamId::Rotation, curveTime);
        spec.transform.anchor = {p.value(ParamId::AnchorX, curveTime), p.value(ParamId::AnchorY, curveTime)};
        spec.opacity = std::clamp(p.value(ParamId::Opacity, curveTime), 0.0, 1.0);
        spec.blend = clip.blendMode();
        spec.track = track;
        if (spec.transform.scale == 0.0) {
            return std::nullopt;
        }
        const auto byteOf = [&](std::string_view key) { // the sequence's encoding, as the raster's
            return static_cast<std::uint8_t>(std::lround(std::clamp(syntheticParam(*item, key), 0.0, 1.0) * 255.0));
        };
        switch (synthetic->kind) {
        case SyntheticKind::ColorMatte:
            spec.fill = std::array<std::uint8_t, 4>{byteOf("color.r"), byteOf("color.g"), byteOf("color.b"), 255};
            break;
        case SyntheticKind::BlackVideo:
            spec.fill = std::array<std::uint8_t, 4>{0, 0, 0, 255};
            break;
        case SyntheticKind::TransparentVideo:
            spec.fill = std::array<std::uint8_t, 4>{0, 0, 0, 0};
            break;
        }
        const VideoStreamInfo& frame = item->info.video.front();
        spec.fillSize = {frame.width > 0 ? frame.width : canvas.width, frame.height > 0 ? frame.height : canvas.height};
        evaluateEffects(clip, curveTime, spec.effects, spec.bypassedEffects, luts,
                        static_cast<double>(canvas.width) / std::max(1, canvas.height));
        return spec;
    }
    if (!item || clip.stream.index < 0 || clip.stream.index >= static_cast<int>(item->info.video.size())) {
        return std::nullopt; // missing media: nothing to decode
    }
    if (item->locator.offline && !(preview && resolveSource(*item, *preview).path)) {
        return std::nullopt; // offline, and no proxy stands in for it
    }
    const VideoStreamInfo& stream = item->info.video[static_cast<std::size_t>(clip.stream.index)];
    const Ticks curveTime = curveTimeAround(clip, time);
    const ClipProperties& p = clip.properties();

    LayerSpec spec;
    spec.clip = clip.id;
    spec.media = clip.media;
    spec.item = item;
    spec.streamIndex = clip.stream.index;
    spec.containerStream = stream.index;
    // A still image has one picture however long its clip runs (RC.1: asking the decoder for later source times failed
    // in export, "Seeking ... failed").
    spec.sourceTime = item->info.isStillImage ? Ticks{} : sourceTimeAround(clip, time);
    spec.transform.position = {p.value(ParamId::PositionX, curveTime), p.value(ParamId::PositionY, curveTime)};
    spec.transform.scale = p.value(ParamId::Scale, curveTime);
    spec.transform.rotationDegrees = p.value(ParamId::Rotation, curveTime);
    spec.transform.anchor = {p.value(ParamId::AnchorX, curveTime), p.value(ParamId::AnchorY, curveTime)};
    spec.opacity = std::clamp(p.value(ParamId::Opacity, curveTime), 0.0, 1.0); // the one clamp the model allows
    spec.blend = clip.blendMode();
    spec.track = track;
    if (spec.transform.scale == 0.0) {
        return std::nullopt;
    }
    // Display rotation of the stream (stored pixels are not rotated): applied before the clip's own motion, so
    // the anchor (given in displayed pixels) turns back into stored pixels.
    const int quarter = ((stream.rotationDegrees / 90) % 4 + 4) % 4;
    if (quarter != 0) {
        spec.transform.rotationDegrees += 90.0 * quarter;
        const render::Vec2 a = spec.transform.anchor;
        spec.transform.anchor = quarter == 1   ? render::Vec2{a.y, -a.x}
                                : quarter == 2 ? render::Vec2{-a.x, -a.y}
                                               : render::Vec2{-a.y, a.x};
    }
    // Masks and redactions are isotropic in displayed pixels: the stored frame's displayed aspect.
    const double aspect = stream.height > 0 ? static_cast<double>(stream.width) *
                                                  (stream.sampleAspect.isValid() ? stream.sampleAspect.toDouble() : 1.0) /
                                                  stream.height
                                            : 16.0 / 9.0;
    evaluateEffects(clip, curveTime, spec.effects, spec.bypassedEffects, luts, aspect, spec.sourceTime);
    spec.redactions = evaluateRedactions(project, clip, spec.sourceTime, aspect, stream.frameRate, path);
    spec.sourceRedactions = redactionsCovering(project, clip.media);
    spec.redactionAspect = aspect;
    return spec;
}

} // namespace

namespace {

/// A redaction's coverage at full strength (Codex wave-4 P2-29): opacity 1 and never inverted, whatever the region
/// says (an inverted redaction would expose exactly what it marks; a region from an older file or a direct edit can say
/// so), and grown by half its feather, so the soft edge lies outside the marked area instead of across it.
std::shared_ptr<const render::MaskSpec> hardenedRedaction(std::shared_ptr<const render::MaskSpec> spec) {
    if (!spec) {
        return spec;
    }
    auto hard = std::make_shared<render::MaskSpec>(*spec);
    for (render::MaskShapeSpec& shape : hard->shapes) {
        shape.opacity = 1.0;
        shape.inverted = false;
        shape.mode = 0; // add
        shape.expansion += shape.feather / 2.0;
    }
    hard->seal();
    return hard;
}

/// Whether `r` applies to `clip` reached through the nested clips `path`: excepting a nested clip excepts everything
/// inside it (C.8; resolveLeaves applies the same rule).
bool appliesAlong(const Redaction& r, ClipId clip, std::span<const ClipId> path) {
    return r.appliesTo(clip) && std::ranges::all_of(path, [&](ClipId outer) { return r.appliesTo(outer); });
}

/// `covering`: the redactions of the clip's media (redactionsCovering: subclip masters and stem sources included).
std::vector<render::LayerRedaction> redactionsAt(std::span<const Redaction* const> covering, ClipId clip,
                                                Ticks geometryTime, double frameAspect, std::optional<TimeRange> shown,
                                                std::span<const ClipId> path) {
    std::vector<render::LayerRedaction> out;
    for (const Redaction* covered : covering) {
        const Redaction& r = *covered;
        if (!appliesAlong(r, clip, path) || !(shown ? r.source.overlaps(*shown) : r.source.contains(geometryTime))) {
            continue;
        }
        render::LayerRedaction red;
        red.style = r.style == RedactionStyle::Mosaic  ? render::LayerRedaction::Style::Mosaic
                    : r.style == RedactionStyle::Solid ? render::LayerRedaction::Style::Solid
                                                       : render::LayerRedaction::Style::Blur;
        red.strength = r.strength;
        red.color = {r.colorR, r.colorG, r.colorB};
        red.mask = redactionMaskAt(r, geometryTime, frameAspect);
        if (red.mask) {
            out.push_back(std::move(red));
        }
    }
    return out;
}

} // namespace

std::shared_ptr<const render::MaskSpec> redactionMaskAt(const Redaction& redaction, Ticks mediaTime, double frameAspect) {
    Mask region = redaction.region;
    region.mode = MaskMode::Add;
    region.enabled = true;
    // AI-14: a matte region fails closed (render/Matte.h): the nearest matte grown, or the bounding box grown.
    return hardenedRedaction(render::maskSpecAt(std::span<const Mask>(&region, 1), mediaTime, frameAspect, render::MatteUse::Redaction));
}

std::vector<render::LayerRedaction> evaluateRedactions(const ProjectState& project, const Clip& clip, Ticks sourceTime,
                                                       double frameAspect, Rate frameRate, std::span<const ClipId> path) {
    // A nominal span is useful before decode; buildCompositeLayers replaces it with the actual displayed span.
    std::optional<TimeRange> shown;
    if (const auto k = ticksToFrames(sourceTime, frameRate, Rounding::Floor)) {
        const auto start = framesToTicks(*k, frameRate, Rounding::Floor);
        const auto end = framesToTicks(*k + 1, frameRate, Rounding::Floor);
        if (start && end && *end > *start) {
            shown = TimeRange::fromStartEnd(*start, *end);
        }
    }
    return redactionsAt(redactionsCovering(project, clip.media), clip.id, sourceTime, frameAspect, shown, path);
}

namespace {

void evaluateEffectStack(std::span<const EffectInstance> stack, Ticks curveTime, std::vector<render::LayerEffect>& out,
                         int& bypassed, const color::LutResolver* luts, double frameAspect, std::optional<Ticks> mediaTime);

} // namespace

void evaluateEffects(const Clip& clip, Ticks curveTime, std::vector<render::LayerEffect>& out, int& bypassed,
                     const color::LutResolver* luts, double frameAspect, std::optional<Ticks> mediaTime) {
    evaluateEffectStack(clip.effects(), curveTime, out, bypassed, luts, frameAspect, mediaTime);
}

namespace {

void evaluateEffectStack(std::span<const EffectInstance> stack, Ticks curveTime, std::vector<render::LayerEffect>& out,
                         int& bypassed, const color::LutResolver* luts, double frameAspect, std::optional<Ticks> mediaTime) {
    out.clear();
    bypassed = 0;
    for (const EffectInstance& instance : stack) {
        if (!instance.enabled) {
            continue;
        }
        if (instance.effect == edit::kTitleStyleEffect) {
            // A title's style: the text itself is the layer (evaluateTitle); its shadow is a drop shadow here.
            const double opacity = effectParameterAt(instance, "shadow.opacity", curveTime);
            if (opacity > 0.0) {
                render::LayerEffect shadow;
                shadow.type = render::EffectType::DropShadow;
                shadow.params = {opacity,
                                 effectParameterAt(instance, "shadow.angle", curveTime),
                                 effectParameterAt(instance, "shadow.distance", curveTime),
                                 effectParameterAt(instance, "shadow.softness", curveTime),
                                 effectParameterAt(instance, "shadow.red", curveTime),
                                 effectParameterAt(instance, "shadow.green", curveTime),
                                 effectParameterAt(instance, "shadow.blue", curveTime), 0.0};
                out.push_back(shadow);
            }
            continue;
        }
        if (instance.effect == color::kGradeEffect && isEvaluable(instance, EffectKind::Video)) {
            static const color::LutResolver builtinOnly = [](std::string_view reference) {
                return color::builtinLut(reference);
            };
            render::LayerEffect grade;
            grade.type = render::EffectType::Grade;
            grade.grade = std::make_shared<const color::GradeSpec>(
                color::evaluateGrade(instance, curveTime, luts ? *luts : builtinOnly));
            grade.mask = render::maskSpecAt(instance.masks, curveTime, frameAspect, render::MatteUse::Effect, mediaTime); // local corrections (I.6)
            out.push_back(std::move(grade));
            continue;
        }
        const auto mapping = std::ranges::find_if(kEffects, [&](const EffectMapping& m) { return m.id == instance.effect; });
        if (mapping == kEffects.end() || !isEvaluable(instance, EffectKind::Video)) {
            ++bypassed;
            continue;
        }
        render::LayerEffect effect;
        effect.type = mapping->type;
        for (std::size_t k = 0; k < mapping->keys.size() && !mapping->keys[k].empty(); ++k) {
            effect.params[k] = effectParameterAt(instance, mapping->keys[k], curveTime);
        }
        effect.mask = render::maskSpecAt(instance.masks, curveTime, frameAspect, render::MatteUse::Effect, mediaTime);
        if (effect.type == render::EffectType::OpacityMask && !effect.mask) {
            continue; // no enabled mask: nothing is cut out
        }
        out.push_back(std::move(effect));
    }
}

} // namespace

render::TitleStyle evaluateTitle(const MediaItem& title, const Clip& clip, Ticks curveTime, render::CanvasSize canvas) {
    render::TitleStyle style;
    if (title.generator) {
        style.text = title.generator->text;
        style.fontFamily = title.generator->fontFamily;
        style.fontStyle = title.generator->fontStyle == "Regular" ? std::string() : title.generator->fontStyle;
    }
    style.frameWidth = canvas.width;
    style.frameHeight = canvas.height;
    EffectInstance defaults;
    defaults.effect = std::string(edit::kTitleStyleEffect);
    const EffectInstance* instance = &defaults;
    for (const EffectInstance& e : clip.effects()) {
        if (e.effect == edit::kTitleStyleEffect && e.enabled) {
            instance = &e;
            break;
        }
    }
    const auto at = [&](std::string_view key) { return effectParameterAt(*instance, key, curveTime); };
    style.sizePx = at("size");
    style.fill = {at("fill.red"), at("fill.green"), at("fill.blue")};
    style.fillOpacity = at("fill.alpha");
    style.strokeWidth = at("stroke.width");
    style.stroke = {at("stroke.red"), at("stroke.green"), at("stroke.blue")};
    style.strokeOpacity = at("stroke.alpha");
    const double align = at("alignment");
    style.align = align < 0.5 ? render::TitleAlign::Left
                  : align < 1.5 ? render::TitleAlign::Center
                                : render::TitleAlign::Right;
    style.lineSpacing = at("lineSpacing");
    style.tracking = at("tracking");
    return style;
}

namespace {

void buildLevel(const std::vector<LayerSpec>& specs, std::vector<media::VideoFrame>& frames, std::size_t begin,
                std::size_t end, std::vector<render::CompositeLayer>& out);

/// The raster of a synthetic fill (C.15): one per size and colour, kept while recent - a matte shows for many frames,
/// and the same raster stays resident on the GPU.
std::shared_ptr<const render::TitleRaster> fillRaster(render::CanvasSize size, const std::array<std::uint8_t, 4>& rgba) {
    static std::mutex mutex;
    static std::list<std::pair<std::uint64_t, std::shared_ptr<const render::TitleRaster>>> recent;
    const std::uint64_t key = (static_cast<std::uint64_t>(size.width) << 48) | (static_cast<std::uint64_t>(size.height) << 32) |
                              (static_cast<std::uint64_t>(rgba[0]) << 24) | (static_cast<std::uint64_t>(rgba[1]) << 16) |
                              (static_cast<std::uint64_t>(rgba[2]) << 8) | static_cast<std::uint64_t>(rgba[3]);
    std::lock_guard lock(mutex);
    for (auto it = recent.begin(); it != recent.end(); ++it) {
        if (it->first == key) {
            recent.splice(recent.begin(), recent, it);
            return recent.front().second;
        }
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height) * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        std::copy(rgba.begin(), rgba.end(), pixels.begin() + static_cast<std::ptrdiff_t>(i));
    }
    recent.emplace_front(key, std::make_shared<const render::TitleRaster>(size.width, size.height, std::move(pixels),
                                                                          render::TitleLayoutInfo{}));
    if (recent.size() > 4) {
        recent.pop_back();
    }
    return recent.front().second;
}

/// The composite layer of specs[j] without its transition; nullopt when it shows nothing (a failed decode, a nested
/// clip with nothing visible inside).
std::optional<render::CompositeLayer> compositeLayerOf(const std::vector<LayerSpec>& specs,
                                                       std::vector<media::VideoFrame>& frames, std::size_t j) {
    const LayerSpec& spec = specs[j];
    render::CompositeLayer layer;
    if (spec.nested) {
        auto nested = std::make_shared<render::PreparedComposite>();
        nested->canvas = spec.nestedCanvas;
        buildLevel(specs, frames, j + 1, std::min(specs.size(), nextSibling(specs, j)), nested->layers);
        if (nested->layers.empty()) {
            return std::nullopt;
        }
        layer.nested = std::move(nested);
        layer.transform = spec.transform;
    } else if (spec.graphic) {
        // A motion graphic (C.13): its raster at the canvas item's frame size; an equal draw list reuses it.
        auto raster = render::GraphicRasterCache::shared().raster(spec.graphicKey, *spec.graphic, spec.graphicSize.width,
                                                                  spec.graphicSize.height, spec.graphicImages.get());
        layer.view = raster->view(raster);
        layer.transform = spec.transform;
    } else if (spec.fill) {
        auto raster = fillRaster(spec.fillSize, *spec.fill); // C.15 synthetic media
        layer.view = raster->view(raster);
        layer.transform = spec.transform;
    } else if (spec.title) {
        auto raster = render::TitleRenderer::shared().render(*spec.title);
        layer.view = raster->view(raster);
        layer.transform = spec.transform;
    } else if (spec.caption) {
        auto raster = render::CaptionRenderer::shared().render(*spec.caption);
        layer.view = raster->view(raster);
        layer.transform = spec.transform; // identity: the caption frame is the canvas
    } else {
        if (j >= frames.size() || !frames[j].isValid()) {
            return std::nullopt;
        }
        layer.frame = std::move(frames[j]);
        const int streamHeight =
            spec.item && spec.streamIndex >= 0 && spec.streamIndex < static_cast<int>(spec.item->info.video.size())
                ? spec.item->info.video[static_cast<std::size_t>(spec.streamIndex)].height
                : 0;
        layer.transform = transformForFrameHeight(spec.transform, streamHeight, layer.frame.height);
    }
    layer.opacity = spec.opacity;
    layer.blend = spec.blend;
    layer.effects = spec.effects;
    layer.redactions = spec.redactions;
    if (!spec.sourceRedactions.empty() && layer.frame.isValid() && layer.frame.duration.value > 0) {
        // A VFR picture can cross a marked interval even when its requested time does not. Geometry follows
        // the displayed picture's PTS, so a tracked region cannot drift while the same frame is held.
        const std::span<const ClipId> path = spec.nestPath ? std::span<const ClipId>(*spec.nestPath) : std::span<const ClipId>{};
        layer.redactions = redactionsAt(spec.sourceRedactions, spec.clip, layer.frame.pts,
                                        spec.redactionAspect, layer.frame.timeRange(), path);
    }
    layer.bypassedEffects = spec.bypassedEffects;
    layer.clipId = spec.clip.value;
    return layer;
}

/// The layers of one level, specs [begin, end) (nested subtrees become nested composites).
void buildLevel(const std::vector<LayerSpec>& specs, std::vector<media::VideoFrame>& frames, std::size_t begin,
                std::size_t end, std::vector<render::CompositeLayer>& out) {
    std::vector<std::size_t> siblings;
    std::vector<std::size_t> matteOf; // per sibling: the spec of its track matte group, or `end`
    std::size_t pendingMatte = end;
    for (std::size_t j = begin; j < end; j = nextSibling(specs, j)) {
        if (specs[j].matte) { // a Track Matte (0.1.2): not a layer; the next one's TrackMatte effect reads it
            pendingMatte = j;
            continue;
        }
        siblings.push_back(j);
        matteOf.push_back(pendingMatte);
        pendingMatte = end;
    }
    // Every layer first: whether a nested side shows anything decides how its transition mixes.
    std::vector<std::optional<render::CompositeLayer>> built;
    built.reserve(siblings.size());
    for (std::size_t k = 0; k < siblings.size(); ++k) {
        built.push_back(compositeLayerOf(specs, frames, siblings[k]));
        if (built.back() && matteOf[k] != end) {
            const std::optional<render::CompositeLayer> matte = compositeLayerOf(specs, frames, matteOf[k]);
            for (render::LayerEffect& effect : built.back()->effects) {
                if (effect.type == render::EffectType::TrackMatte && matte) {
                    effect.matte = matte->nested; // nothing visible there: null, an empty matte
                }
            }
        }
    }
    for (std::size_t k = 0; k < siblings.size(); ++k) {
        const std::optional<render::LayerTransition>& group = specs[siblings[k]].transition;
        if (group && group->layerCount == 2 && k + 1 < siblings.size()) {
            // A transition pair: a side that shows nothing leaves a one-sided mix with the other.
            const bool a = built[k].has_value();
            const bool b = built[k + 1].has_value();
            render::LayerTransition mix = *group;
            mix.layerCount = (a ? 1 : 0) + (b ? 1 : 0);
            mix.incomingOnly = !a;
            if (a) {
                built[k]->transition = mix;
                out.push_back(std::move(*built[k]));
            }
            if (b) {
                if (!a) {
                    built[k + 1]->transition = mix;
                }
                out.push_back(std::move(*built[k + 1]));
            }
            ++k;
            continue;
        }
        if (built[k]) {
            built[k]->transition = group;
            out.push_back(std::move(*built[k]));
        }
    }
}

} // namespace

void buildCompositeLayers(const std::vector<LayerSpec>& specs, std::vector<media::VideoFrame>& frames,
                          std::vector<render::CompositeLayer>& out) {
    buildLevel(specs, frames, 0, specs.size(), out);
}

const CaptionTrack* captionTrackFor(const Sequence& sequence, const CaptionSettings& settings) {
    if (settings.track) {
        for (const CaptionTrack& t : sequence.captionTracks) {
            if (t.id == *settings.track && t.enabled) {
                return &t;
            }
        }
    }
    for (const CaptionTrack& t : sequence.captionTracks) {
        if (t.enabled) {
            return &t;
        }
    }
    return nullptr;
}

render::CaptionStyle captionStyleOf(const CaptionStyle& m, Ticks time) {
    render::CaptionStyle s;
    s.fontFamily = m.fontFamily;
    s.weight = m.fontWeight;
    s.sizePx = m.fontSize * 1080.0;
    s.fill = m.fill;
    s.strokeWidth = m.outlineWidth * s.sizePx;
    s.stroke = m.outline;
    s.shadowOpacity = m.shadowOpacity;
    s.shadowColor = m.shadowColor;
    s.shadowDistance = m.shadowDistance;
    s.shadowAngle = m.shadowAngle;
    s.shadowSoftness = m.shadowSoftness;
    s.box = m.box;
    s.boxColor = m.boxColor;
    s.boxOpacity = m.boxOpacity;
    s.boxPadding = m.boxPadding;
    s.boxRadius = m.boxRadius;
    s.position = m.placement == CaptionPlacement::Top      ? render::CaptionPosition::Top
                 : m.placement == CaptionPlacement::Custom ? render::CaptionPosition::Custom
                                                           : render::CaptionPosition::Bottom;
    s.safeArea = m.safeArea;
    s.centerX = m.positionX.valueAt(time);
    s.centerY = m.positionY.valueAt(time);
    s.align = m.alignment == CaptionAlign::Left    ? render::CaptionAlign::Left
              : m.alignment == CaptionAlign::Right ? render::CaptionAlign::Right
                                                   : render::CaptionAlign::Center;
    s.maxWidth = m.maxWidth;
    s.lineSpacing = m.lineSpacing;
    s.highlight = m.highlight;
    return s;
}

namespace {

/// When each word of `text` (a cue's drawn text) starts: the transcript's word times when they line up with its
/// words, else the cue's time shared among its words by length.
std::vector<Ticks> wordStarts(const CaptionTrack& track, const CaptionCue& cue,
                              const std::vector<std::pair<int, int>>& spans, const CaptionWordTimings* words) {
    std::vector<Ticks> starts;
    if (words) {
        if (const auto it = words->cues.find({track.id.value, cue.start.value});
            it != words->cues.end() && it->second.size() == spans.size()) {
            for (const CaptionWordTiming& w : it->second) {
                starts.push_back(w.start);
            }
        }
    }
    if (starts.empty() && !spans.empty()) {
        std::int64_t letters = 0;
        for (const auto& [b, e] : spans) {
            letters += e - b;
        }
        std::int64_t before = 0;
        for (const auto& [b, e] : spans) {
            starts.push_back(cue.start + Ticks{static_cast<std::int64_t>(static_cast<long double>(cue.duration.value) *
                                                                         before / std::max<std::int64_t>(1, letters))});
            before += e - b;
        }
    }
    return starts;
}

/// UTF-16 length of UTF-8 `text` (the renderer's offsets).
int utf16Length(const std::string& text) {
    int n = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if ((c & 0xC0) != 0x80) {
            n += c >= 0xF0 ? 2 : 1; // a 4-byte sequence is a surrogate pair
        }
    }
    return n;
}

} // namespace

std::optional<render::CaptionFrame> evaluateCaption(const Sequence& sequence, Ticks time, const CaptionSettings& settings,
                                                    const ProjectState* project) {
    const CaptionTrack* track = captionTrackFor(sequence, settings);
    if (!track || track->cues.empty()) {
        return std::nullopt;
    }
    // C.13: brand colours and fonts the style names come from the project's kit (a new kit re-themes captions).
    const CaptionStyle model =
        project ? resolvedCaptionStyle(track->style, project->brandKit ? &*project->brandKit : nullptr) : track->style;
    // The cues shown, in start order: the last one starting at or before `time`, or with overlapping cues allowed
    // (stacked), every one still running among the 256 that start last before it.
    const auto after = std::upper_bound(track->cues.begin(), track->cues.end(), time,
                                        [](Ticks t, const CaptionCue& cue) { return t < cue.start; });
    std::vector<const CaptionCue*> shown;
    if (model.allowOverlap) {
        const std::ptrdiff_t scan = std::min<std::ptrdiff_t>(256, after - track->cues.begin());
        for (auto it = after - scan; it != after; ++it) {
            if (time < it->end() && !it->text.empty()) {
                shown.push_back(&*it);
            }
        }
    } else if (after != track->cues.begin()) {
        const CaptionCue& cue = *std::prev(after);
        if (time < cue.end() && !cue.text.empty()) {
            shown.push_back(&cue);
        }
    }
    if (shown.empty()) {
        return std::nullopt;
    }
    render::CaptionFrame frame;
    frame.frameWidth = sequence.width;
    frame.frameHeight = sequence.height;
    frame.style = settings.style ? *settings.style : captionStyleOf(model, time);
    const bool redactions = project && hasSpeechRedactions(*project);
    const bool singlePlain = shown.size() == 1 && !(model.showSpeakers && project && shown.front()->speaker.isValid());
    if (settings.karaoke && singlePlain) {
        frame.highlightEnd = 0;
    }
    for (std::size_t i = 0; i < shown.size(); ++i) {
        const CaptionCue& cue = *shown[i];
        if (i > 0) {
            frame.text += '\n';
        }
        int base = utf16Length(frame.text);
        if (model.showSpeakers && project && cue.speaker.isValid()) {
            if (const Speaker* speaker = project->findSpeaker(cue.speaker); speaker && !speaker->name.empty()) {
                const int nameLength = utf16Length(speaker->name);
                frame.colorSpans.push_back({base, base + nameLength,
                                            {((speaker->color >> 16) & 0xFF) / 255.0, ((speaker->color >> 8) & 0xFF) / 255.0,
                                             (speaker->color & 0xFF) / 255.0}});
                frame.text += speaker->name + ": ";
                base += nameLength + 2;
            }
        }
        std::string text = cue.text;
        if (redactions) {
            const std::vector<TimeRange> redacted = redactedSpeechRanges(*project, sequence, TimeRange{cue.start, cue.duration});
            text = redactedCueText(track->id, cue, redacted, settings.words.get(), settings.redactedWord);
        }
        frame.text += text;
        const bool byWord = model.animation == CaptionAnimation::WordByWord;
        if (!settings.karaoke && !byWord) {
            continue;
        }
        const std::vector<std::pair<int, int>> spans = render::captionWordSpans(text);
        const std::vector<Ticks> starts = wordStarts(*track, cue, spans, settings.words.get());
        int spokenEnd = 0;
        std::optional<int> firstUnspoken;
        for (std::size_t k = 0; k < spans.size() && k < starts.size(); ++k) {
            if (starts[k] <= time) {
                spokenEnd = spans[k].second;
            } else if (!firstUnspoken) {
                firstUnspoken = spans[k].first;
            }
        }
        if (settings.karaoke) {
            if (singlePlain) {
                frame.highlightEnd = base + spokenEnd;
            } else if (spokenEnd > 0) {
                frame.highlights.emplace_back(base, base + spokenEnd);
            }
        }
        if (byWord && firstUnspoken) {
            frame.hidden.emplace_back(base + *firstUnspoken, base + utf16Length(text)); // words appear as they are spoken
        }
    }
    // Pop and fade follow the cue that started last.
    const CaptionCue& latest = *shown.back();
    const double sinceStart = (time - latest.start).toSeconds();
    const double untilEnd = (latest.end() - time).toSeconds();
    if (model.animation == CaptionAnimation::Fade) {
        constexpr double kFade = 0.2;
        frame.opacity = std::clamp(std::min(sinceStart, untilEnd) / kFade, 0.0, 1.0);
    } else if (model.animation == CaptionAnimation::Pop) {
        constexpr double kPop = 0.15;
        if (sinceStart < kPop) {
            // Ease-out with a slight overshoot (easeOutBack), from 80 % to full size; it fades in over the first half.
            const double p = std::clamp(sinceStart / kPop, 0.0, 1.0) - 1.0;
            constexpr double c1 = 1.70158;
            const double eased = 1.0 + (c1 + 1.0) * p * p * p + c1 * p * p;
            frame.scale = 0.8 + 0.2 * eased;
            frame.opacity = std::clamp(sinceStart / (kPop / 2.0), 0.0, 1.0);
        }
    }
    return frame;
}

namespace {

/// What evaluating one level of video needs: the top-level sequence, or a nested one reached through `path`.
struct Level {
    const ProjectState& project;
    const PreviewSettings* preview = nullptr;
    const color::LutResolver* luts = nullptr;
    bool cullOccluded = true;
    /// The nested clips leading to this level, outermost first; null at the top level.
    std::shared_ptr<const std::vector<ClipId>> path;
    /// A key of `path` (0 at the top level), mixed into the decode keys of the level's layers.
    std::uint64_t pathKey = 0;
    std::size_t depth = 0;
    /// Clips the whole frame may still expand into layers (shared by every level; review wave 5 E5-P2-9).
    int* budget = nullptr;
    /// The top level's burned-in captions (null: none shown): nested sequences then draw their own caption track too
    /// (I.9), in their own style, at nested time.
    const CaptionSettings* captions = nullptr;

    std::span<const ClipId> pathSpan() const { return path ? std::span<const ClipId>(*path) : std::span<const ClipId>{}; }
};

/// Layers one frame may hold, nested ones included: far beyond any real timeline. Past it a clip shows nothing (a
/// crafted project of doubling nests stops here instead of expanding 2^depth layers; validate() refuses such files).
constexpr int kMaxLayersPerFrame = 4096;

/// Puts `unit` (the layers of one track, or of a whole level) into a group of its own on a `canvas`-sized canvas
/// (identity transform) carrying `effects`: a video track's effects, a sequence's master stack (I.9).
void wrapInStage(std::vector<LayerSpec>& unit, std::vector<render::LayerEffect> effects, int bypassed,
                 render::CanvasSize canvas, Ticks time, int track, const std::shared_ptr<const std::vector<ClipId>>& path) {
    LayerSpec stage;
    stage.nested = true;
    stage.nestedCanvas = canvas;
    stage.sourceTime = time;
    stage.effects = std::move(effects);
    stage.bypassedEffects = bypassed;
    stage.nestedCount = static_cast<int>(unit.size());
    stage.track = track;
    stage.nestPath = path;
    unit.insert(unit.begin(), std::move(stage));
}

/// The caption a nested sequence draws at `nestedTime` when the top level shows captions: its first enabled caption
/// track, in that track's own style (the top level's track choice and style override are the top sequence's).
std::optional<render::CaptionFrame> nestedCaption(const Level& level, const Sequence& nested, Ticks nestedTime) {
    if (!level.captions || nested.multicam || nested.captionTracks.empty()) {
        return std::nullopt;
    }
    CaptionSettings settings;
    settings.karaoke = level.captions->karaoke;
    settings.redactedWord = level.captions->redactedWord; // word timings are the top transcript's: estimated here
    return evaluateCaption(nested, nestedTime, settings, &level.project);
}

std::uint64_t mixKey(std::uint64_t key, std::uint64_t value) {
    key ^= value + 0x9E3779B97F4A7C15ull + (key << 6) + (key >> 2);
    return key * 0xFF51AFD7ED558CCDull;
}

int appendClip(const Level& level, const Clip& clip, Ticks time, int track, render::CanvasSize canvas,
               std::vector<LayerSpec>& unit);

/// A Track Matte Key on unit[head] (C.15, 0.1.2; docs/internal/c15-parity-handoff.md): marks its TrackMatte effects valid when
/// their matte track exists and is not the layer's own, and puts the matte track's clip at `time` (a group flagged
/// `matte`, whatever that track's output switch; its own track mattes are not followed) right before the layer.
/// Returns the layer's new index in `unit`.
int addTrackMatte(const Level& level, const Sequence& sequence, Ticks time, int track, render::CanvasSize canvas,
                  std::vector<LayerSpec>& unit, int head) {
    LayerSpec& spec = unit[static_cast<std::size_t>(head)];
    int number = 0; // the first instance's matte track decides
    for (render::LayerEffect& effect : spec.effects) {
        if (effect.type != render::EffectType::TrackMatte) {
            continue;
        }
        const int n = static_cast<int>(std::lround(effect.params[0]));
        number = number == 0 ? n : number;
        const bool valid = n == number && n >= 1 && n <= static_cast<int>(sequence.videoTracks.size()) && n - 1 != track &&
                           sequence.videoTracks[static_cast<std::size_t>(n - 1)];
        effect.params[3] = valid ? 1.0 : 0.0;
    }
    if (number < 1 || number > static_cast<int>(sequence.videoTracks.size()) || number - 1 == track ||
        !sequence.videoTracks[static_cast<std::size_t>(number - 1)]) {
        return head;
    }
    const Track& matteTrack = *sequence.videoTracks[static_cast<std::size_t>(number - 1)];
    const auto index = matteTrack.clipIndexAt(time);
    if (!index) {
        return head; // an empty matte
    }
    const Clip& clip = matteTrack.clips[*index];
    if (const MediaItem* item = level.project.findMedia(clip.media); item && isAdjustmentLayer(*item)) {
        return head;
    }
    std::vector<LayerSpec> matte;
    if (appendClip(level, clip, time, number - 1, canvas, matte) < 0) {
        return head;
    }
    wrapInStage(matte, {}, 0, canvas, time, number - 1, level.path);
    matte.front().matte = true;
    const auto inserted = static_cast<int>(matte.size());
    unit.insert(unit.begin() + head, std::make_move_iterator(matte.begin()), std::make_move_iterator(matte.end()));
    return head + inserted;
}

/// The decode key of a layer at a nested level: never a plain clip id (those are the top level's).
constexpr std::uint64_t kNestedDecodeKeyBit = std::uint64_t{1} << 63;

void evaluateLevel(const Level& level, const Sequence& sequence, Ticks time, std::span<const std::size_t> tracks,
                   std::vector<LayerSpec>& out);

/// Appends what `clip` (a clip of the level's sequence) shows at `time` to `unit`: one spec for a media or title clip,
/// a group and its subtree for a nested or multicam clip. Returns the index in `unit` of the clip's own (head) spec,
/// or -1 when it shows nothing.
int appendClip(const Level& level, const Clip& clip, Ticks time, int track, render::CanvasSize canvas,
               std::vector<LayerSpec>& unit) {
    if (!clip.enabled || clip.stream.kind != StreamKind::Video || clip.hasUnknownSource()) {
        return -1;
    }
    if (level.budget && --*level.budget < 0) {
        return -1; // past the frame's layer budget
    }
    if (!clip.isNested()) {
        auto spec = layerOf(level.project, clip, time, track, canvas, level.preview, level.luts, level.pathSpan());
        if (!spec) {
            return -1;
        }
        if (level.path) {
            spec->nestPath = level.path;
            spec->decodeKey = mixKey(level.pathKey, clip.id.value) | kNestedDecodeKeyBit;
        }
        unit.push_back(std::move(*spec));
        return static_cast<int>(unit.size()) - 1;
    }
    // A nested or multicam clip (C.8): the nested sequence at the clip's source time, as a group.
    const Sequence* nested = level.project.findSequence(clip.sequence);
    if (!nested || nested->width <= 0 || nested->height <= 0 || level.depth + 1 >= kMaxNestingDepth) {
        return -1;
    }
    const Ticks nestedTime = sourceTimeAround(clip, time);
    if (nestedTime.value < 0 || !(nestedTime < nested->duration)) {
        return -1; // past the nested sequence's end (it may have been shortened after the clip was cut)
    }
    const Ticks curveTime = curveTimeAround(clip, time);
    const ClipProperties& p = clip.properties();
    LayerSpec head;
    head.clip = clip.id;
    head.nested = true;
    head.nestedCanvas = {nested->width, nested->height};
    head.sourceTime = nestedTime;
    head.transform.position = {p.value(ParamId::PositionX, curveTime), p.value(ParamId::PositionY, curveTime)};
    head.transform.scale = p.value(ParamId::Scale, curveTime);
    head.transform.rotationDegrees = p.value(ParamId::Rotation, curveTime);
    head.transform.anchor = {p.value(ParamId::AnchorX, curveTime), p.value(ParamId::AnchorY, curveTime)};
    head.opacity = std::clamp(p.value(ParamId::Opacity, curveTime), 0.0, 1.0);
    head.blend = clip.blendMode();
    head.track = track;
    head.nestPath = level.path;
    if (head.transform.scale == 0.0) {
        return -1;
    }
    const double aspect = static_cast<double>(nested->width) / nested->height;
    evaluateEffects(clip, curveTime, head.effects, head.bypassedEffects, level.luts, aspect);

    auto path = std::make_shared<std::vector<ClipId>>(level.path ? *level.path : std::vector<ClipId>{});
    path->push_back(clip.id);
    Level inner{level.project, level.preview, level.luts, level.cullOccluded, std::move(path),
                mixKey(level.pathKey, clip.id.value), level.depth + 1, level.budget, level.captions};
    std::vector<LayerSpec> children;
    const std::vector<std::size_t> read = nestedTracksRead(*nested, clip); // a multicam clip: its angle's track
    evaluateLevel(inner, *nested, nestedTime, read, children);
    // The nested sequence's own captions (I.9): above its master stage, placed with the nest like its pictures.
    std::optional<render::CaptionFrame> caption = nestedCaption(level, *nested, nestedTime);
    if (children.empty() && !caption) {
        return -1;
    }
    // The nested sequence's master stage (a multicam source has none): its own group over the leaves.
    std::vector<render::LayerEffect> master;
    int masterBypassed = 0;
    if (!nested->multicam && !children.empty()) {
        evaluateEffectStack(nested->effects, nestedTime, master, masterBypassed, level.luts, aspect, std::nullopt);
    }
    if (!master.empty()) {
        wrapInStage(children, std::move(master), masterBypassed, head.nestedCanvas, nestedTime,
                    static_cast<int>(nested->videoTracks.size()), inner.path);
    } else {
        head.bypassedEffects += masterBypassed;
    }
    if (caption) {
        LayerSpec spec; // no clip, no media: the nested sequence's caption, above its tracks
        spec.caption = std::move(*caption);
        spec.track = static_cast<int>(nested->videoTracks.size());
        spec.nestPath = inner.path;
        children.push_back(std::move(spec));
    }
    const int index = static_cast<int>(unit.size());
    head.nestedCount = static_cast<int>(children.size());
    unit.push_back(std::move(head));
    for (LayerSpec& child : children) {
        unit.push_back(std::move(child));
    }
    return index;
}

/// The layers of `tracks` (indices into sequence.videoTracks, V1 first) of one level at `time`, bottom to top.
void evaluateLevel(const Level& level, const Sequence& sequence, Ticks time, std::span<const std::size_t> tracks,
                   std::vector<LayerSpec>& out) {
    const render::CanvasSize canvas{sequence.width, sequence.height};
    // Topmost track first: once an opaque layer covers the canvas, nothing below can be seen. Each track's layers (a
    // clip, a nested subtree, a transition's A then B) are one unit; units are appended bottom to top at the end.
    std::vector<std::vector<LayerSpec>> units;
    // Adjustment layers (C.15): each becomes a group over the units of the tracks below it, carrying its effects.
    struct Adjustment {
        std::size_t below = 0; ///< units[below..] are the tracks under it
        std::vector<render::LayerEffect> effects;
        int bypassed = 0;
        double opacity = 1.0;
        BlendMode blend = BlendMode::Normal;
        int track = 0;
    };
    std::vector<Adjustment> adjustments;
    for (auto it = tracks.rbegin(); it != tracks.rend(); ++it) {
        if (*it >= sequence.videoTracks.size() || !sequence.videoTracks[*it]) {
            continue;
        }
        const Track& track = *sequence.videoTracks[*it];
        const int ti = static_cast<int>(*it);
        // The track's own effects (I.9): a group over the track's layers, at the level's (timeline) time.
        std::vector<render::LayerEffect> trackEffects;
        int trackBypassed = 0;
        if (!track.effects.empty()) {
            evaluateEffectStack(track.effects, time, trackEffects, trackBypassed, level.luts,
                                static_cast<double>(canvas.width) / std::max(1, canvas.height), std::nullopt);
        }
        const auto pushUnit = [&](std::vector<LayerSpec>&& unit) {
            if (!trackEffects.empty()) {
                wrapInStage(unit, trackEffects, trackBypassed, canvas, time, ti, level.path);
            } else if (trackBypassed > 0 && !unit.empty()) {
                unit.front().bypassedEffects += trackBypassed;
            }
            units.push_back(std::move(unit));
        };

        if (const auto hit = transitionAt(track, time)) {
            const auto mix = transitionMix(*hit->transition, hit->cut, time);
            std::vector<LayerSpec> a;
            std::vector<LayerSpec> b;
            int ia = hit->outgoing ? appendClip(level, *hit->outgoing, time, ti, canvas, a) : -1;
            int ib = hit->incoming ? appendClip(level, *hit->incoming, time, ti, canvas, b) : -1;
            ia = ia >= 0 ? addTrackMatte(level, sequence, time, ti, canvas, a, ia) : ia;
            ib = ib >= 0 ? addTrackMatte(level, sequence, time, ti, canvas, b, ib) : ib;
            if (!mix) {
                // Unknown transition id: bypassed, a plain cut.
                const bool before = time < hit->cut;
                std::vector<LayerSpec>& shown = before ? a : b;
                const int head = before ? ia : ib;
                if (head >= 0 && shown[static_cast<std::size_t>(head)].opacity > 0.0) {
                    pushUnit(std::move(shown));
                }
                continue;
            }
            const int count = (ia >= 0 ? 1 : 0) + (ib >= 0 ? 1 : 0);
            if (count == 0) {
                continue;
            }
            render::LayerTransition group = *mix;
            group.layerCount = count;
            group.incomingOnly = ia < 0;
            if (ia >= 0) {
                a[static_cast<std::size_t>(ia)].transition = group;
                for (LayerSpec& s : b) {
                    a.push_back(std::move(s));
                }
                pushUnit(std::move(a));
            } else {
                b[static_cast<std::size_t>(ib)].transition = group;
                pushUnit(std::move(b));
            }
            continue; // a mix is never treated as covering the layers below
        }

        const auto index = track.clipIndexAt(time);
        if (!index) {
            continue;
        }
        const Clip& clip = track.clips[*index];
        if (const MediaItem* item = level.project.findMedia(clip.media); item && isAdjustmentLayer(*item)) {
            // An adjustment layer (C.15): no picture of its own; its effects, opacity and blend mode apply to what the
            // tracks below it composite to (wrapped after the loop).
            if (clip.enabled && clip.stream.kind == StreamKind::Video) {
                const Ticks curveTime = curveTimeAround(clip, time);
                Adjustment adjustment;
                adjustment.below = units.size();
                adjustment.opacity = std::clamp(clip.properties().value(ParamId::Opacity, curveTime), 0.0, 1.0);
                adjustment.blend = clip.blendMode();
                adjustment.track = ti;
                evaluateEffects(clip, curveTime, adjustment.effects, adjustment.bypassed, level.luts,
                                static_cast<double>(canvas.width) / std::max(1, canvas.height));
                if (adjustment.opacity > 0.0 && !adjustment.effects.empty()) {
                    adjustments.push_back(std::move(adjustment));
                }
            }
            continue;
        }
        std::vector<LayerSpec> unit;
        int head = appendClip(level, clip, time, ti, canvas, unit);
        if (head < 0) {
            continue;
        }
        head = addTrackMatte(level, sequence, time, ti, canvas, unit, head);
        const LayerSpec& spec = unit[static_cast<std::size_t>(head)];
        if (spec.opacity <= 0.0) {
            continue;
        }
        bool covers = false;
        if (level.cullOccluded && trackEffects.empty() && !spec.nested && spec.item && spec.streamIndex >= 0 &&
            spec.streamIndex < static_cast<int>(spec.item->info.video.size())) {
            // A track with effects is a group: what its effects make of the layer (a crop, a blur) may not cover.
            const VideoStreamInfo& stream = spec.item->info.video[static_cast<std::size_t>(spec.streamIndex)];
            const double par = stream.sampleAspect.isValid() ? stream.sampleAspect.toDouble() : 1.0;
            covers = spec.opacity >= 1.0 && spec.blend == BlendMode::Normal && spec.effects.empty() &&
                     stream.alpha == AlphaMode::None &&
                     coversCanvas(spec.transform, stream.width, stream.height, par, canvas);
        }
        pushUnit(std::move(unit));
        if (covers) {
            break;
        }
    }
    // The lowest adjustment layer first: a higher one then wraps the lower one's group with the rest below it.
    for (auto a = adjustments.rbegin(); a != adjustments.rend(); ++a) {
        if (a->below >= units.size()) {
            continue; // nothing under it
        }
        std::vector<LayerSpec> below; // bottom to top
        for (std::size_t u = units.size(); u-- > a->below;) {
            for (LayerSpec& s : units[u]) {
                below.push_back(std::move(s));
            }
        }
        units.resize(a->below);
        std::vector<LayerSpec> unit;
        std::vector<LayerSpec> group;
        if (a->opacity < 1.0) {
            group = below;
            unit = std::move(below); // the tracks as they are, with the adjusted copy mixed over them by the opacity
        } else {
            group = std::move(below);
        }
        wrapInStage(group, std::move(a->effects), a->bypassed, canvas, time, a->track, level.path);
        group.front().opacity = a->opacity;
        group.front().blend = a->blend;
        for (LayerSpec& s : group) {
            unit.push_back(std::move(s));
        }
        units.push_back(std::move(unit));
    }
    for (auto it = units.rbegin(); it != units.rend(); ++it) { // bottom (V1) to top; a transition group reads A, B
        for (LayerSpec& s : *it) {
            out.push_back(std::move(s));
        }
    }
}

} // namespace

void evaluateVideoLayers(const ProjectState& project, const Sequence& sequence, Ticks time,
                         std::vector<LayerSpec>& out, bool cullOccluded, const PreviewSettings* preview,
                         const CaptionSettings* captions) {
    out.clear();
    // Grades find their LUTs among the project's assets (and the built-ins); files load once (LutLibrary).
    const color::LutResolver luts = color::LutLibrary::shared().resolver(project);
    std::vector<std::size_t> tracks;
    tracks.reserve(sequence.videoTracks.size());
    for (std::size_t i = 0; i < sequence.videoTracks.size(); ++i) {
        if (sequence.videoTracks[i] && sequence.videoTracks[i]->enabled) {
            tracks.push_back(i);
        }
    }
    int budget = kMaxLayersPerFrame;
    const Level level{project, preview, &luts, cullOccluded, nullptr, 0, 0, &budget, captions};
    evaluateLevel(level, sequence, time, tracks, out);
    // The sequence's master stack (I.9): a group over every track (the burned-in caption stays above it, as delivered).
    if (!sequence.effects.empty() && !sequence.multicam && !out.empty()) {
        std::vector<render::LayerEffect> master;
        int bypassed = 0;
        const render::CanvasSize canvas{sequence.width, sequence.height};
        evaluateEffectStack(sequence.effects, time, master, bypassed, &luts,
                            static_cast<double>(canvas.width) / std::max(1, canvas.height), std::nullopt);
        if (!master.empty()) {
            wrapInStage(out, std::move(master), bypassed, canvas, time, static_cast<int>(sequence.videoTracks.size()),
                        nullptr);
        } else {
            out.front().bypassedEffects += bypassed;
        }
    }
    if (captions) {
        if (auto caption = evaluateCaption(sequence, time, *captions, &project)) {
            LayerSpec spec; // no clip, no media: drawn above every video track
            spec.caption = std::move(*caption);
            spec.track = static_cast<int>(sequence.videoTracks.size());
            out.push_back(std::move(spec));
        }
    }
}

ResolvedSource resolveSource(const MediaItem& item, const PreviewSettings& preview) {
    ResolvedSource r;
    if (item.isGenerator()) {
        return r;
    }
    const std::string* proxy = nullptr;
    int divisor = 2;
    if (preview.proxies) {
        if (const auto it = preview.proxies->entries.find(item.id.value); it != preview.proxies->entries.end() &&
                                                                          !it->second.path.empty()) {
            proxy = &it->second.path;
            divisor = std::max(1, it->second.divisor);
        }
    } else if (item.locator.proxy && !item.locator.proxy->path.empty()) {
        proxy = &item.locator.proxy->path;
        const std::string& settings = item.locator.proxy->provenance.settings;
        divisor = settings.find(" 1/4") != std::string::npos ? 4 : 2; // exporter::describeProxySettings
    }
    const bool online = !item.locator.offline && !item.locator.path.empty();
    if (proxy && (preview.useProxies || !online)) {
        r.path = proxy;
        r.proxy = true;
        r.originalOffline = !online;
        r.divisor = divisor;
        return r;
    }
    if (online) {
        r.path = &item.locator.path;
    }
    return r;
}

render::LayerTransform transformForFrameHeight(const render::LayerTransform& transform, int streamHeight,
                                               int frameHeight) {
    if (streamHeight <= 0 || frameHeight <= 0 || streamHeight == frameHeight) {
        return transform;
    }
    const double k = static_cast<double>(streamHeight) / frameHeight;
    render::LayerTransform t = transform;
    t.scale *= k;
    t.anchor = {t.anchor.x / k, t.anchor.y / k};
    return t;
}

const std::string* sourcePathOf(const MediaItem& item, bool preferProxy) {
    if (item.locator.offline) {
        return nullptr;
    }
    if (preferProxy && item.locator.proxy && !item.locator.proxy->path.empty()) {
        return &item.locator.proxy->path;
    }
    return item.locator.path.empty() ? nullptr : &item.locator.path;
}

} // namespace wabcut::playback
