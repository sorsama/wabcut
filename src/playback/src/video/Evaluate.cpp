#include <wabcut/playback/video/Evaluate.h>

#include <wabcut/color/Looks.h>
#include <wabcut/model/Animations.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/FrameScaling.h>
#include <wabcut/model/MotionOps.h>
#include <wabcut/model/RedactionMedia.h>
#include <wabcut/model/Resolve.h>
#include <wabcut/model/Synthetic.h>
#include <wabcut/model/GraphicResolve.h>
#include <wabcut/model/TitleOps.h>
#include <wabcut/model/Timecode.h>
#include <wabcut/motion/Geometry.h>
#include <wabcut/render/Caption.h>
#include <wabcut/render/EffectLibrary.h>
#include <wabcut/render/SyntheticPicture.h>

#include <QDateTime>
#include <QFileInfo>
#include <QImageReader>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace wabcut::playback {

namespace {

/// Posterize Time (0.2) is not a pass: the evaluator holds the clip's time (posterizedTime).
constexpr std::string_view kPosterizeTimeEffect = "wabcut.time.posterizeTime";

} // namespace

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

/// 0.2: what field options assume without a sequence (a progressive one at its default rate).
const Sequence& progressiveSequence() {
    static const Sequence kProgressive{};
    return kProgressive;
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

/// Display-referred (sRGB-encoded) 0..1 -> linear light: the user colours of transitions (as Compositor.cpp converts
/// the effects' colours).
double srgbToLinear(double v) {
    v = std::clamp(v, 0.0, 1.0);
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

/// The video transitions the compositor implements, by registry id (0.2: Premiere's library, EffectsLibrary.cpp).
struct TransitionMapping {
    std::string_view id;
    render::TransitionKind kind;
};
constexpr std::array<TransitionMapping, 42> kTransitions{{
    {"crossDissolve", render::TransitionKind::CrossDissolve},
    {"filmDissolve", render::TransitionKind::CrossDissolve}, // a dissolve in linear light: the compositor's own
    {"additiveDissolve", render::TransitionKind::AdditiveDissolve},
    {"nonAdditiveDissolve", render::TransitionKind::NonAdditiveDissolve},
    {"dipToBlack", render::TransitionKind::DipToColor},
    {"dipToWhite", render::TransitionKind::DipToColor},
    {"dipToColor", render::TransitionKind::DipToColor},
    {"wipe", render::TransitionKind::Wipe},
    {"irisBox", render::TransitionKind::IrisBox},
    {"irisCross", render::TransitionKind::IrisCross},
    {"irisDiamond", render::TransitionKind::IrisDiamond},
    {"irisRound", render::TransitionKind::IrisRound},
    {"pagePeel", render::TransitionKind::PagePeel},
    {"pageTurn", render::TransitionKind::PageTurn},
    {"bandSlide", render::TransitionKind::BandSlide},
    {"centerSplit", render::TransitionKind::CenterSplit},
    {"push", render::TransitionKind::Push},
    {"slide", render::TransitionKind::Slide},
    {"split", render::TransitionKind::Split},
    {"whip", render::TransitionKind::Whip},
    {"bandWipe", render::TransitionKind::BandWipe},
    {"barnDoors", render::TransitionKind::BarnDoors},
    {"checkerWipe", render::TransitionKind::CheckerWipe},
    {"checkerBoard", render::TransitionKind::CheckerBoard},
    {"clockWipe", render::TransitionKind::ClockWipe},
    {"gradientWipe", render::TransitionKind::GradientWipe},
    {"inset", render::TransitionKind::Inset},
    {"paintSplatter", render::TransitionKind::PaintSplatter},
    {"pinwheel", render::TransitionKind::Pinwheel},
    {"radialWipe", render::TransitionKind::RadialWipe},
    {"randomBlocks", render::TransitionKind::RandomBlocks},
    {"randomWipe", render::TransitionKind::RandomWipe},
    {"spiralBoxes", render::TransitionKind::SpiralBoxes},
    {"venetianBlinds", render::TransitionKind::VenetianBlinds},
    {"wedgeWipe", render::TransitionKind::WedgeWipe},
    {"zigZagBlocks", render::TransitionKind::ZigZagBlocks},
    {"crossZoom", render::TransitionKind::CrossZoom},
    {"zoomBlur", render::TransitionKind::ZoomBlur},
    {"lightLeak", render::TransitionKind::LightLeak},
    {"flash", render::TransitionKind::Flash},
    {"glitch", render::TransitionKind::Glitch},
    // P.2: the sides are frozen at the transition's ends (evaluateLevel); buildCompositeLayers adds their motion.
    {kMorphCutTransition, render::TransitionKind::FlowMorph},
}};

} // namespace

std::optional<render::LayerTransition> transitionMixAt(const Transition& tr, double progress) {
    const auto mapping = std::ranges::find_if(kTransitions, [&](const TransitionMapping& m) { return m.id == tr.effect; });
    const EffectDescriptor* d = EffectRegistry::builtin().find(tr.effect);
    if (mapping == kTransitions.end() || !d || d->kind != EffectKind::VideoTransition) {
        return std::nullopt; // unknown transition: a plain cut
    }
    // A parameter's constant, else the descriptor's default (transition parameters are constants).
    const auto param = [&](std::string_view key, double fallback = 0.0) {
        for (const EffectParam& p : tr.params) {
            if (p.key == key) {
                return p.value.valueAt(Ticks{});
            }
        }
        const EffectParamInfo* info = d->param(key);
        return info ? info->defaultValue : fallback;
    };
    render::LayerTransition mix;
    mix.kind = mapping->kind;
    if (tr.effect == "dipToWhite") {
        mix.color = {1.0, 1.0, 1.0};
    } else if (tr.effect == "dipToColor" || tr.effect == "lightLeak" || tr.effect == "flash") {
        mix.color = {srgbToLinear(param("color.r")), srgbToLinear(param("color.g")), srgbToLinear(param("color.b"))};
    }
    mix.angleDegrees = param("angle");
    mix.feather = std::clamp(param("feather"), 0.0, 1.0);
    mix.borderWidth = std::max(0.0, param("borderWidth"));
    mix.borderColor = {srgbToLinear(param("borderColor.r")), srgbToLinear(param("borderColor.g")),
                       srgbToLinear(param("borderColor.b"))};
    mix.reverse = param("reverse") > 0.5;
    mix.antiAliasing = std::clamp(static_cast<int>(std::lround(param("antiAliasing"))), 0, 3);
    mix.center = {std::clamp(param("center.x", 0.5), 0.0, 1.0), std::clamp(param("center.y", 0.5), 0.0, 1.0)};
    mix.direction = std::max(0, static_cast<int>(std::lround(param("direction"))));
    // Per kind (render/reference/Transitions.h): bands, cells, wedges, the gradient, amounts.
    if (d->param("bands")) {
        mix.custom[0] = param("bands");
    } else if (d->param("cells.x")) {
        mix.custom = {param("cells.x"), param("cells.y"), 0.0, 0.0};
    } else if (d->param("wedges")) {
        mix.custom[0] = param("wedges");
    } else if (d->param("gradient")) {
        mix.custom = {param("gradient"), param("softness"), 0.0, 0.0};
    } else if (d->param("intensity")) {
        mix.custom[0] = param("intensity");
    } else if (d->param("amount")) {
        mix.custom = {param("amount"), param("colorSplit"), 0.0, 0.0};
    }
    mix.seed = static_cast<std::uint32_t>(tr.id.value & 0xFFFFFFu);
    // Start and End: where the transition begins and ends (Premiere's sliders).
    const double start = std::clamp(param("start", 0.0), 0.0, 1.0);
    const double end = std::clamp(param("end", 1.0), 0.0, 1.0);
    mix.progress = std::clamp(start + (end - start) * std::clamp(progress, 0.0, 1.0), 0.0, 1.0);
    return mix;
}

namespace {

double easeOut(double u) {
    const double v = 1.0 - std::clamp(u, 0.0, 1.0);
    return 1.0 - v * v * v;
}

double smooth(double u) {
    u = std::clamp(u, 0.0, 1.0);
    return u * u * (3.0 - 2.0 * u);
}

/// A curve through (u, value) keys: eased out of the first key, smooth between the others (the Animations presets'
/// keys, EffectPresets.cpp, over 0..1).
double keyed(std::span<const std::pair<double, double>> keys, double u) {
    if (u <= keys.front().first) {
        return keys.front().second;
    }
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (u <= keys[i].first) {
            const auto [u0, v0] = keys[i - 1];
            const auto [u1, v1] = keys[i];
            const double x = (u - u0) / (u1 - u0);
            return v0 + (v1 - v0) * (i == 1 ? easeOut(x) : smooth(x));
        }
    }
    return keys.back().second;
}

render::LayerEffect libraryEffect(render::LibraryOp op, std::initializer_list<double> params) {
    render::LayerEffect effect;
    effect.type = render::EffectType::Library;
    effect.op = static_cast<std::uint16_t>(op);
    std::copy(params.begin(), params.end(), effect.params.begin());
    return effect;
}

/// Basic 3D by `swivel` and `tilt` (degrees), and with a `hinge` edge (a point of the frame relative to its centre, in
/// frame pixels) a Transform that brings that edge's middle back to where it was: the shader's projection
/// (fx_distort.frag basic3d: focal 1.6 x the frame's longer side, Distance to Image 0).
void addBasic3D(LayerAnimation& a, double swivel, double tilt, render::CanvasSize frame, std::optional<render::Vec2> hinge) {
    a.effects.push_back(libraryEffect(render::LibraryOp::Basic3D, {swivel, tilt, 0.0, 0.0, 0.0}));
    if (!hinge) {
        return;
    }
    const double w = std::max(1, frame.width);
    const double h = std::max(1, frame.height);
    const double focal = 1.6 * std::max(w, h);
    const double s = swivel * std::numbers::pi / 180.0;
    const double t = tilt * std::numbers::pi / 180.0;
    // The hinge point on the rotated plane (render/reference LibraryDistort.cpp basic3d: ex, ey), projected.
    const double ex[3] = {std::cos(s), 0.0, -std::sin(s)};
    const double ey[3] = {std::sin(t) * std::sin(s), std::cos(t), std::sin(t) * std::cos(s)};
    const double px = hinge->x * ex[0] + hinge->y * ey[0];
    const double py = hinge->x * ex[1] + hinge->y * ey[1];
    const double pz = hinge->x * ex[2] + hinge->y * ey[2];
    const double k = focal / std::max(focal * 1e-3, focal + pz);
    const double dx = hinge->x - px * k;
    const double dy = hinge->y - py * k;
    // Transform: anchor and position (fractions of the frame), uniform scale 100 %, no skew or rotation, opaque, the
    // composition's shutter (no motion blur), bilinear.
    a.effects.push_back(libraryEffect(render::LibraryOp::Transform,
                                      {0.5, 0.5, 0.5 + dx / w, 0.5 + dy / h, 1.0, 100.0, 100.0, 0.0, 0.0, 0.0, 1.0, 1.0,
                                       0.0, 0.0}));
}

} // namespace

