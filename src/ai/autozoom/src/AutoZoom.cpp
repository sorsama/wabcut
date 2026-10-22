#include <wabcut/ai/autozoom/AutoZoom.h>

#include <wabcut/ai/reframe/CameraPath.h>
#include <wabcut/ai/reframe/Reframe.h>
#include <wabcut/model/ProjectOps.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

namespace wabcut::ai::autozoom {

using Json = nlohmann::json;

namespace {

Ticks fromSeconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

/// A rectangle in frame units.
struct Region {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    Region united(const Region& o) const { return {std::min(x0, o.x0), std::min(y0, o.y0), std::max(x1, o.x1), std::max(y1, o.y1)}; }
    double cx() const { return (x0 + x1) / 2.0; }
    double cy() const { return (y0 + y1) / 2.0; }
};

struct Segment {
    Ticks first;
    Ticks last;
    Region region;
    std::size_t clicks = 0;
    std::size_t keys = 0;
    std::size_t dwells = 0;
    std::size_t scrolls = 0;
    std::size_t changes = 0;
    double zoom = 1.0;
};

double zoomFor(const Region& r, const ZoomOptions& options) {
    const double w = std::max(1e-6, (r.x1 - r.x0) * (1.0 + 2.0 * options.margin));
    const double h = std::max(1e-6, (r.y1 - r.y0) * (1.0 + 2.0 * options.margin));
    return std::clamp(std::min(1.0 / w, 1.0 / h), 1.0, options.maxZoom);
}

std::string reasonOf(const Segment& s) {
    std::vector<std::string> parts;
    const auto add = [&](std::size_t n, std::string_view one, std::string_view many) {
        if (n == 1) {
            parts.emplace_back(one);
        } else if (n > 1) {
            parts.push_back(std::to_string(n) + " " + std::string(many));
        }
    };
    add(s.clicks, "a click", "clicks");
    add(s.keys > 0 ? 1 : 0, "typing", "typing");
    add(s.scrolls, "a scroll", "scrolls");
    add(s.changes, "a change", "changes");
    add(s.clicks + s.keys + s.scrolls + s.changes == 0 ? s.dwells : 0, "the cursor resting", "cursor rests");
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        out += (i == 0 ? "" : i + 1 == parts.size() ? " and " : ", ") + parts[i];
    }
    return out;
}

/// Ramer-Douglas-Peucker over the samples [a, b] of three channels; keeps indices into `keep`.
void simplify(const std::vector<double>& z, const std::vector<double>& u, const std::vector<double>& v, std::size_t a, std::size_t b,
              double tolerance, std::vector<std::size_t>& keep) {
    if (b <= a + 1) {
        return;
    }
    double worst = 0.0;
    std::size_t at = a;
    for (std::size_t k = a + 1; k < b; ++k) {
        const double f = static_cast<double>(k - a) / static_cast<double>(b - a);
        const double dz = std::abs(z[k] - (z[a] + f * (z[b] - z[a]))) * 0.5;
        const double du = std::abs(u[k] - (u[a] + f * (u[b] - u[a])));
        const double dv = std::abs(v[k] - (v[a] + f * (v[b] - v[a])));
        const double d = std::max({dz, du, dv});
        if (d > worst) {
            worst = d;
            at = k;
        }
    }
    if (worst > tolerance) {
        simplify(z, u, v, a, at, tolerance, keep);
        keep.push_back(at);
        simplify(z, u, v, at, b, tolerance, keep);
    }
}

} // namespace

std::string_view toString(Easing easing) {
    switch (easing) {
    case Easing::Linear:
        return "linear";
    case Easing::Smooth:
        return "smooth";
    case Easing::EaseInOut:
        break;
    }
    return "easeInOut";
}

std::optional<Easing> easingFromString(std::string_view text) {
    for (const Easing e : {Easing::EaseInOut, Easing::Linear, Easing::Smooth}) {
        if (text == toString(e)) {
            return e;
        }
    }
    return std::nullopt;
}