std::optional<LayerAnimation> transitionAnimationAt(const Transition& tr, double progress, bool out,
                                                    render::CanvasSize canvas, render::CanvasSize frame) {
    if (!isAnimationTransition(tr.effect)) {
        return std::nullopt;
    }
    std::string effect = tr.effect;
    std::vector<EffectParam> params = tr.params;
    if (effect == kSurpriseAnimation) {
        PickedAnimation picked = surpriseAnimation(tr.id.value, std::move(params));
        effect = std::move(picked.effect);
        params = std::move(picked.params);
    }
    const EffectDescriptor* d = EffectRegistry::builtin().find(effect);
    if (!d) {
        return std::nullopt;
    }
    const auto param = [&](std::string_view key, double fallback = 0.0) {
        for (const EffectParam& p : params) {
            if (p.key == key) {
                return p.value.valueAt(Ticks{});
            }
        }
        const EffectParamInfo* info = d->param(key);
        return info ? info->defaultValue : fallback;
    };
    const double start = std::clamp(param("start", 0.0), 0.0, 1.0);
    const double end = std::clamp(param("end", 1.0), 0.0, 1.0);
    const double p = std::clamp(start + (end - start) * std::clamp(progress, 0.0, 1.0), 0.0, 1.0);
    // u: 1 = at rest. Out animations are the In ones backwards; turns (Spin, Flip) carry on the same way.
    const double u = out ? 1.0 - p : p;
    const double e = easeOut(u);
    const double turn = out ? 1.0 : -1.0;
    const double w = std::max(1, canvas.width);
    const double h = std::max(1, canvas.height);
    const auto option = [&](std::string_view key) { return static_cast<int>(std::lround(param(key))); };
    LayerAnimation a;
    if (effect == "animationFade") {
        a.opacity = u;
    } else if (effect == "animationPop") {
        static constexpr std::array<std::pair<double, double>, 3> kScale{{{0.0, 0.0}, {0.64, 1.1}, {1.0, 1.0}}};
        a.scale = keyed(kScale, u);
        a.opacity = std::clamp(u / 0.24, 0.0, 1.0);
    } else if (effect == "animationSpin") {
        a.scale = e;
        a.rotationDegrees = (option("turn") == 1 ? -1.0 : 1.0) * turn * 180.0 * (1.0 - e);
    } else if (effect == "animationPull") {
        a.scale = 1.0 + 0.6 * (1.0 - e);
        a.opacity = std::clamp(u / 0.6, 0.0, 1.0);
    } else if (effect == "animationTravel") {
        static constexpr std::array<render::Vec2, 4> kSides{{{-1.0, 0.0}, {1.0, 0.0}, {0.0, -1.0}, {0.0, 1.0}}};
        const render::Vec2 side = kSides[static_cast<std::size_t>(std::clamp(option("side"), 0, 3))];
        a.offset = {side.x * w * (1.0 - e), side.y * h * (1.0 - e)};
    } else if (effect == "animationSpring") {
        static constexpr std::array<std::pair<double, double>, 4> kRise{
            {{0.0, 1.0}, {0.545, -0.06}, {0.788, 0.025}, {1.0, 0.0}}};
        a.offset = {0.0, keyed(kRise, u) * h};
    } else if (effect == "animationFlip") {
        const double angle = turn * 90.0 * (1.0 - e);
        if (option("axis") == 1) {
            addBasic3D(a, 0.0, angle, frame, std::nullopt);
        } else {
            addBasic3D(a, angle, 0.0, frame, std::nullopt);
        }
    } else if (effect == "animationFold") {
        // Folded away from the camera about the hinge edge at u = 0, flat at rest.
        const double angle = 90.0 * (1.0 - e);
        const double fw = std::max(1, frame.width) * 0.5;
        const double fh = std::max(1, frame.height) * 0.5;
        switch (std::clamp(option("hinge"), 0, 3)) {
        case 0: addBasic3D(a, 0.0, angle, frame, render::Vec2{0.0, -fh}); break;  // top
        case 1: addBasic3D(a, 0.0, -angle, frame, render::Vec2{0.0, fh}); break;  // bottom
        case 2: addBasic3D(a, -angle, 0.0, frame, render::Vec2{-fw, 0.0}); break; // left
        default: addBasic3D(a, angle, 0.0, frame, render::Vec2{fw, 0.0}); break;  // right
        }
    } else if (effect == "animationBlock") {
        const double size = std::max(2.0, param("blockSize", 64.0));
        a.effects.push_back(libraryEffect(render::LibraryOp::BlockDissolve, {1.0 - u, size, size, 0.0, 0.0}));
    }
    return a;
}

namespace {

std::optional<render::LayerTransition> transitionMix(const Transition& tr, Ticks cut, Ticks time) {
    const Ticks length = tr.before + tr.after;
    const double progress = length.value > 0 ? static_cast<double>((time - (cut - tr.before)).value) /
                                                   static_cast<double>(length.value)
                                             : 1.0;
    return transitionMixAt(tr, progress);
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

/// Posterize Time (0.2): "wabcut.time.posterizeTime" plays the clip at its own Frame Rate - the timeline time held on
/// steps of 1 / rate from the clip's start (the picture and every effect of the clip follow). Its first enabled
/// instance decides; `time` itself without one.
Ticks posterizedTime(const Clip& clip, Ticks time) {
    for (const EffectInstance& e : clip.effects()) {
        if (!e.enabled || e.effect != kPosterizeTimeEffect) {
            continue;
        }
        const double rate = effectParameterAt(e, "frameRate", curveTimeAround(clip, time));
        const Ticks since = time - clip.start();
        if (!(rate > 0.0) || since.value <= 0) {
            return time;
        }
        const double step = static_cast<double>(kTicksPerSecond) / rate;
        const double held = std::floor(static_cast<double>(since.value) / step + 1e-9) * step;
        return clip.start() + Ticks{static_cast<std::int64_t>(std::llround(held))};
    }
    return time;
}

/// The last path component of a UTF-8 path ("/" or "\" separated).
std::string fileNameOf(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// What `clip`'s time and text effects read at timeline time `time` of `sequence` (null: no frame rate, no Timecode).
EffectContext effectContextOf(const ProjectState& project, const Sequence* sequence, const Clip& clip, const MediaItem* item,
                              Ticks time, render::CanvasSize canvas) {
    EffectContext c;
    if (sequence) {
        c.frameRate = sequence->frameRate;
        c.dropFrame = sequence->dropFrame;
    }
    c.canvas = canvas;
    c.clipTime = Ticks{std::max<std::int64_t>(0, (time - clip.start()).value)};
    c.sourceTime = sourceTimeAround(clip, time);
    c.clipName = clip.name();
    if (item) {
        c.mediaStartTimecode = item->info.startTimecode;
        c.projectClipName = item->name;
        c.fileName = item->locator.path.empty() ? item->name : fileNameOf(item->locator.path);
    } else {
        c.projectClipName = clip.name();
        c.fileName = clip.name();
    }
    c.project = &project;
    return c;
}

/// The context of a track's or a sequence's own effects (I.9) at `time`: the sequence's time and name.
EffectContext stageContextOf(const ProjectState& project, const Sequence& sequence, Ticks time, render::CanvasSize canvas) {
    EffectContext c;
    c.frameRate = sequence.frameRate;
    c.dropFrame = sequence.dropFrame;
    c.canvas = canvas;
    c.clipTime = time;
    c.sourceTime = time;
    c.mediaStartTimecode = sequence.startTimecode;
    c.clipName = sequence.name;
    c.projectClipName = sequence.name;
    c.fileName = sequence.name;
    c.project = &project;
    return c;
}

/// The layer of `clip` (a media, title or graphic clip) at `time` (inside or around its range); nullopt when it shows
/// nothing.
/// `path`: the nested clips leading to it (C.8). `sequence`: the clip's sequence (the time effects' frame rate; P.2:
/// Time Interpolation applies to footage of another rate than it). With `interpolate`, a retimed media clip with Frame
/// Blending or Optical Flow gets LayerSpec::interpolation (appendClip turns it into its group).
std::optional<LayerSpec> layerOf(const ProjectState& project, const Clip& clip, Ticks time, int track,
                                 render::CanvasSize canvas, const PreviewSettings* preview, const color::LutResolver* luts,
                                 std::span<const ClipId> path, const Sequence* sequence = nullptr, bool interpolate = false) {
    if (!clip.enabled || clip.stream.kind != StreamKind::Video) {
        return std::nullopt;
    }
    const MediaItem* item = project.findMedia(clip.media);
    const EffectContext context = effectContextOf(project, sequence, clip, item, time, canvas);
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
        spec.transform.scale = p.value(ParamId::Scale, curveTime) *
                               scaleToFrameFactor(project, clip, canvas.width, canvas.height); // P.2: Scale to Frame Size
        spec.transform.rotationDegrees = p.value(ParamId::Rotation, curveTime);
        spec.transform.anchor = {p.value(ParamId::AnchorX, curveTime), p.value(ParamId::AnchorY, curveTime)};
        spec.opacity = std::clamp(p.value(ParamId::Opacity, curveTime), 0.0, 1.0);
        spec.blend = clip.blendMode();
        spec.track = track;
        if (spec.transform.scale == 0.0 || item->generator->text.empty()) {
            return std::nullopt;
        }
        evaluateEffects(clip, curveTime, spec.effects, spec.bypassedEffects, luts,
                        static_cast<double>(canvas.width) / std::max(1, canvas.height), std::nullopt, &context);
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
        spec.transform.scale = p.value(ParamId::Scale, curveTime) *
                               scaleToFrameFactor(project, clip, canvas.width, canvas.height); // P.2: Scale to Frame Size
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
                        static_cast<double>(canvas.width) / std::max(1, canvas.height), std::nullopt, &context);
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
        spec.transform.scale = p.value(ParamId::Scale, curveTime) *
                               scaleToFrameFactor(project, clip, canvas.width, canvas.height); // P.2: Scale to Frame Size
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
        case SyntheticKind::BarsAndTone:
        case SyntheticKind::CountingLeader:
            // P.2: drawn by render::syntheticPicture at the media time shown (the leader counts down with it).
            spec.syntheticPicture = true;
            spec.sourceTime = synthetic->kind == SyntheticKind::CountingLeader ? sourceTimeAround(clip, time) : Ticks{};
            break;
        }
        const VideoStreamInfo& frame = item->info.video.front();
        spec.fillSize = {frame.width > 0 ? frame.width : canvas.width, frame.height > 0 ? frame.height : canvas.height};
        evaluateEffects(clip, curveTime, spec.effects, spec.bypassedEffects, luts,
                        static_cast<double>(canvas.width) / std::max(1, canvas.height), std::nullopt, &context);
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
    spec.transform.scale = p.value(ParamId::Scale, curveTime) *
                           scaleToFrameFactor(project, clip, canvas.width, canvas.height); // P.2: Scale to Frame Size
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
    evaluateEffects(clip, curveTime, spec.effects, spec.bypassedEffects, luts, aspect, spec.sourceTime, &context);
    spec.redactions = evaluateRedactions(project, clip, spec.sourceTime, aspect, stream.frameRate, path);
    spec.sourceRedactions = redactionsCovering(project, clip.media);
    spec.redactionAspect = aspect;
    if (interpolate && clip.timeInterpolation() != TimeInterpolation::FrameSampling &&
        retimesFrames(clip, *item, sequence ? sequence->frameRate : Rate{})) {
        spec.interpolation = clip.timeInterpolation(); // P.2: appendClip makes the group
    }
    spec.inputSpace = clip.inputColorSpace();
    const ColorManagement& management = project.colorManagement;
    if (!spec.inputSpace && management.autoDetectLog && management.mode != ColorManagementMode::Disabled) {
        spec.inputSpace = detectedLogSpace(stream); // 0.2: the camera log the media's metadata names
    }
    spec.fields = fieldRenderingOf(clip, item, clip.stream.index, sequence ? *sequence : progressiveSequence());
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
                         int& bypassed, const color::LutResolver* luts, double frameAspect, std::optional<Ticks> mediaTime,
                         const EffectContext* context = nullptr);

} // namespace

void evaluateEffects(const Clip& clip, Ticks curveTime, std::vector<render::LayerEffect>& out, int& bypassed,
                     const color::LutResolver* luts, double frameAspect, std::optional<Ticks> mediaTime,
                     const EffectContext* context) {
    evaluateEffectStack(clip.effects(), curveTime, out, bypassed, luts, frameAspect, mediaTime, context);
}

namespace {

/// Strobe Light (0.2): whether it strobes at `seconds` of curve time: for Strobe Duration (params[4]) at the start of
/// every Strobe Period (params[5]), and in any other duration-long slot with Random Strobe Probability (params[6]), the
/// slot's draw seeded by Random Seed (params[9]).
bool strobeOn(const std::array<double, 32>& params, double seconds) {
    const double duration = std::max(0.0, params[4]);
    const double period = std::max(0.001, params[5]);
    const double phase = seconds - std::floor(seconds / period) * period;
    if (phase < duration) {
        return true;
    }
    if (params[6] <= 0.0 || duration <= 0.0) {
        return false;
    }
    std::uint64_t z = static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(seconds / duration))) +
                      static_cast<std::uint64_t>(std::max(0.0, params[9])) * 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<double>(z >> 11) * 0x1.0p-53 < params[6];
}

// ---- 0.2: what the time and text effects need beyond their parameters ----

/// One picture on a canvas of `canvas`'s size: the second picture of a library effect (text, a matte image).
std::shared_ptr<const render::PreparedComposite> pictureComposite(std::shared_ptr<const render::TitleRaster> raster,
                                                                  render::CanvasSize canvas) {
    auto composite = std::make_shared<render::PreparedComposite>();
    composite->canvas = canvas;
    render::CompositeLayer layer;
    layer.view = raster->view(raster);
    composite->layers.push_back(std::move(layer));
    return composite;
}

/// The text of Timecode (its parameters in render/EffectLibrary.h order): the clip's time, its media's timecode or a
/// generated one, in SMPTE, frames or feet + frames, at the sequence's rate or the chosen one, plus Offset frames.
std::string timecodeText(const render::LayerEffect& e, const EffectContext& c) {
    const auto& q = e.params;
    const int format = static_cast<int>(std::lround(q[5]));
    const int source = static_cast<int>(std::lround(q[6]));
    const int display = static_cast<int>(std::lround(q[7]));
    constexpr std::array<std::int32_t, 6> kRates{0, 24, 25, 30, 50, 60};
    const Rate rate = display > 0 && display < 6 ? Rate{kRates[static_cast<std::size_t>(display)], 1} : c.frameRate;
    const bool dropFrame = display == 0 && c.dropFrame;
    if (!rate.isValid()) {
        return {};
    }
    const Ticks time = source == 1 ? c.sourceTime + c.mediaStartTimecode.value_or(Ticks{}) : c.clipTime;
    std::int64_t frames = ticksToFrames(time, rate) + std::llround(q[8]);
    if (source == 2) {
        frames += std::llround(q[9]); // Generate: from Starting Timecode
    }
    frames = std::max<std::int64_t>(0, frames);
    std::string text;
    if (format == 1) {
        text = std::to_string(frames);
    } else if (format == 2 || format == 3) {
        const std::int64_t perFoot = format == 2 ? 40 : 16; // 16 mm, 35 mm
        const std::int64_t rest = frames % perFoot;
        text = std::to_string(frames / perFoot) + "+" + (rest < 10 ? "0" : "") + std::to_string(rest);
    } else {
        text = framesToTimecode(frames, rate, dropFrame);
    }
    const int label = static_cast<int>(std::lround(q[10]));
    if (label >= 2) {
        text = "CAM" + std::to_string(label - 1) + " " + text;
    }
    return text;
}

/// The text effects' picture (Timecode, Clip Name, Simple Text): the line on a box, centred on Position (fractions of the
/// canvas), Size percent of a fifth of the frame height, the box at Opacity (Simple Text: the text at Opacity, its box at
/// Background Opacity).
std::shared_ptr<const render::PreparedComposite> textPicture(const EffectInstance& instance, const render::LayerEffect& e,
                                                             const EffectContext& c) {
    const auto& q = e.params;
    render::CaptionFrame frame;
    frame.frameWidth = c.canvas.width;
    frame.frameHeight = c.canvas.height;
    render::CaptionStyle& style = frame.style;
    style.position = render::CaptionPosition::Custom;
    style.centerX = q[0];
    style.centerY = q[1];
    style.safeArea = 0.0;
    style.maxWidth = 1.0;
    style.boxPadding = 0.2;
    style.boxRadius = 0.0;
    style.boxColor = {0.0, 0.0, 0.0};
    const bool timecode = instance.effect == "wabcut.video.timecode";
    const bool simple = instance.effect == "wabcut.video.simpleText";
    // Caption sizes are pixels of a 1080-line frame (they scale with the canvas): Size 15 % = a 65 px line.
    const double size = timecode ? q[2] : q[3];
    style.sizePx = std::max(1.0, size / 100.0 * 1080.0 * 0.4);
    if (!timecode) {
        const int align = static_cast<int>(std::lround(q[2]));
        style.align = align == 0 ? render::CaptionAlign::Left : align == 2 ? render::CaptionAlign::Right : render::CaptionAlign::Center;
    }
    if (timecode) {
        frame.text = timecodeText(e, c);
        style.boxOpacity = std::clamp(q[3], 0.0, 1.0);
        style.fontFamily = "Consolas";
    } else if (simple) {
        const EffectData* text = instance.findData("text");
        frame.text = text && text->text() ? *text->text() : std::string("Simple Text");
        frame.opacity = std::clamp(q[4], 0.0, 1.0);
        style.boxOpacity = std::clamp(q[5], 0.0, 1.0);
        style.box = style.boxOpacity > 0.0;
    } else {
        const int display = static_cast<int>(std::lround(q[5]));
        frame.text = display == 1 ? c.projectClipName : display == 2 ? c.fileName : c.clipName;
        style.boxOpacity = std::clamp(q[4], 0.0, 1.0);
    }
    if (frame.text.empty() || !c.canvas.isValid()) {
        return nullptr;
    }
    return pictureComposite(render::CaptionRenderer::shared().render(frame), c.canvas);
}