std::string optionsToJson(const ZoomOptions& o) {
    return Json{{"maxZoom", o.maxZoom},
                {"holdSeconds", o.holdSeconds},
                {"idleSeconds", o.idleSeconds},
                {"transitionSeconds", o.transitionSeconds},
                {"easing", toString(o.easing)},
                {"smoothness", o.smoothness},
                {"clicks", o.clicks},
                {"typing", o.typing},
                {"dwell", o.dwell}}
        .dump();
}

std::pair<double, double> Placement::toFrame(double x, double y) const {
    const double px = (x - 0.5) * pictureWidth - anchorX;
    const double py = (y - 0.5) * pictureHeight - anchorY;
    const double r = rotation * std::numbers::pi / 180.0;
    const double rx = std::cos(r) * px - std::sin(r) * py;
    const double ry = std::sin(r) * px + std::cos(r) * py;
    return {(positionX + scale * rx) / frameWidth, (positionY + scale * ry) / frameHeight};
}

ZoomPath planZoom(const ActivityTrack& activity, TimeRange source, const Placement& placement, const ZoomOptions& options) {
    ZoomPath path;
    const double fps = std::clamp(options.gridFps, 5.0, 60.0);
    const double dt = 1.0 / fps;
    const double length = source.duration.toSeconds();
    if (length <= 0.0) {
        return path;
    }
    const std::size_t n = static_cast<std::size_t>(std::floor(length * fps)) + 1;
    for (std::size_t k = 0; k < n; ++k) {
        path.times.push_back(std::min(source.start + fromSeconds(static_cast<double>(k) * dt), source.end() - Ticks{1}));
    }
    // 1. Segments of nearby events.
    const double minW = options.minRegion * placement.frameHeight / placement.frameWidth;
    const double minH = options.minRegion;
    std::vector<Segment> segments;
    std::vector<Ticks> sceneChanges;
    const Ticks idle = fromSeconds(options.idleSeconds);
    for (const ActivityEvent& e : activity.events) {
        if (e.time < source.start || e.time >= source.end()) {
            continue;
        }
        if (e.kind == ActivityKind::SceneChange) {
            sceneChanges.push_back(e.time);
            continue;
        }
        const bool wanted = ((e.kind == ActivityKind::Click || e.kind == ActivityKind::Scroll || e.kind == ActivityKind::Change) && options.clicks) ||
                            (e.kind == ActivityKind::Key && options.typing) || (e.kind == ActivityKind::Dwell && options.dwell);
        if (!wanted) {
            continue;
        }
        const auto [fx0, fy0] = placement.toFrame(e.x - e.width / 2.0, e.y - e.height / 2.0);
        const auto [fx1, fy1] = placement.toFrame(e.x + e.width / 2.0, e.y + e.height / 2.0);
        Region r{std::min(fx0, fx1), std::min(fy0, fy1), std::max(fx0, fx1), std::max(fy0, fy1)};
        if (r.x1 - r.x0 < minW) {
            const double c = r.cx();
            r.x0 = c - minW / 2.0;
            r.x1 = c + minW / 2.0;
        }
        if (r.y1 - r.y0 < minH) {
            const double c = r.cy();
            r.y0 = c - minH / 2.0;
            r.y1 = c + minH / 2.0;
        }
        const bool sceneBetween = !segments.empty() && std::ranges::any_of(sceneChanges, [&](Ticks s) { return s > segments.back().last && s <= e.time; });
        if (!segments.empty() && !sceneBetween && e.time - segments.back().last <= idle &&
            zoomFor(segments.back().region.united(r), options) >= std::min(1.25, options.maxZoom)) {
            Segment& s = segments.back();
            s.last = e.time;
            s.region = s.region.united(r);
        } else {
            segments.push_back(Segment{e.time, e.time, r});
        }
        Segment& s = segments.back();
        switch (e.kind) {
        case ActivityKind::Click:
            ++s.clicks;
            break;
        case ActivityKind::Key:
            ++s.keys;
            break;
        case ActivityKind::Scroll:
            ++s.scrolls;
            break;
        case ActivityKind::Change:
            ++s.changes;
            break;
        case ActivityKind::Dwell:
        case ActivityKind::SceneChange:
            ++s.dwells;
            break;
        }
    }
    // 2. What the camera wants per sample.
    std::vector<double> wantZoom(n, 1.0);
    std::vector<double> wantU(n, 0.0);
    std::vector<double> wantV(n, 0.0);
    // Data weights (per second). The camera-path weights are made for pans of a fraction of the frame over whole
    // shots; a zoom of x2 held for a second or two must outweigh the cost of easing in and out (the jerk and
    // acceleration terms of a full zoom step are some 20 times the data term of a 2 s hold at weight 1), and an idle
    // stretch must pull the zoom back out. In the L1 balance an ease lasts about weight^(-1/3): weight 40 eases over
    // about 2.2 s at the default smoothness, so the weights are set for eases of about transitionSeconds.
    const double easeScale = std::pow(2.2 / std::clamp(options.transitionSeconds, 0.2, 3.0), 3.0);
    const double kHoldWeight = 40.0 * easeScale;
    const double kIdleWeight = 4.0 * easeScale;
    std::vector<double> weightZoom(n, kIdleWeight);
    std::vector<double> weightUV(n, 1.0);
    const Ticks lead = fromSeconds(options.leadSeconds);
    const Ticks hold = fromSeconds(options.holdSeconds);
    for (Segment& s : segments) {
        s.zoom = zoomFor(s.region, options);
        if (s.zoom < 1.15) {
            continue;
        }
        Ticks from = std::max(source.start, s.first - lead);
        Ticks to = std::min(source.end(), s.last + hold);
        for (const Ticks scene : sceneChanges) {
            if (scene > s.last && scene < to) {
                to = scene - fromSeconds(0.1);
            }
        }
        if (to <= from) {
            continue;
        }
        const double half = (1.0 - 1.0 / s.zoom) / 2.0;
        const double u = std::clamp(s.region.cx(), -half, half);
        const double v = std::clamp(s.region.cy(), -half, half);
        for (std::size_t k = 0; k < n; ++k) {
            if (path.times[k] >= from && path.times[k] <= to && s.zoom > wantZoom[k]) {
                wantZoom[k] = s.zoom;
                wantU[k] = u;
                wantV[k] = v;
                weightZoom[k] = kHoldWeight;
                weightUV[k] = kHoldWeight;
            }
        }
        ZoomSegment out;
        out.start = from;
        out.end = to;
        out.zoom = s.zoom;
        // The focus as a picture point (inverse of toFrame for the unrotated case; rotation is rare in recordings).
        out.x = std::clamp(0.5 + (s.region.cx() * placement.frameWidth - placement.positionX) / (placement.scale * placement.pictureWidth) +
                               placement.anchorX / placement.pictureWidth,
                           0.0, 1.0);
        out.y = std::clamp(0.5 + (s.region.cy() * placement.frameHeight - placement.positionY) / (placement.scale * placement.pictureHeight) +
                               placement.anchorY / placement.pictureHeight,
                           0.0, 1.0);
        out.events = s.clicks + s.keys + s.dwells + s.scrolls + s.changes;
        out.reason = reasonOf(s);
        path.segments.push_back(out);
    }
    // 3. The L1-optimal camera (AI-9): zoom first, then the centre inside the bounds that zoom allows. The path starts
    //    zoomed out at the clip's first frame, so the clip's own placement is always recoverable (idempotent reruns).
    const reframe::PathWeights weights = reframe::weightsForSmoothness(options.smoothness);
    reframe::PathProblem zoomProblem;
    zoomProblem.dt = dt;
    zoomProblem.desired = wantZoom;
    zoomProblem.weight = weightZoom;
    zoomProblem.lower.assign(n, 1.0);
    zoomProblem.upper.assign(n, options.maxZoom);
    zoomProblem.upper[0] = 1.0;
    std::vector<double> zoom = reframe::solveCameraPath(zoomProblem, weights);
    for (double& z : zoom) {
        z = std::clamp(z, 1.0, options.maxZoom);
        if (z < 1.0 + 1e-4) {
            z = 1.0;
        }
    }
    path.zoom = std::move(zoom);
    const auto solveCentre = [&](const std::vector<double>& want) {
        reframe::PathProblem p;
        p.dt = dt;
        p.weight = weightUV;
        p.lower.resize(n);
        p.upper.resize(n);
        p.desired.resize(n);
        for (std::size_t k = 0; k < n; ++k) {
            const double half = (1.0 - 1.0 / path.zoom[k]) / 2.0;
            p.lower[k] = -half;
            p.upper[k] = half;
            p.desired[k] = std::clamp(want[k], -half, half);
        }
        std::vector<double> c = reframe::solveCameraPath(p, weights);
        for (std::size_t k = 0; k < n; ++k) {
            c[k] = std::clamp(c[k], p.lower[k], p.upper[k]);
            if (std::abs(c[k]) < 1e-6) {
                c[k] = 0.0;
            }
        }
        return c;
    };
    path.u = solveCentre(wantU);
    path.v = solveCentre(wantV);
    // 4. Keys: where the camera starts or stops moving (easing), or the simplified path (smooth).
    std::vector<std::size_t> keys{0};
    if (options.easing == Easing::Smooth) {
        simplify(path.zoom, path.u, path.v, 0, n - 1, options.tolerance, keys);
    } else {
        const double still = options.tolerance * 0.1;
        const auto moving = [&](std::size_t k) {
            return std::abs(path.zoom[k + 1] - path.zoom[k]) * 0.5 > still || std::abs(path.u[k + 1] - path.u[k]) > still ||
                   std::abs(path.v[k + 1] - path.v[k]) > still;
        };
        for (std::size_t k = 0; k + 1 < n; ++k) {
            const bool before = k > 0 && moving(k - 1);
            const bool after = moving(k);
            if (before != after && k > 0) {
                keys.push_back(k);
            }
        }
    }
    keys.push_back(n - 1);
    std::ranges::sort(keys);
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    // Holds exactly still, and every move at least transitionSeconds long (taken from the holds around it).
    if (options.easing != Easing::Smooth && keys.size() > 2) {
        const auto same = [&](std::size_t a, std::size_t b) {
            return std::abs(path.zoom[a] - path.zoom[b]) * 0.5 <= options.tolerance && std::abs(path.u[a] - path.u[b]) <= options.tolerance &&
                   std::abs(path.v[a] - path.v[b]) <= options.tolerance;
        };
        for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
            if (same(keys[i], keys[i + 1])) {
                for (std::size_t k = keys[i] + 1; k <= keys[i + 1]; ++k) {
                    path.zoom[k] = path.zoom[keys[i]];
                    path.u[k] = path.u[keys[i]];
                    path.v[k] = path.v[keys[i]];
                }
            }
        }
        const std::size_t want = static_cast<std::size_t>(std::lround(options.transitionSeconds * fps));
        const std::size_t minHold = static_cast<std::size_t>(std::lround(0.2 * fps));
        for (std::size_t i = 1; i + 2 < keys.size(); ++i) {
            const std::size_t a = keys[i];
            const std::size_t b = keys[i + 1];
            if (same(a, b) || b - a >= want) {
                continue;
            }
            std::size_t extra = want - (b - a);
            const std::size_t room0 = (a > keys[i - 1] + minHold && same(keys[i - 1], a)) ? a - keys[i - 1] - minHold : 0;
            const std::size_t room1 = (keys[i + 2] > b + minHold && same(b, keys[i + 2])) ? keys[i + 2] - b - minHold : 0;
            const std::size_t take0 = std::min(room0, (extra + 1) / 2);
            const std::size_t take1 = std::min(room1, extra - take0);
            // Move the keys, carrying their values (the plateau they bound).
            const auto move = [&](std::size_t from, std::size_t to) {
                path.zoom[to] = path.zoom[from];
                path.u[to] = path.u[from];
                path.v[to] = path.v[from];
            };
            if (take0 > 0) {
                move(a, a - take0);
                keys[i] = a - take0;
            }
            if (take1 > 0) {
                move(b, b + take1);
                keys[i + 1] = b + take1;
            }
        }
    }
    path.keys = std::move(keys);
    return path;
}