/// Image Matte Key's matte: its image item ("media:<id>" in the effect's data "image"), a still picture stretched to the
/// canvas (premultiplied), kept while recent.
std::shared_ptr<const render::PreparedComposite> matteImage(const EffectInstance& instance, const EffectContext& c) {
    const EffectData* data = instance.findData("image");
    const std::string* text = data ? data->text() : nullptr;
    if (!text || !text->starts_with("media:") || !c.project || !c.canvas.isValid()) {
        return nullptr;
    }
    std::uint64_t id = 0;
    for (const char ch : text->substr(6)) {
        if (ch < '0' || ch > '9' || id > (UINT64_MAX - 9) / 10) {
            return nullptr;
        }
        id = id * 10 + static_cast<std::uint64_t>(ch - '0');
    }
    const MediaItem* item = c.project->findMedia(MediaId{id});
    if (!item || item->isGenerator() || item->locator.offline || item->locator.path.empty()) {
        return nullptr;
    }
    static std::mutex mutex;
    static std::list<std::pair<std::string, std::shared_ptr<const render::TitleRaster>>> recent;
    const std::string key = item->locator.path + "|" + std::to_string(c.canvas.width) + "x" + std::to_string(c.canvas.height);
    {
        std::lock_guard lock(mutex);
        for (auto it = recent.begin(); it != recent.end(); ++it) {
            if (it->first == key) {
                recent.splice(recent.begin(), recent, it);
                return pictureComposite(recent.front().second, c.canvas);
            }
        }
    }
    const QImage picture = FilePictures({{item->id, item->locator.path}}).picture(item->id);
    if (picture.isNull()) {
        return nullptr;
    }
    const QImage scaled = picture.scaled(c.canvas.width, c.canvas.height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(c.canvas.width) * static_cast<std::size_t>(c.canvas.height) * 4);
    for (int y = 0; y < c.canvas.height; ++y) {
        std::memcpy(pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(c.canvas.width) * 4,
                    scaled.constScanLine(y), static_cast<std::size_t>(c.canvas.width) * 4);
    }
    auto raster = std::make_shared<const render::TitleRaster>(c.canvas.width, c.canvas.height, std::move(pixels),
                                                              render::TitleLayoutInfo{});
    std::lock_guard lock(mutex);
    recent.emplace_front(key, raster);
    if (recent.size() > 4) {
        recent.pop_back();
    }
    return pictureComposite(raster, c.canvas);
}

void evaluateEffectStack(std::span<const EffectInstance> stack, Ticks curveTime, std::vector<render::LayerEffect>& out,
                         int& bypassed, const color::LutResolver* luts, double frameAspect, std::optional<Ticks> mediaTime,
                         const EffectContext* context) {
    out.clear();
    bypassed = 0;
    for (const EffectInstance& instance : stack) {
        if (!instance.enabled) {
            continue;
        }
        if (instance.effect == kWarpStabilizerEffect || instance.effect == kRollingShutterRepairEffect) {
            continue; // P.2: a warp of the source picture: the clip's stabilizer group draws it (appendClip)
        }
        if (instance.effect == kPosterizeTimeEffect) {
            continue; // 0.2: the evaluator plays the clip at its rate (posterizedTime)
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
        if (const render::EffectLibraryEntry* library = render::findLibraryEffect(instance.effect)) {
            // 0.2: Premiere's effect library (render/EffectLibrary.h).
            if (!isEvaluable(instance, EffectKind::Video)) {
                ++bypassed;
                continue;
            }
            render::LayerEffect effect;
            effect.type = render::EffectType::Library;
            effect.op = static_cast<std::uint16_t>(library->op);
            for (std::size_t k = 0; k < library->keys.size() && !library->keys[k].empty(); ++k) {
                effect.params[k] = effectParameterAt(instance, library->keys[k], curveTime);
            }
            const double seconds = curveTime.toSeconds();
            if (library->needsTime) {
                effect.params[render::kLibraryTimeParam] = seconds;
            }
            if (library->op == render::LibraryOp::StrobeLight) {
                effect.params[10] = strobeOn(effect.params, seconds) ? 1.0 : 0.0;
            }
            if (library->op == render::LibraryOp::Transform && context && context->frameRate.isValid() &&
                effect.params[11] < 0.5 && effect.params[12] > 0.0) {
                // Motion blur: the transform at the shutter's end, Shutter Angle / 360 of a frame later.
                const Ticks frame = framesToTicks(1, context->frameRate);
                const Ticks end =
                    curveTime + Ticks{static_cast<std::int64_t>(std::llround(static_cast<double>(frame.value) * effect.params[12] / 360.0))};
                constexpr std::array<std::size_t, 9> kMoving{0, 1, 2, 3, 5, 6, 7, 8, 9};
                for (std::size_t i = 0; i < kMoving.size(); ++i) {
                    effect.params[14 + i] = effectParameterAt(instance, library->keys[kMoving[i]], end);
                }
                effect.params[23] = 1.0;
            }
            if (library->op == render::LibraryOp::TextOverlay && context) {
                effect.matte = textPicture(instance, effect, *context);
            } else if (library->op == render::LibraryOp::ImageMatteKey && context) {
                effect.matte = matteImage(instance, *context);
            }
            effect.mask = render::maskSpecAt(instance.masks, curveTime, frameAspect, render::MatteUse::Effect, mediaTime);
            out.push_back(std::move(effect));
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

/// P.2: the pixel aspect a media layer's frames show with: Interpret Footage's when set (EditingOps.h), else the
/// stream's.
double pixelAspectOf(const MediaItem& item, const VideoStreamInfo& stream) {
    if (item.interpretation && item.interpretation->pixelAspect && item.interpretation->pixelAspect->isValid()) {
        return item.interpretation->pixelAspect->toDouble();
    }
    return stream.sampleAspect.isValid() ? stream.sampleAspect.toDouble() : 1.0;
}

/// P.2: Interpret Footage on a media layer's decoded frame - the pixel aspect and the alpha channel as the user
/// conformed them. Every layer that shows an item's frame takes it: the plain layer, and the frames inside Time
/// Interpolation's and the stabilizers' groups.
void applyInterpretation(const MediaItem* item, render::CompositeLayer& layer) {
    if (!item || !item->interpretation) {
        return;
    }
    const MediaInterpretation& in = *item->interpretation;
    if (in.pixelAspect) {
        layer.frame.sampleAspect = *in.pixelAspect;
    }
    if (in.premultiplication && (layer.frame.alpha != AlphaMode::None || media::pixelFormatHasAlpha(layer.frame.format))) {
        layer.frame.alpha = *in.premultiplication;
    }
    layer.ignoreAlpha = in.ignoreAlpha;
    layer.invertAlpha = in.invertAlpha;
}

} // namespace

double interpolationWeight(Ticks exact, const media::VideoFrame& a, const media::VideoFrame& b) {
    if (!a.isValid() || !b.isValid() || !(a.pts < b.pts)) {
        return 0.0;
    }
    const double w = static_cast<double>((exact - a.pts).value) / static_cast<double>((b.pts - a.pts).value);
    constexpr double kSnap = 1.0 / 1024.0;
    return w < kSnap ? 0.0 : w > 1.0 - kSnap ? 1.0 : w;
}

std::optional<MotionPicture> motionPictureOf(const LayerSpec& spec, Ticks time, bool placed) {
    if (!needsDecode(spec) || !spec.item || spec.streamIndex < 0 ||
        spec.streamIndex >= static_cast<int>(spec.item->info.video.size())) {
        return std::nullopt;
    }
    const MediaItem& item = *spec.item;
    const std::string* path = sourcePathOf(item, false);
    if (!path && item.locator.proxy && !item.locator.proxy->path.empty()) {
        path = &item.locator.proxy->path; // an offline original: its proxy (export refuses such media anyway)
    }
    if (!path) {
        return std::nullopt;
    }
    const VideoStreamInfo& stream = item.info.video[static_cast<std::size_t>(spec.streamIndex)];
    MotionPicture p;
    p.path = *path;
    p.stream = spec.containerStream;
    p.time = time;
    p.frameWidth = stream.width;
    p.frameHeight = stream.height;
    p.pixelAspect = pixelAspectOf(item, stream);
    if (placed) {
        p.transform = spec.transform;
    }
    const SourceFingerprint& f = item.locator.fingerprint;
    std::uint64_t id = static_cast<std::uint64_t>(f.size) * 0x9E3779B97F4A7C15ull ^ static_cast<std::uint64_t>(f.mtimeNs);
    for (const char c : f.sampleHash) {
        id = (id ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
    }
    p.identity = id;
    return p;
}

namespace {

void buildLevel(const std::vector<LayerSpec>& specs, std::vector<media::VideoFrame>& frames, std::size_t begin,
                std::size_t end, std::vector<render::CompositeLayer>& out, MotionSource* motion, render::CanvasSize canvas);

/// The displayed size of a layer's stream: what an interpolation group's canvas is (its stored pixels stretched by the
/// pixel aspect, Interpret Footage's when set).
render::CanvasSize displayedSize(const MediaItem& item, const VideoStreamInfo& stream) {
    const double par = pixelAspectOf(item, stream);
    return {std::max(1, static_cast<int>(std::lround(stream.width * par))), std::max(1, stream.height)};
}

/// The frame a layer's distort effects see (Basic 3D's focal length): a nested group's canvas, a graphic's raster, a
/// fill's size, a stream's displayed size; titles and captions draw on the canvas.
render::CanvasSize layerFrameOf(const LayerSpec& spec, render::CanvasSize canvas) {
    if (spec.nested && spec.nestedCanvas.isValid()) {
        return spec.nestedCanvas;
    }
    if (spec.graphic && spec.graphicSize.isValid()) {
        return spec.graphicSize;
    }
    if ((spec.fill || spec.syntheticPicture) && spec.fillSize.isValid()) {
        return spec.fillSize;
    }
    if (spec.item && spec.streamIndex >= 0 && spec.streamIndex < static_cast<int>(spec.item->info.video.size())) {
        return displayedSize(*spec.item, spec.item->info.video[static_cast<std::size_t>(spec.streamIndex)]);
    }
    return canvas;
}

/// The motion an interpolation group (specs[j], interpolation set) needs with its frames: Optical Flow between two
/// distinct frames that the source time lies strictly between.
std::optional<MotionRequest> interpolationRequest(const std::vector<LayerSpec>& specs,
                                                  const std::vector<media::VideoFrame>& frames, std::size_t j) {
    const LayerSpec& head = specs[j];
    if (head.interpolation != TimeInterpolation::OpticalFlow || j + 2 >= specs.size() || j + 2 >= frames.size()) {
        return std::nullopt;
    }
    const media::VideoFrame& a = frames[j + 1];
    const media::VideoFrame& b = frames[j + 2];
    const double w = interpolationWeight(head.sourceTime, a, b);
    if (w <= 0.0 || w >= 1.0) {
        return std::nullopt;
    }
    auto pa = motionPictureOf(specs[j + 1], a.pts, false);
    auto pb = motionPictureOf(specs[j + 2], b.pts, false);
    if (!pa || !pb) {
        return std::nullopt;
    }
    MotionRequest r{std::move(*pa), std::move(*pb), head.nestedCanvas};
    return r.isValid() ? std::optional<MotionRequest>(std::move(r)) : std::nullopt;
}

/// The motion a Morph Cut pair (specs[ia] then specs[ib], each a plain media layer) needs on a `canvas` level: their
/// pictures at the frozen source times, placed as they are drawn.
std::optional<MotionRequest> morphRequest(const std::vector<LayerSpec>& specs, const std::vector<media::VideoFrame>& frames,
                                          std::size_t ia, std::size_t ib, render::CanvasSize canvas) {
    if (!canvas.isValid() || ia >= frames.size() || ib >= frames.size() || !frames[ia].isValid() || !frames[ib].isValid()) {
        return std::nullopt;
    }
    auto pa = motionPictureOf(specs[ia], specs[ia].sourceTime, true);
    auto pb = motionPictureOf(specs[ib], specs[ib].sourceTime, true);
    if (!pa || !pb) {
        return std::nullopt;
    }
    MotionRequest r{std::move(*pa), std::move(*pb), canvas};
    return r.isValid() ? std::optional<MotionRequest>(std::move(r)) : std::nullopt;
}

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

/// P.2: the picture of an interpolation group (specs[j]): its two frames on the group's canvas, alone where the source
/// time falls on one of them, else mixed at the source time's place between them (a flow mix along `motion`'s motion
/// for Optical Flow when it has it, a cross dissolve otherwise). Null when the first frame failed.
std::shared_ptr<render::PreparedComposite> interpolationComposite(const std::vector<LayerSpec>& specs,
                                                                  std::vector<media::VideoFrame>& frames, std::size_t j,
                                                                  MotionSource* motion) {
    const LayerSpec& head = specs[j];
    if (j + 2 >= specs.size() || j + 2 >= frames.size() || !frames[j + 1].isValid()) {
        return nullptr;
    }
    const double w = interpolationWeight(head.sourceTime, frames[j + 1], frames[j + 2]);
    std::shared_ptr<const motion::FlowPair> flow;
    if (motion) {
        if (const auto request = interpolationRequest(specs, frames, j)) {
            flow = motion->motion(*request);
        }
    }
    const int streamHeight = head.item && head.streamIndex >= 0 && head.streamIndex < static_cast<int>(head.item->info.video.size())
                                 ? head.item->info.video[static_cast<std::size_t>(head.streamIndex)].height
                                 : 0;
    const auto frameLayer = [&](std::size_t k) {
        render::CompositeLayer l;
        l.frame = std::move(frames[k]);
        applyInterpretation(head.item, l); // P.2: Interpret Footage holds for the interpolated frames too
        l.inputSpace = head.inputSpace;    // and so does the clip's input colour space (I.9)
        l.fieldMode = static_cast<std::uint8_t>( // 0.2: and its field options, at the time each frame is shown for
            fieldRenderAt(specs[k].fields, head.sourceTime, l.frame.pts, l.frame.duration));
        l.transform = transformForFrameHeight(render::LayerTransform{}, streamHeight, l.frame.height);
        l.clipId = head.clip.value;
        return l;
    };
    auto nested = std::make_shared<render::PreparedComposite>();
    nested->canvas = head.nestedCanvas;
    if (w <= 0.0 || w >= 1.0) {
        nested->layers.push_back(frameLayer(w >= 1.0 ? j + 2 : j + 1));
        return nested;
    }
    render::LayerTransition mix;
    mix.kind = flow ? render::TransitionKind::FlowMorph : render::TransitionKind::CrossDissolve;
    mix.progress = w;
    mix.motion = std::move(flow);
    nested->layers.push_back(frameLayer(j + 1));
    nested->layers.back().transition = std::move(mix);
    nested->layers.push_back(frameLayer(j + 2));
    return nested;
}

/// The composite layer of specs[j] without its transition; nullopt when it shows nothing (a failed decode, a nested
/// clip with nothing visible inside).
std::optional<render::CompositeLayer> compositeLayerOf(const std::vector<LayerSpec>& specs,
                                                       std::vector<media::VideoFrame>& frames, std::size_t j,
                                                       MotionSource* motion) {
    const LayerSpec& spec = specs[j];
    render::CompositeLayer layer;
    if (spec.interpolation) {
        auto nested = interpolationComposite(specs, frames, j, motion); // P.2 Time Interpolation
        if (!nested) {
            return std::nullopt;
        }
        layer.nested = std::move(nested);
        layer.transform = spec.transform;
    } else if (spec.nested) {
        auto nested = std::make_shared<render::PreparedComposite>();
        nested->canvas = spec.nestedCanvas;
        buildLevel(specs, frames, j + 1, std::min(specs.size(), nextSibling(specs, j)), nested->layers, motion,
                   spec.nestedCanvas);
        if (nested->layers.empty()) {
            return std::nullopt;
        }
        layer.nested = std::move(nested);
        layer.transform = spec.transform;
        layer.fieldMode = static_cast<std::uint8_t>(fieldRenderAt(spec.fields, spec.sourceTime, Ticks{}, Ticks{}));
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
    } else if (spec.syntheticPicture) {
        auto raster = spec.item ? render::syntheticPicture(*spec.item, spec.sourceTime, spec.fillSize) : nullptr;
        if (!raster) {
            return std::nullopt;
        }
        layer.view = raster->view(raster); // P.2: Bars and Tone, the counting leader
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
        applyInterpretation(spec.item, layer); // P.2: Interpret Footage
        // 0.2 field options: the field the frame shows at this time (both fields at the field rate).
        layer.fieldMode = static_cast<std::uint8_t>(
            fieldRenderAt(spec.fields, spec.sourceTime, layer.frame.pts, layer.frame.duration));
        const int streamHeight =
            spec.item && spec.streamIndex >= 0 && spec.streamIndex < static_cast<int>(spec.item->info.video.size())
                ? spec.item->info.video[static_cast<std::size_t>(spec.streamIndex)].height
                : 0;
        layer.transform = transformForFrameHeight(spec.transform, streamHeight, layer.frame.height);
    }
    layer.warp = spec.warp; // P.2: the stabilizers' warp of the frame inside their group
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
    layer.inputSpace = spec.inputSpace;
    // Display-referred pictures (I.9): titles, captions, graphics and C.15's colour mattes / black video.
    layer.graphics = spec.title.has_value() || spec.caption.has_value() || spec.graphic != nullptr || spec.fill.has_value();
    return layer;
}

/// The layers of one level, specs [begin, end) (nested subtrees become nested composites), on a `canvas` level.
void buildLevel(const std::vector<LayerSpec>& specs, std::vector<media::VideoFrame>& frames, std::size_t begin,
                std::size_t end, std::vector<render::CompositeLayer>& out, MotionSource* motion, render::CanvasSize canvas) {
    std::vector<std::size_t> siblings;
    std::vector<std::vector<std::size_t>> mattesOf; // per sibling: the specs of its matte groups
    std::vector<std::size_t> pendingMattes;
    for (std::size_t j = begin; j < end; j = nextSibling(specs, j)) {
        // A Track Matte (0.1.2) or a library effect's second picture (0.2): not a layer; the next one's effect reads it.
        if (specs[j].matte) {
            pendingMattes.push_back(j);
            continue;
        }
        siblings.push_back(j);
        mattesOf.push_back(std::move(pendingMattes));
        pendingMattes.clear();
    }
    // P.2 Morph Cut: the motion of each pair, asked before the frames move into their layers.
    std::vector<std::shared_ptr<const motion::FlowPair>> morphs(siblings.size());
    for (std::size_t k = 0; motion && k + 1 < siblings.size(); ++k) {
        const std::optional<render::LayerTransition>& group = specs[siblings[k]].transition;
        if (group && group->kind == render::TransitionKind::FlowMorph && group->layerCount == 2) {
            if (const auto request = morphRequest(specs, frames, siblings[k], siblings[k + 1], canvas)) {
                morphs[k] = motion->motion(*request);
            }
        }
    }
    // Every layer first: whether a nested side shows anything decides how its transition mixes.
    std::vector<std::optional<render::CompositeLayer>> built;
    built.reserve(siblings.size());
    for (std::size_t k = 0; k < siblings.size(); ++k) {
        built.push_back(compositeLayerOf(specs, frames, siblings[k], motion));
        if (!built.back()) {
            continue;
        }
        std::vector<render::LayerEffect>& effects = built.back()->effects;
        for (const std::size_t m : mattesOf[k]) {
            const std::optional<render::CompositeLayer> matte = compositeLayerOf(specs, frames, m, motion);
            const int target = specs[m].matteEffect;
            if (target >= 0) {
                if (static_cast<std::size_t>(target) < effects.size()) {
                    effects[static_cast<std::size_t>(target)].matte = matte ? matte->nested : nullptr;
                }
                continue;
            }
            for (render::LayerEffect& effect : effects) {
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
            if (mix.kind == render::TransitionKind::FlowMorph) {
                // P.2 Morph Cut: along the pair's motion; without it (not analysed, not media) a cross dissolve.
                mix.motion = morphs[k];
                if (!mix.motion) {
                    mix.kind = render::TransitionKind::CrossDissolve;
                }
            }
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
                          std::vector<render::CompositeLayer>& out, MotionSource* motion, render::CanvasSize canvas) {
    buildLevel(specs, frames, 0, specs.size(), out, motion, canvas);
}

std::vector<MotionRequest> motionRequestsOf(const std::vector<LayerSpec>& specs,
                                            const std::vector<media::VideoFrame>& frames, render::CanvasSize canvas) {
    std::vector<MotionRequest> out;
    // Every level: the top one on `canvas`, a nested group's on its own canvas.
    struct Range {
        std::size_t begin, end;
        render::CanvasSize canvas;
    };
    std::vector<Range> todo{{0, specs.size(), canvas}};
    while (!todo.empty()) {
        const Range r = todo.back();
        todo.pop_back();
        std::vector<std::size_t> siblings;
        for (std::size_t j = r.begin; j < r.end && j < specs.size(); j = nextSibling(specs, j)) {
            if (specs[j].interpolation) {
                if (auto request = interpolationRequest(specs, frames, j)) {
                    out.push_back(std::move(*request));
                }
                continue;
            }
            if (specs[j].nested) {
                todo.push_back({j + 1, std::min(specs.size(), nextSibling(specs, j)), specs[j].nestedCanvas});
            }
            if (!specs[j].matte) {
                siblings.push_back(j);
            }
        }
        for (std::size_t k = 0; k + 1 < siblings.size(); ++k) {
            const std::optional<render::LayerTransition>& group = specs[siblings[k]].transition;
            if (group && group->kind == render::TransitionKind::FlowMorph && group->layerCount == 2) {
                if (auto request = morphRequest(specs, frames, siblings[k], siblings[k + 1], r.canvas)) {
                    out.push_back(std::move(*request));
                }
            }
        }
    }
    return out;
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
    /// The level's sequence (0.2: the frame rate and names the time and text effects read; P.2: Time Interpolation
    /// applies to footage of another rate than it).
    const Sequence* sequence = nullptr;

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
               std::vector<LayerSpec>& unit, bool interpolate = true);

/// P.2: `spec` (a media layer with LayerSpec::interpolation) as its interpolation group: the head (a group on a canvas of
/// the stream's displayed size, with the clip's placement, effects and redactions) and its two frames.
void appendInterpolationGroup(LayerSpec spec, std::vector<LayerSpec>& unit) {
    const VideoStreamInfo& stream = spec.item->info.video[static_cast<std::size_t>(spec.streamIndex)];
    LayerSpec first;
    first.clip = spec.clip;
    first.media = spec.media;
    first.item = spec.item;
    first.streamIndex = spec.streamIndex;
    first.containerStream = spec.containerStream;
    first.sourceTime = spec.sourceTime;
    first.track = spec.track;
    first.nestPath = spec.nestPath;
    first.decodeKey = spec.decodeKey;
    first.inputSpace = spec.inputSpace;
    first.fields = spec.fields; // 0.2: the frames are deinterlaced, not the group's canvas
    spec.fields = {};
    LayerSpec second = first;
    second.interpolationSecond = true;
    // A first guess at the next frame (one nominal frame on); the decoders take the frame after the first one's.
    const Ticks period = framesToTicks(1, stream.frameRate.isValid() ? stream.frameRate : kRate30, Rounding::Ceil)
                             .value_or(Ticks{kTicksPerSecond / 30});
    second.sourceTime = spec.sourceTime + period;
    spec.nested = true;
    spec.nestedCount = 2;
    spec.nestedCanvas = displayedSize(*spec.item, stream);
    unit.push_back(std::move(spec));
    unit.push_back(std::move(first));
    unit.push_back(std::move(second));
}

/// P.2: what a clip's stabilizers (its first enabled Warp Stabilizer and Rolling Shutter Repair) do to its frame at
/// `curveTime`: the warp (fractions of the frame: render::CompositeLayer::warp), Stabilize, Crop's crop of the
/// stabilized frame, and whether Synthesize Edges fills the uncovered borders.
struct Stabilization {
    motion::Matrix3 warp = motion::kIdentity3;
    std::optional<std::array<double, 4>> crop; ///< left, top, right, bottom fractions
    bool synthesizeEdges = false;
};

std::optional<Stabilization> stabilizationOf(const Clip& clip, Ticks curveTime, const VideoStreamInfo& stream) {
    const EffectInstance* warp = nullptr;
    const EffectInstance* shutter = nullptr;
    for (const EffectInstance& e : clip.effects()) {
        if (!e.enabled || !isEvaluable(e, EffectKind::Video)) {
            continue;
        }
        warp = !warp && e.effect == kWarpStabilizerEffect ? &e : warp;
        shutter = !shutter && e.effect == kRollingShutterRepairEffect ? &e : shutter;
    }
    const double w = stream.width;
    const double h = stream.height;
    if ((!warp && !shutter) || w < 1.0 || h < 1.0) {
        return std::nullopt;
    }
    const auto at = [&](const EffectInstance& e, std::string_view key) { return effectParameterAt(e, key, curveTime); };
    Stabilization s;
    if (shutter) {
        // Rolling Shutter Repair: each line of the picture back to where it was at mid-frame. A line read a share `u` of
        // the frame into the scan saw the picture moved by rate * (u - 0.5) * its motion per frame.
        const double rate = std::clamp(at(*shutter, "rate") / 100.0, 0.0, 1.0);
        const double vx = at(*shutter, "motion.x") / w * rate;
        const double vy = at(*shutter, "motion.y") / h * rate;
        switch (static_cast<int>(std::lround(at(*shutter, "scanDirection")))) {
        case 1: // bottom to top: u = 1 - y
            s.warp = {1.0, vx, -0.5 * vx, 0.0, 1.0 + vy, -0.5 * vy, 0.0, 0.0, 1.0};
            break;
        case 2: // left to right: u = x
            s.warp = {1.0 - vx, 0.0, 0.5 * vx, -vy, 1.0, 0.5 * vy, 0.0, 0.0, 1.0};
            break;
        case 3: // right to left: u = 1 - x
            s.warp = {1.0 + vx, 0.0, -0.5 * vx, vy, 1.0, -0.5 * vy, 0.0, 0.0, 1.0};
            break;
        default: // top to bottom: u = y
            s.warp = {1.0, -vx, 0.5 * vx, 0.0, 1.0 - vy, 0.5 * vy, 0.0, 0.0, 1.0};
            break;
        }
    }
    if (warp) {
        // Warp Stabilizer: the compensating transform the analysis keyed, in source pixels (centres at integers), about
        // the frame's centre; a corner pin for Perspective (and Subspace Warp, approximated by it).
        const int method = static_cast<int>(std::lround(at(*warp, "method")));
        motion::Matrix3 m = motion::kIdentity3;
        if (method >= 2) {
            const std::array<std::array<double, 2>, 4> from{{{-0.5, -0.5}, {w - 0.5, -0.5}, {w - 0.5, h - 0.5}, {-0.5, h - 0.5}}};
            std::array<std::array<double, 2>, 4> to = from;
            static constexpr std::array<std::string_view, 4> kCorners{"tl", "tr", "br", "bl"};
            for (std::size_t k = 0; k < 4; ++k) {
                to[k][0] += at(*warp, "corner." + std::string(kCorners[k]) + ".x");
                to[k][1] += at(*warp, "corner." + std::string(kCorners[k]) + ".y");
            }
            m = motion::homographyFromQuad(from, to).value_or(motion::kIdentity3);
        } else {
            m = motion::similarityAbout((w - 1.0) / 2.0, (h - 1.0) / 2.0, at(*warp, "transform.x"), at(*warp, "transform.y"),
                                        at(*warp, "transform.rotation") * std::numbers::pi / 180.0,
                                        at(*warp, "transform.scale"));
        }
        s.warp = motion::multiply(motion::pixelCentresToFractions(m, w, h), s.warp);
        const int framing = static_cast<int>(std::lround(at(*warp, "framing")));
        if (framing == 1) {
            s.crop = std::array<double, 4>{at(*warp, "crop.left"), at(*warp, "crop.top"), at(*warp, "crop.right"),
                                           at(*warp, "crop.bottom")};
        }
        s.synthesizeEdges = framing == 3;
    }
    if (!std::all_of(s.warp.begin(), s.warp.end(), [](double v) { return std::isfinite(v); }) || !motion::invert(s.warp)) {
        return std::nullopt;
    }
    if (s.warp == motion::kIdentity3 && !s.crop && !s.synthesizeEdges) {
        return std::nullopt; // not analysed yet: nothing to do
    }
    return s;
}

/// P.2: `spec` (a media layer) as its stabilizer group: the head (a group on a canvas of the stream's displayed size,
/// with the clip's placement, effects - Stabilize, Crop's crop first - redactions and transition) over its frame (or its
/// interpolation group) warped, and with Synthesize Edges the frame unwarped behind it (Premiere synthesizes the borders
/// from neighbouring frames; the frame itself stands in for them here).
void appendStabilizerGroup(LayerSpec spec, const Stabilization& s, std::vector<LayerSpec>& unit) {
    const VideoStreamInfo& stream = spec.item->info.video[static_cast<std::size_t>(spec.streamIndex)];
    LayerSpec inner;
    inner.clip = spec.clip;
    inner.media = spec.media;
    inner.item = spec.item;
    inner.streamIndex = spec.streamIndex;
    inner.containerStream = spec.containerStream;
    inner.sourceTime = spec.sourceTime;
    inner.track = spec.track;
    inner.nestPath = spec.nestPath;
    inner.decodeKey = spec.decodeKey;
    inner.interpolation = spec.interpolation;
    inner.inputSpace = spec.inputSpace; // the frame is read in the clip's input colour space inside the group too
    inner.fields = spec.fields;         // and deinterlaced there (0.2), not the group's canvas
    spec.fields = {};
    std::vector<LayerSpec> children;
    if (s.synthesizeEdges) {
        children.push_back(inner); // the frame as it is, behind
    }
    inner.warp = s.warp;
    if (inner.interpolation) {
        appendInterpolationGroup(std::move(inner), children);
    } else {
        children.push_back(std::move(inner));
    }
    spec.nested = true;
    spec.interpolation.reset();
    spec.nestedCanvas = displayedSize(*spec.item, stream);
    spec.nestedCount = static_cast<int>(children.size());
    if (s.crop) {
        render::LayerEffect crop;
        crop.type = render::EffectType::Crop;
        crop.params = {(*s.crop)[0], (*s.crop)[1], (*s.crop)[2], (*s.crop)[3], 0.0};
        spec.effects.insert(spec.effects.begin(), crop);
    }
    unit.push_back(std::move(spec));
    for (LayerSpec& child : children) {
        unit.push_back(std::move(child));
    }
}

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

/// Echo (0.2): the clip's Number of Echoes earlier (Echo Time < 0) or later frames, each at Starting Intensity x
/// Decay^k, composited by the Echo Operator into one group, the second picture of unit[head]'s Echo effect `effect`
/// (the shader adds the current frame). The echoes are the clip's pictures at its motion of now, without its effects
/// (the documented gap: Premiere's echoes carry the effects above Echo); each reads its own decoder. Times outside the
/// clip show nothing. Returns the head's new index.
int addEchoes(const Level& level, const Clip& clip, Ticks time, int track, render::CanvasSize canvas,
              std::vector<LayerSpec>& unit, int head, std::size_t effect) {
    render::LayerEffect& echo = unit[static_cast<std::size_t>(head)].effects[effect];
    const auto& q = echo.params;
    const int count = std::clamp(static_cast<int>(std::lround(q[1])), 0, 8);
    const double intensity = std::clamp(q[2], 0.0, 1.0);
    const double decay = std::clamp(q[3], 0.0, 1.0);
    const int op = static_cast<int>(std::lround(q[4]));
    const LayerSpec& now = unit[static_cast<std::size_t>(head)];
    if (count == 0 || q[0] == 0.0 || now.nested) {
        return head; // nested clips: no echoes (each would be a whole sequence again)
    }
    // Blend: every picture weighs its intensity over their total (the current one too, through Starting Intensity).
    double total = 0.0;
    for (int k = 0; k <= count; ++k) {
        total += intensity * std::pow(decay, k);
    }
    const bool blend = op == 6;
    if (blend) {
        echo.params[2] = total > 0.0 ? intensity / total : 0.0;
    }
    const render::LayerTransform transform = now.transform;
    const std::uint64_t key = sessionKey(now);
    std::vector<LayerSpec> echoes;
    // Farthest echo first (bottom): the nearer echoes cover the farther ones (Composite In Back's order).
    for (int k = count; k >= 1; --k) {
        const double offset = q[0] * k * static_cast<double>(kTicksPerSecond);
        const Ticks at = time + Ticks{static_cast<std::int64_t>(std::llround(offset))};
        if (at < clip.start() || !(at < clip.end())) {
            continue;
        }
        auto spec = layerOf(level.project, clip, at, track, canvas, level.preview, level.luts, level.pathSpan(), level.sequence);
        if (!spec || spec->nested) {
            continue;
        }
        spec->effects.clear();
        spec->bypassedEffects = 0;
        spec->transform = transform;
        const double weight = intensity * std::pow(decay, k);
        spec->opacity = std::clamp(blend && total > 0.0 ? weight / total : weight, 0.0, 1.0);
        spec->blend = op == 1   ? BlendMode::Lighten
                      : op == 2 ? BlendMode::Darken
                      : op == 3 ? BlendMode::Screen
                      : op == 4 || op == 5 ? BlendMode::Normal
                                           : BlendMode::Add;
        spec->nestPath = level.path;
        spec->decodeKey = mixKey(key, 0xEC40u + static_cast<std::uint64_t>(k)) | kNestedDecodeKeyBit;
        echoes.push_back(std::move(*spec));
    }
    if (echoes.empty()) {
        return head;
    }
    if (op == 5) {
        std::reverse(echoes.begin(), echoes.end()); // Composite In Front: each echo over the nearer ones
    }
    wrapInStage(echoes, {}, 0, canvas, time, track, level.path);
    echoes.front().matte = true;
    echoes.front().matteEffect = static_cast<int>(effect);
    const auto inserted = static_cast<int>(echoes.size());
    unit.insert(unit.begin() + head, std::make_move_iterator(echoes.begin()), std::make_move_iterator(echoes.end()));
    return head + inserted;
}

/// The library effects of unit[head] that read another track (0.2): Gradient Wipe's Gradient Layer and Difference
/// Matte's Difference Layer, a video track number. That track's clip at `time` becomes a group flagged `matte` with the
/// effect's index (as addTrackMatte), placed before the layer; an invalid or empty track leaves the effect without it
/// (Gradient Wipe then uses the layer's own luminance, Difference Matte keeps everything). Echo's echoes too
/// (addEchoes). Returns the head's new index.
int addSecondPictures(const Level& level, const Sequence& sequence, const Clip& clip, Ticks time, int track,
                      render::CanvasSize canvas, std::vector<LayerSpec>& unit, int head) {
    for (std::size_t e = 0; e < unit[static_cast<std::size_t>(head)].effects.size(); ++e) {
        const render::LayerEffect& effect = unit[static_cast<std::size_t>(head)].effects[e];
        if (effect.type != render::EffectType::Library) {
            continue;
        }
        const auto op = static_cast<render::LibraryOp>(effect.op);
        if (op == render::LibraryOp::Echo) {
            head = addEchoes(level, clip, time, track, canvas, unit, head, e);
            continue;
        }
        if (op != render::LibraryOp::GradientWipe && op != render::LibraryOp::DifferenceMatte) {
            continue;
        }
        const int number = static_cast<int>(std::lround(op == render::LibraryOp::GradientWipe ? effect.params[2] : effect.params[1]));
        if (number < 1 || number > static_cast<int>(sequence.videoTracks.size()) || number - 1 == track ||
            !sequence.videoTracks[static_cast<std::size_t>(number - 1)]) {
            continue;
        }
        const Track& other = *sequence.videoTracks[static_cast<std::size_t>(number - 1)];
        const auto index = other.clipIndexAt(time);
        if (!index) {
            continue;
        }
        const Clip& source = other.clips[*index];
        if (const MediaItem* item = level.project.findMedia(source.media); item && isAdjustmentLayer(*item)) {
            continue;
        }
        std::vector<LayerSpec> picture;
        if (appendClip(level, source, time, number - 1, canvas, picture) < 0) {
            continue;
        }
        wrapInStage(picture, {}, 0, canvas, time, number - 1, level.path);
        picture.front().matte = true;
        picture.front().matteEffect = static_cast<int>(e);
        const auto inserted = static_cast<int>(picture.size());
        unit.insert(unit.begin() + head, std::make_move_iterator(picture.begin()), std::make_move_iterator(picture.end()));
        head += inserted;
    }
    return head;
}

void evaluateLevel(const Level& level, const Sequence& sequence, Ticks time, std::span<const std::size_t> tracks,
                   std::vector<LayerSpec>& out);

/// Appends what `clip` (a clip of the level's sequence) shows at `time` to `unit`: one spec for a media or title clip,
/// a group and its subtree for a nested or multicam clip. Returns the index in `unit` of the clip's own (head) spec,
/// or -1 when it shows nothing.
int appendClip(const Level& level, const Clip& clip, Ticks time, int track, render::CanvasSize canvas,
               std::vector<LayerSpec>& unit, bool interpolate) {
    if (!clip.enabled || clip.stream.kind != StreamKind::Video || clip.hasUnknownSource()) {
        return -1;
    }
    if (level.budget && --*level.budget < 0) {
        return -1; // past the frame's layer budget
    }
    time = posterizedTime(clip, time); // 0.2 Posterize Time: the clip held at its own frame rate
    if (!clip.isNested()) {
        auto spec = layerOf(level.project, clip, time, track, canvas, level.preview, level.luts, level.pathSpan(),
                            level.sequence, interpolate);
        if (!spec) {
            return -1;
        }
        if (level.path) {
            spec->nestPath = level.path;
            spec->decodeKey = mixKey(level.pathKey, clip.id.value) | kNestedDecodeKeyBit;
        }
        if (needsDecode(*spec) && spec->item && spec->streamIndex >= 0 &&
            spec->streamIndex < static_cast<int>(spec->item->info.video.size())) {
            const VideoStreamInfo& stream = spec->item->info.video[static_cast<std::size_t>(spec->streamIndex)];
            if (const auto stabilization = stabilizationOf(clip, curveTimeAround(clip, time), stream)) {
                const int head = static_cast<int>(unit.size());
                appendStabilizerGroup(std::move(*spec), *stabilization, unit); // P.2 Warp Stabilizer, Rolling Shutter Repair
                return head;
            }
        }
        if (spec->interpolation) {
            appendInterpolationGroup(std::move(*spec), unit); // P.2 Time Interpolation
            return static_cast<int>(unit.size()) - 3;
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
    head.transform.scale = p.value(ParamId::Scale, curveTime) * // P.2: Scale to Frame Size
                           scaleToFrameFactor(level.project, clip, canvas.width, canvas.height);
    head.transform.rotationDegrees = p.value(ParamId::Rotation, curveTime);
    head.transform.anchor = {p.value(ParamId::AnchorX, curveTime), p.value(ParamId::AnchorY, curveTime)};
    head.opacity = std::clamp(p.value(ParamId::Opacity, curveTime), 0.0, 1.0);
    head.blend = clip.blendMode();
    head.track = track;
    head.nestPath = level.path;
    // 0.2: the nested canvas is progressive; Always Deinterlace and Flicker Removal still apply to it.
    head.fields = fieldRenderingOf(clip, nullptr, 0, level.sequence ? *level.sequence : progressiveSequence());
    if (head.transform.scale == 0.0) {
        return -1;
    }
    const double aspect = static_cast<double>(nested->width) / nested->height;
    EffectContext context = effectContextOf(level.project, level.sequence, clip, nullptr, time, canvas);
    context.sourceTime = nestedTime;
    evaluateEffects(clip, curveTime, head.effects, head.bypassedEffects, level.luts, aspect, std::nullopt, &context);

    auto path = std::make_shared<std::vector<ClipId>>(level.path ? *level.path : std::vector<ClipId>{});
    path->push_back(clip.id);
    Level inner{level.project, level.preview, level.luts, level.cullOccluded, std::move(path),
                mixKey(level.pathKey, clip.id.value), level.depth + 1, level.budget, level.captions, nested};
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
        const EffectContext stage = stageContextOf(level.project, *nested, nestedTime, head.nestedCanvas);
        evaluateEffectStack(nested->effects, nestedTime, master, masterBypassed, level.luts, aspect, std::nullopt, &stage);
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
            const EffectContext context = stageContextOf(level.project, sequence, time, canvas);
            evaluateEffectStack(track.effects, time, trackEffects, trackBypassed, level.luts,
                                static_cast<double>(canvas.width) / std::max(1, canvas.height), std::nullopt, &context);
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
            // P.2 Morph Cut: each side frozen at the transition's end it touches - the last picture of the outgoing clip
            // before the transition and the first of the incoming one after it - morphed into each other (no handles
            // are read, so no removed words come back).
            Ticks ta = time;
            Ticks tb = time;
            const bool morph = mix && mix->kind == render::TransitionKind::FlowMorph && hit->outgoing && hit->incoming;
            if (morph) {
                const Transition& tr = *hit->transition;
                ta = std::clamp(hit->cut - tr.before, hit->outgoing->start(), hit->outgoing->end() - Ticks{1});
                tb = std::clamp(hit->cut + tr.after, hit->incoming->start(), hit->incoming->end() - Ticks{1});
            }
            std::vector<LayerSpec> a;
            std::vector<LayerSpec> b;
            int ia = hit->outgoing ? appendClip(level, *hit->outgoing, ta, ti, canvas, a, !morph) : -1;
            int ib = hit->incoming ? appendClip(level, *hit->incoming, tb, ti, canvas, b, !morph) : -1;
            ia = ia >= 0 ? addTrackMatte(level, sequence, ta, ti, canvas, a, ia) : ia;
            ib = ib >= 0 ? addTrackMatte(level, sequence, tb, ti, canvas, b, ib) : ib;
            ia = ia >= 0 ? addSecondPictures(level, sequence, *hit->outgoing, ta, ti, canvas, a, ia) : ia;
            ib = ib >= 0 ? addSecondPictures(level, sequence, *hit->incoming, tb, ti, canvas, b, ib) : ib;
            if (isAnimationTransition(hit->transition->effect)) {
                // 0.2 Animations: the incoming clip animates in over the outgoing one (or alone at its In point); a clip
                // with no incoming one animates out. Its own layer moves, so nothing is mixed.
                const Transition& tr = *hit->transition;
                const bool leaving = ib < 0;
                std::vector<LayerSpec>& moving = leaving ? a : b;
                const int head = leaving ? ia : ib;
                if (head < 0) {
                    continue;
                }
                LayerSpec& spec = moving[static_cast<std::size_t>(head)];
                const Ticks length = tr.before + tr.after;
                const double progress = length.value > 0 ? static_cast<double>((time - (hit->cut - tr.before)).value) /
                                                               static_cast<double>(length.value)
                                                         : 1.0;
                if (const auto anim = transitionAnimationAt(tr, progress, leaving, canvas, layerFrameOf(spec, canvas))) {
                    spec.transform.position.x += anim->offset.x;
                    spec.transform.position.y += anim->offset.y;
                    spec.transform.scale *= anim->scale;
                    spec.transform.rotationDegrees += anim->rotationDegrees;
                    spec.opacity *= anim->opacity;
                    spec.effects.insert(spec.effects.end(), anim->effects.begin(), anim->effects.end());
                }
                if (!leaving && ia >= 0) {
                    for (LayerSpec& s : b) {
                        a.push_back(std::move(s));
                    }
                    pushUnit(std::move(a));
                } else {
                    pushUnit(std::move(moving));
                }
                continue;
            }
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
                const EffectContext context = effectContextOf(level.project, &sequence, clip, nullptr, time, canvas);
                evaluateEffects(clip, curveTime, adjustment.effects, adjustment.bypassed, level.luts,
                                static_cast<double>(canvas.width) / std::max(1, canvas.height), std::nullopt, &context);
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
        head = addSecondPictures(level, sequence, clip, time, ti, canvas, unit, head);
        const LayerSpec& spec = unit[static_cast<std::size_t>(head)];
        if (spec.opacity <= 0.0) {
            continue;
        }
        bool covers = false;
        if (level.cullOccluded && trackEffects.empty() && (!spec.nested || spec.interpolation) && spec.item && spec.streamIndex >= 0 &&
            spec.streamIndex < static_cast<int>(spec.item->info.video.size())) {
            // A track with effects is a group: what its effects make of the layer (a crop, a blur) may not cover.
            const VideoStreamInfo& stream = spec.item->info.video[static_cast<std::size_t>(spec.streamIndex)];
            const double par = pixelAspectOf(*spec.item, stream); // P.2: Interpret Footage's when set
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
    const Level level{project, preview, &luts, cullOccluded, nullptr, 0, 0, &budget, captions, &sequence};
    evaluateLevel(level, sequence, time, tracks, out);
    // The sequence's master stack (I.9): a group over every track (the burned-in caption stays above it, as delivered).
    if (!sequence.effects.empty() && !sequence.multicam && !out.empty()) {
        std::vector<render::LayerEffect> master;
        int bypassed = 0;
        const render::CanvasSize canvas{sequence.width, sequence.height};
        const EffectContext context = stageContextOf(project, sequence, time, canvas);
        evaluateEffectStack(sequence.effects, time, master, bypassed, &luts,
                            static_cast<double>(canvas.width) / std::max(1, canvas.height), std::nullopt, &context);
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

std::vector<MotionRequest> morphCutRequests(const ProjectState& project, const Sequence& sequence,
                                            const PreviewSettings* preview) {
    std::vector<MotionRequest> out;
    const render::CanvasSize canvas{sequence.width, sequence.height};
    for (std::size_t i = 0; i < sequence.videoTracks.size(); ++i) {
        const TrackPtr& track = sequence.videoTracks[i];
        if (!track || !track->enabled) {
            continue;
        }
        for (const Transition& tr : track->transitions) {
            if (tr.effect != kMorphCutTransition || !tr.outgoing.isValid() || !tr.incoming.isValid()) {
                continue;
            }
            const Clip* a = nullptr;
            const Clip* b = nullptr;
            for (const Clip& c : track->clips) {
                a = c.id == tr.outgoing ? &c : a;
                b = c.id == tr.incoming ? &c : b;
            }
            if (!a || !b || a->end() != b->start()) {
                continue;
            }
            // The frozen sides evaluateLevel shows (the same specs, so the same requests buildCompositeLayers makes).
            const Ticks cut = a->end();
            const Ticks ta = std::clamp(cut - tr.before, a->start(), a->end() - Ticks{1});
            const Ticks tb = std::clamp(cut + tr.after, b->start(), b->end() - Ticks{1});
            const int ti = static_cast<int>(i);
            const auto sa = layerOf(project, *a, ta, ti, canvas, preview, nullptr, {}, &sequence, false);
            const auto sb = layerOf(project, *b, tb, ti, canvas, preview, nullptr, {}, &sequence, false);
            if (!sa || !sb) {
                continue;
            }
            auto pa = motionPictureOf(*sa, sa->sourceTime, true);
            auto pb = motionPictureOf(*sb, sb->sourceTime, true);
            if (!pa || !pb) {
                continue;
            }
            MotionRequest r{std::move(*pa), std::move(*pb), canvas};
            if (r.isValid()) {
                out.push_back(std::move(r));
            }
        }
    }
    return out;
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