std::expected<Placement, Error> placementOf(const ProjectState& state, SequenceId sequence, ClipId clipId) {
    const Sequence* seq = state.findSequence(sequence);
    if (!seq) {
        return std::unexpected(Error(ErrorCode::NotFound, "no sequence " + std::to_string(sequence.value)));
    }
    const auto location = seq->findClip(clipId);
    if (!location) {
        return std::unexpected(Error(ErrorCode::NotFound, "no clip " + std::to_string(clipId.value) + " in '" + seq->name + "'"));
    }
    const Clip& clip = location->clip();
    if (clip.stream.kind != StreamKind::Video) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "auto zoom needs a video clip"));
    }
    if (clip.isFreeze() || clip.isRemapped()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "auto zoom needs a clip that plays at a steady speed (not a freeze frame or time remap)"));
    }
    reframe::DisplayGeometry display;
    if (clip.isNested()) {
        // 0.2: a nested or multicam clip's picture is its nested sequence's canvas.
        const Sequence* inner = state.findSequence(clip.sequence);
        if (!inner || inner->width <= 0 || inner->height <= 0) {
            return std::unexpected(Error(ErrorCode::NotFound, "the clip's nested sequence has no picture"));
        }
        display = reframe::DisplayGeometry{static_cast<double>(inner->width), static_cast<double>(inner->height)};
    } else {
        const MediaItem* media = state.findMedia(clip.media);
        if (!media || clip.stream.index < 0 || static_cast<std::size_t>(clip.stream.index) >= media->info.video.size()) {
            return std::unexpected(Error(ErrorCode::NotFound, "the clip's media has no such video stream"));
        }
        display = reframe::displayGeometry(media->info.video[static_cast<std::size_t>(clip.stream.index)]);
    }
    Placement p;
    p.frameWidth = seq->width;
    p.frameHeight = seq->height;
    p.pictureWidth = display.width > 0.0 ? display.width : seq->width;
    p.pictureHeight = display.height > 0.0 ? display.height : seq->height;
    const auto base = [&](ParamId id) {
        const ParamValue& value = clip.properties()[id];
        if (const Curve* curve = value.curve(); curve && !curve->keys.empty()) {
            return curve->keys.front().value;
        }
        return value.constant();
    };
    p.positionX = base(ParamId::PositionX);
    p.positionY = base(ParamId::PositionY);
    p.scale = base(ParamId::Scale);
    p.anchorX = base(ParamId::AnchorX);
    p.anchorY = base(ParamId::AnchorY);
    p.rotation = base(ParamId::Rotation);
    if (p.scale <= 0.0) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the clip's scale is 0: nothing to zoom"));
    }
    return p;
}

std::expected<ZoomResult, Error> applyZoom(const ProjectState& state, SequenceId sequence, ClipId clipId, const ZoomPath& path,
                                           const Placement& placement, const ZoomOptions& options) {
    const Sequence* seq = state.findSequence(sequence);
    const auto location = seq ? seq->findClip(clipId) : std::nullopt;
    if (!location) {
        return std::unexpected(Error(ErrorCode::NotFound, "no such clip"));
    }
    const Clip& clip = location->clip();
    ZoomResult result;
    result.placement = placement;
    result.path = path;
    ClipProperties props = clip.properties();
    for (const ParamId id : {ParamId::PositionX, ParamId::PositionY, ParamId::Scale}) {
        if (const Curve* curve = props[id].curve()) {
            result.replacedKeys += curve->keys.size();
        }
    }
    const auto valueOf = [&](std::size_t k, ParamId id) {
        const double z = path.zoom[k];
        switch (id) {
        case ParamId::PositionX:
            return z * (placement.positionX - path.u[k] * placement.frameWidth);
        case ParamId::PositionY:
            return z * (placement.positionY - path.v[k] * placement.frameHeight);
        default:
            return z * placement.scale;
        }
    };
    const bool zooms = std::ranges::any_of(path.zoom, [](double z) { return z > 1.0 + 1e-6; });
    if (!zooms || path.keys.size() < 2) {
        props.set(ParamId::PositionX, placement.positionX);
        props.set(ParamId::PositionY, placement.positionY);
        props.set(ParamId::Scale, placement.scale);
        result.warnings.emplace_back("nothing in the recording asked for a zoom: the clip keeps its framing");
    } else if (options.easing == Easing::Smooth) {
        std::vector<Ticks> times;
        for (const std::size_t k : path.keys) {
            times.push_back(path.times[k]);
        }
        for (const ParamId id : {ParamId::PositionX, ParamId::PositionY, ParamId::Scale}) {
            std::vector<double> values;
            for (const std::size_t k : path.keys) {
                values.push_back(valueOf(k, id));
            }
            const double tolerance = id == ParamId::Scale ? placement.scale * 0.002 : 1.0;
            Curve curve = reframe::fitCurve(times, values, tolerance);
            result.keyframes += curve.keys.size();
            props[id] = curve.keys.size() == 1 ? ParamValue(curve.keys.front().value) : ParamValue(std::move(curve));
        }
    } else {
        const Interpolation interpolation = options.easing == Easing::Linear ? Interpolation::Linear : Interpolation::EaseInOut;
        for (const ParamId id : {ParamId::PositionX, ParamId::PositionY, ParamId::Scale}) {
            Curve curve;
            for (const std::size_t k : path.keys) {
                Keyframe key;
                key.time = path.times[k];
                key.value = valueOf(k, id);
                key.interpolation = interpolation;
                curve.keys.push_back(key);
            }
            result.keyframes += curve.keys.size();
            props[id] = ParamValue(std::move(curve));
        }
    }
    const ParameterInfo& scaleInfo = parameterInfo(ParamId::Scale);
    if (placement.scale * options.maxZoom > scaleInfo.maxValue) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the zoomed scale would exceed the largest scale"));
    }
    wabcut::edit::ClipAttributeChange change;
    change.properties = props;
    auto edited = wabcut::edit::setClipAttributes(state, sequence, std::vector<ClipId>{clipId}, change);
    if (!edited) {
        return std::unexpected(edited.error());
    }
    result.state = std::move(edited->state);
    return result;
}

double maxOutside(const Clip& clip, const Placement& placement, double fps) {
    double worst = 0.0;
    const double length = clip.sourceDuration().toSeconds();
    const std::size_t n = static_cast<std::size_t>(std::floor(length * fps)) + 1;
    for (std::size_t k = 0; k < n; ++k) {
        const Ticks t = clip.sourceIn + fromSeconds(static_cast<double>(k) / fps);
        const double scale = clip.properties().value(ParamId::Scale, t);
        const double z = scale / placement.scale;
        if (z <= 0.0) {
            continue;
        }
        const double wx = (placement.positionX - clip.properties().value(ParamId::PositionX, t) / z) / placement.frameWidth;
        const double wy = (placement.positionY - clip.properties().value(ParamId::PositionY, t) / z) / placement.frameHeight;
        const double half = 0.5 / z;
        worst = std::max({worst, std::abs(wx) + half - 0.5, std::abs(wy) + half - 0.5});
    }
    return std::max(0.0, worst);
}

} // namespace wabcut::ai::autozoom
