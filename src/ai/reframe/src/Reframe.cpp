#include <wabcut/ai/reframe/Reframe.h>

#include <wabcut/ai/JsonAccess.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <numbers>
#include <set>

namespace wabcut::ai::reframe {

namespace {

using Json = nlohmann::json;
using vision::Box;
using vision::SubjectAnalysis;
using vision::SubjectBox;
using vision::SubjectSample;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

int even(double v) { return std::max(2, static_cast<int>(std::lround(v / 2.0)) * 2); }

/// The analysis sample nearest to `time` inside the same shot, if any.
const SubjectSample* nearestSample(const SubjectAnalysis& a, Ticks time) {
    if (a.samples.empty()) {
        return nullptr;
    }
    const std::size_t shot = a.shotOf(time);
    const auto it = std::ranges::lower_bound(a.samples, time, {}, &SubjectSample::time);
    const SubjectSample* best = nullptr;
    std::int64_t bestDistance = std::numeric_limits<std::int64_t>::max();
    for (auto candidate = it == a.samples.begin() ? it : std::prev(it); candidate != a.samples.end() && candidate <= it; ++candidate) {
        if (a.shotOf(candidate->time) != shot) {
            continue;
        }
        const std::int64_t distance = std::abs((candidate->time - time).value);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = &*candidate;
        }
    }
    return best;
}

Box unite(const Box& a, const Box& b) {
    const float x0 = std::min(a.x, b.x);
    const float y0 = std::min(a.y, b.y);
    const float x1 = std::max(a.x + a.w, b.x + b.w);
    const float y1 = std::max(a.y + a.h, b.y + b.h);
    return Box{x0, y0, x1 - x0, y1 - y0};
}

/// The head of a person box: where a vertical crop should look.
Box headOf(const Box& person) { return Box{person.cx() - person.w * 0.3f, person.y, person.w * 0.6f, person.h * 0.3f}; }

const SubjectBox* findTrack(const std::vector<SubjectBox>& boxes, int track) {
    for (const SubjectBox& b : boxes) {
        if (b.track == track) {
            return &b;
        }
    }
    return nullptr;
}

/// The track that matters most over a segment: present the longest, biggest and surest.
std::optional<int> mainTrack(const std::vector<const SubjectSample*>& samples, bool faces) {
    std::map<int, double> weight;
    std::map<int, int> seen;
    for (const SubjectSample* s : samples) {
        if (!s) {
            continue;
        }
        for (const SubjectBox& b : faces ? s->faces : s->people) {
            if (b.track < 0) {
                continue;
            }
            weight[b.track] += std::sqrt(std::max(b.box.area(), 0.0f)) * b.score;
            ++seen[b.track];
        }
    }
    std::optional<int> best;
    double bestWeight = 0.0;
    const int needed = std::max(1, static_cast<int>(samples.size() / 5)); // at least a fifth of the segment
    for (const auto& [track, w] : weight) {
        if (seen[track] >= std::min(needed, 2) && w > bestWeight) {
            bestWeight = w;
            best = track;
        }
    }
    return best;
}

/// Tracks seen in at least `fraction` of the samples (steady faces, not passing false positives).
std::set<int> steadyTracks(const std::vector<const SubjectSample*>& samples, double fraction) {
    std::map<int, int> seen;
    for (const SubjectSample* s : samples) {
        if (s) {
            for (const SubjectBox& b : s->faces) {
                ++seen[b.track];
            }
        }
    }
    std::set<int> out;
    const double needed = std::max(1.0, fraction * static_cast<double>(samples.size()));
    for (const auto& [track, n] : seen) {
        if (track >= 0 && n >= needed) {
            out.insert(track);
        }
    }
    return out;
}

/// The talking face per sample: each face's mouth activity smoothed over about a second, the speaker kept until
/// another one is clearly more active for a while (hysteresis).
std::vector<int> speakers(const std::vector<const SubjectSample*>& samples, double dt) {
    std::set<int> tracks;
    for (const SubjectSample* s : samples) {
        if (s) {
            for (const SubjectBox& b : s->faces) {
                if (b.track >= 0) {
                    tracks.insert(b.track);
                }
            }
        }
    }
    const std::size_t n = samples.size();
    std::map<int, std::vector<double>> smooth;
    const int half = std::max(1, static_cast<int>(std::lround(0.6 / dt)));
    for (const int track : tracks) {
        std::vector<double> raw(n, 0.0);
        for (std::size_t i = 0; i < n; ++i) {
            if (const SubjectBox* b = samples[i] ? findTrack(samples[i]->faces, track) : nullptr) {
                raw[i] = b->activity;
            }
        }
        std::vector<double>& out = smooth[track];
        out.assign(n, 0.0);
        for (std::size_t i = 0; i < n; ++i) {
            double sum = 0.0;
            int count = 0;
            for (int d = -half; d <= half; ++d) {
                const auto j = static_cast<std::ptrdiff_t>(i) + d;
                if (j >= 0 && j < static_cast<std::ptrdiff_t>(n)) {
                    sum += raw[static_cast<std::size_t>(j)];
                    ++count;
                }
            }
            out[i] = sum / std::max(count, 1);
        }
    }
    std::vector<int> who(n, -1);
    int current = -1;
    int challenger = -1;
    int challenged = 0;
    const int patience = std::max(1, static_cast<int>(std::lround(0.6 / dt)));
    for (std::size_t i = 0; i < n; ++i) {
        int loudest = -1;
        double loudness = 0.0;
        for (const auto& [track, values] : smooth) {
            if (values[i] > loudness) {
                loudness = values[i];
                loudest = track;
            }
        }
        if (current < 0) {
            current = loudest;
        } else if (loudest >= 0 && loudest != current) {
            const double mine = smooth[current][i];
            if (loudness > mine * 1.3 + 0.004) {
                challenged = loudest == challenger ? challenged + 1 : 1;
                challenger = loudest;
                if (challenged >= patience) {
                    current = loudest;
                    challenged = 0;
                }
            } else {
                challenged = 0;
            }
        } else {
            challenged = 0;
        }
        who[i] = current;
    }
    return who;
}

/// Bounds of a window centre on one axis: inside the picture, and keeping [subjectLo, subjectHi] in view.
std::pair<double, double> centreBounds(double window, std::optional<std::pair<double, double>> subject, double margin) {
    double lo = window >= 1.0 ? 0.5 : window / 2.0;
    double hi = window >= 1.0 ? 0.5 : 1.0 - window / 2.0;
    if (subject) {
        const auto [a, b] = *subject;
        const double m = margin * window;
        double sLo = b + m - window / 2.0;
        double sHi = a - m + window / 2.0;
        if (sLo > sHi) { // wider than the window: centre on it
            sLo = sHi = 0.5 * (a + b);
        }
        const double nLo = std::max(lo, sLo);
        const double nHi = std::min(hi, sHi);
        if (nLo <= nHi) {
            lo = nLo;
            hi = nHi;
        } else {
            lo = hi = std::clamp(0.5 * (a + b), lo, hi);
        }
    }
    return {lo, hi};
}

} // namespace

// --- Names and options --------------------------------------------------------------------------------------------

std::string_view toString(ReframeMode mode) {
    switch (mode) {
    case ReframeMode::Follow:
        return "follow";
    case ReframeMode::Faces:
        return "faces";
    case ReframeMode::Speaker:
        return "speaker";
    case ReframeMode::Anchor:
        return "anchor";
    }
    return "follow";
}

std::optional<ReframeMode> reframeModeFromString(std::string_view text) {
    for (const ReframeMode m : {ReframeMode::Follow, ReframeMode::Faces, ReframeMode::Speaker, ReframeMode::Anchor}) {
        if (toString(m) == text) {
            return m;
        }
    }
    return std::nullopt;
}

std::string_view toString(FitMode mode) {
    switch (mode) {
    case FitMode::Crop:
        return "crop";
    case FitMode::Letterbox:
        return "letterbox";
    case FitMode::BlurFill:
        return "blur";
    }
    return "crop";
}

std::optional<FitMode> fitModeFromString(std::string_view text) {
    for (const FitMode m : {FitMode::Crop, FitMode::Letterbox, FitMode::BlurFill}) {
        if (toString(m) == text) {
            return m;
        }
    }
    return std::nullopt;
}

std::optional<Aspect> aspectFromString(std::string_view text) {
    const auto colon = text.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    Aspect a;
    const auto w = std::from_chars(text.data(), text.data() + colon, a.width);
    const auto h = std::from_chars(text.data() + colon + 1, text.data() + text.size(), a.height);
    if (w.ec != std::errc{} || h.ec != std::errc{} || w.ptr != text.data() + colon || h.ptr != text.data() + text.size() ||
        a.width < 1 || a.height < 1 || a.width > 100 || a.height > 100) {
        return std::nullopt;
    }
    return a;
}

std::string toString(Aspect aspect) { return std::to_string(aspect.width) + ":" + std::to_string(aspect.height); }

std::string optionsToJson(const ReframeOptions& o) {
    Json j{{"aspect", toString(o.aspect)},
           {"mode", toString(o.mode)},
           {"fit", toString(o.fit)},
           {"smoothness", std::round(o.smoothness * 1000.0) / 1000.0},
           {"margin", std::round(o.margin * 1000.0) / 1000.0}};
    if (o.mode == ReframeMode::Anchor) {
        j["anchor"] = Json::array({std::round(o.anchorX * 1e4) / 1e4, std::round(o.anchorY * 1e4) / 1e4});
    }
    if (o.width > 0 && o.height > 0) {
        j["width"] = o.width;
        j["height"] = o.height;
    }
    return j.dump();
}

std::expected<ReframeOptions, std::string> optionsFromJson(std::string_view json) {
    const Json j = Json::parse(json, nullptr, false);
    if (!j.is_object()) {
        return std::unexpected(std::string("the reframe parameters are not an object"));
    }
    ReframeOptions o;
    const auto aspect = aspectFromString(jsonStringOr(j, "aspect", "9:16"));
    const auto mode = reframeModeFromString(jsonStringOr(j, "mode", "follow"));
    const auto fit = fitModeFromString(jsonStringOr(j, "fit", "crop"));
    if (!aspect || !mode || !fit) {
        return std::unexpected(std::string("the reframe parameters name an unknown aspect, mode or fit"));
    }
    o.aspect = *aspect;
    o.mode = *mode;
    o.fit = *fit;
    o.smoothness = std::clamp(jsonNumberOr(j, "smoothness", 0.5), 0.0, 1.0);
    o.margin = std::clamp(jsonNumberOr(j, "margin", 0.04), 0.0, 0.4);
    if (const Json* anchor = jsonArray(j, "anchor"); anchor && anchor->size() == 2) {
        o.anchorX = std::clamp(asNumber(&(*anchor)[0]).value_or(0.5), 0.0, 1.0);
        o.anchorY = std::clamp(asNumber(&(*anchor)[1]).value_or(0.5), 0.0, 1.0);
    }
    o.width = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(j, "width", 0), 0, 16384));
    o.height = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(j, "height", 0), 0, 16384));
    return o;
}

std::pair<int, int> outputSize(int sourceWidth, int sourceHeight, const ReframeOptions& options) {
    if (options.width > 0 && options.height > 0) {
        return {even(options.width), even(options.height)};
    }
    const double shortSide = std::max(2, std::min(sourceWidth, sourceHeight));
    const Aspect a = options.aspect;
    if (a.width == a.height) {
        return {even(shortSide), even(shortSide)};
    }
    if (a.width < a.height) {
        return {even(shortSide), even(shortSide * a.height / a.width)};
    }
    return {even(shortSide * a.width / a.height), even(shortSide)};
}

DisplayGeometry displayGeometry(const VideoStreamInfo& stream) {
    const double sar = stream.sampleAspect.num > 0 && stream.sampleAspect.den > 0
                           ? static_cast<double>(stream.sampleAspect.num) / static_cast<double>(stream.sampleAspect.den)
                           : 1.0;
    const double w = static_cast<double>(stream.width) * sar;
    const double h = static_cast<double>(stream.height);
    const int rotation = ((stream.rotationDegrees % 360) + 360) % 360;
    return rotation == 90 || rotation == 270 ? DisplayGeometry{h, w} : DisplayGeometry{w, h};
}

// --- Planning -----------------------------------------------------------------------------------------------------

ClipPlan planClip(const SubjectAnalysis* analysis, TimeRange sourceRange, DisplayGeometry display, int targetWidth, int targetHeight,
                  const ReframeOptions& options) {
    ClipPlan plan;
    if (display.width <= 0.0 || display.height <= 0.0 || targetWidth <= 0 || targetHeight <= 0) {
        display = DisplayGeometry{static_cast<double>(std::max(targetWidth, 1)), static_cast<double>(std::max(targetHeight, 1))};
    }
    const double target = static_cast<double>(targetWidth) / static_cast<double>(targetHeight);
    const double picture = display.width / display.height;
    plan.windowWidth = picture > target ? target / picture : 1.0;
    plan.windowHeight = picture > target ? 1.0 : picture / target;
    const double maxZoom = std::min(std::max(1.0 / plan.windowWidth, 1.0 / plan.windowHeight), 4.0);
    const bool canZoom = options.fit != FitMode::Crop;

    // Segments: the source range cut at the shot cuts inside it.
    std::vector<Ticks> bounds{sourceRange.start};
    if (analysis) {
        for (const Ticks cut : analysis->cuts) {
            if (cut > sourceRange.start && cut < sourceRange.end()) {
                bounds.push_back(cut);
                plan.cuts.push_back(cut);
            }
        }
    }
    bounds.push_back(std::max(sourceRange.end(), sourceRange.start + Ticks{1}));
    std::set<std::string> guides;
    const PathWeights weights = weightsForSmoothness(options.smoothness);
    const double fps = std::clamp(options.gridFps, 1.0, 30.0);

    for (std::size_t seg = 0; seg + 1 < bounds.size(); ++seg) {
        const Ticks from = bounds[seg];
        const Ticks to = bounds[seg + 1];
        const double length = (to - from).toSeconds();
        const auto n = static_cast<std::size_t>(std::clamp(std::floor(length * fps), 1.0, 1e6));
        const double dt = length / static_cast<double>(n);
        std::vector<Ticks> times(n);
        std::vector<const SubjectSample*> samples(n, nullptr);
        for (std::size_t i = 0; i < n; ++i) {
            times[i] = from + Ticks{static_cast<std::int64_t>(std::llround((static_cast<double>(i) + 0.5) * dt *
                                                                            static_cast<double>(kTicksPerSecond)))};
            times[i] = std::clamp(times[i], from, to - Ticks{1});
            samples[i] = analysis ? nearestSample(*analysis, times[i]) : nullptr;
        }

        // What the window should show at each sample: a box (the safe region) and how much it matters.
        std::vector<std::optional<Box>> wanted(n);
        std::vector<double> weight(n, 1.0);
        std::string guide = "centre";
        if (options.mode == ReframeMode::Anchor) {
            guide = "anchor";
        } else {
            bool done = false;
            if (options.mode == ReframeMode::Faces) {
                const std::set<int> steady = steadyTracks(samples, 0.25);
                for (std::size_t i = 0; i < n && !steady.empty(); ++i) {
                    if (!samples[i]) {
                        continue;
                    }
                    for (const SubjectBox& b : samples[i]->faces) {
                        if (steady.contains(b.track)) {
                            wanted[i] = wanted[i] ? unite(*wanted[i], b.box) : b.box;
                        }
                    }
                }
                done = !steady.empty();
                if (done) {
                    guide = "faces";
                }
            } else if (options.mode == ReframeMode::Speaker) {
                const std::vector<int> who = speakers(samples, dt);
                for (std::size_t i = 0; i < n; ++i) {
                    if (const SubjectBox* b = samples[i] && who[i] >= 0 ? findTrack(samples[i]->faces, who[i]) : nullptr) {
                        wanted[i] = b->box;
                        done = true;
                    }
                }
                if (done) {
                    guide = "faces";
                }
            }
            if (!done) { // follow (and the fallback of the others): the shot's main face, else its main person
                if (const auto face = mainTrack(samples, true)) {
                    for (std::size_t i = 0; i < n; ++i) {
                        if (const SubjectBox* b = samples[i] ? findTrack(samples[i]->faces, *face) : nullptr) {
                            wanted[i] = b->box;
                        }
                    }
                    guide = "faces";
                    done = true;
                } else if (const auto person = mainTrack(samples, false)) {
                    for (std::size_t i = 0; i < n; ++i) {
                        if (const SubjectBox* b = samples[i] ? findTrack(samples[i]->people, *person) : nullptr) {
                            wanted[i] = headOf(b->box);
                        }
                    }
                    guide = "people";
                    done = true;
                }
            }
            if (!done) { // nothing detected: lean towards the salient region, without holding the window to it
                for (std::size_t i = 0; i < n; ++i) {
                    if (samples[i] && samples[i]->salient) {
                        wanted[i] = *samples[i]->salient;
                        weight[i] = 0.3;
                        guide = "saliency";
                    }
                }
            }
        }
        guides.insert(guide);
        const bool hard = guide == "faces" || guide == "people";

        // Zoom out (letterbox, blur fill) when the subjects cannot fit; one zoom per shot.
        double zoom = 1.0;
        if (canZoom && hard) {
            for (const auto& box : wanted) {
                if (box) {
                    const double needX = (box->w + 2.0 * options.margin * plan.windowWidth) / plan.windowWidth;
                    const double needY = (box->h + 2.0 * options.margin * plan.windowHeight) / plan.windowHeight;
                    zoom = std::max({zoom, needX, needY});
                }
            }
            zoom = std::min(zoom, maxZoom);
        }
        const double windowX = plan.windowWidth * zoom;
        const double windowY = plan.windowHeight * zoom;

        PathProblem px;
        PathProblem py;
        px.dt = py.dt = dt;
        for (std::size_t i = 0; i < n; ++i) {
            double dx = kNaN;
            double dy = kNaN;
            std::optional<std::pair<double, double>> sx;
            std::optional<std::pair<double, double>> sy;
            if (options.mode == ReframeMode::Anchor) {
                dx = options.anchorX;
                dy = options.anchorY;
            } else if (wanted[i]) {
                const Box& b = *wanted[i];
                dx = b.cx();
                // Faces sit in the upper part of a tall window (eyes near the top third), not dead centre.
                dy = hard ? b.cy() + 0.12 * windowY : b.cy();
                if (hard) {
                    sx = std::pair<double, double>{b.x, b.x + b.w};
                    sy = std::pair<double, double>{b.y, b.y + b.h};
                }
            }
            const auto [xl, xh] = centreBounds(windowX, sx, options.margin);
            const auto [yl, yh] = centreBounds(windowY, sy, options.margin);
            px.desired.push_back(dx);
            py.desired.push_back(dy);
            px.weight.push_back(std::isfinite(dx) ? weight[i] : 0.0);
            py.weight.push_back(std::isfinite(dy) ? weight[i] : 0.0);
            px.lower.push_back(xl);
            px.upper.push_back(xh);
            py.lower.push_back(yl);
            py.upper.push_back(yh);
        }
        // With no preference at all the window rests in the middle.
        if (std::ranges::none_of(px.weight, [](double w) { return w > 0.0; })) {
            std::ranges::fill(px.desired, 0.5);
            std::ranges::fill(px.weight, 0.05);
        }
        if (std::ranges::none_of(py.weight, [](double w) { return w > 0.0; })) {
            std::ranges::fill(py.desired, 0.5);
            std::ranges::fill(py.weight, 0.05);
        }
        const std::vector<double> x = solveCameraPath(px, weights);
        const std::vector<double> y = solveCameraPath(py, weights);
        for (std::size_t i = 0; i < n; ++i) {
            plan.times.push_back(times[i]);
            plan.centreX.push_back(x[i]);
            plan.centreY.push_back(y[i]);
            plan.zoom.push_back(zoom);
            plan.lowerX.push_back(px.lower[i]);
            plan.upperX.push_back(px.upper[i]);
            plan.lowerY.push_back(py.lower[i]);
            plan.upperY.push_back(py.upper[i]);
            plan.subjects.push_back(hard ? wanted[i] : std::nullopt);
        }
        plan.zoomedOut = plan.zoomedOut || windowX > 1.0 + 1e-9 || windowY > 1.0 + 1e-9;
    }
    // The most telling guide of the clip.
    for (const char* g : {"faces", "people", "saliency", "anchor", "centre"}) {
        if (guides.contains(g)) {
            plan.guide = g;
            break;
        }
    }
    return plan;
}

// --- The Transform ---------------------------------------------------------------------------------------------------

std::size_t applyPlan(Clip& clip, const ClipPlan& plan, DisplayGeometry display, int targetWidth, int targetHeight,
                      const ReframeOptions& options) {
    ClipProperties props = clip.properties();
    props.set(ParamId::AnchorX, 0.0);
    props.set(ParamId::AnchorY, 0.0);
    const std::size_t n = plan.times.size();
    if (n == 0 || display.width <= 0.0 || display.height <= 0.0) {
        clip.setProperties(props);
        return 0;
    }
    const double fill = static_cast<double>(targetHeight) / (plan.windowHeight * display.height);
    (void)targetWidth;

    // Segments between cuts, each with a first sample at its very start (so a new shot's framing starts on its first
    // frame) and a last key that holds until the next shot.
    std::vector<std::size_t> starts{0};
    std::vector<Ticks> startTimes{plan.times[0]};
    for (std::size_t i = 1; i < n; ++i) {
        for (const Ticks cut : plan.cuts) {
            if (plan.times[i - 1] < cut && plan.times[i] >= cut) {
                starts.push_back(i);
                startTimes.push_back(cut);
                break;
            }
        }
    }
    starts.push_back(n);
    Curve xCurve;
    Curve yCurve;
    Curve scaleCurve;
    for (std::size_t s = 0; s + 1 < starts.size(); ++s) {
        const std::size_t a = starts[s];
        const std::size_t b = starts[s + 1];
        const Ticks segmentStart = startTimes[s];
        std::vector<Ticks> times;
        std::vector<double> xs;
        std::vector<double> ys;
        std::vector<double> xLo;
        std::vector<double> xHi;
        std::vector<double> yLo;
        std::vector<double> yHi;
        const double scale = fill / plan.zoom[a];
        const auto push = [&](Ticks t, std::size_t i) {
            const double rotation = clip.properties().value(ParamId::Rotation, t) * std::numbers::pi / 180.0;
            const double cs = std::cos(rotation);
            const double sn = std::sin(rotation);
            const auto position = [&](double cx, double cy) {
                const double ox = (cx - 0.5) * display.width;
                const double oy = (cy - 0.5) * display.height;
                return std::pair<double, double>{-scale * (cs * ox - sn * oy), -scale * (sn * ox + cs * oy)};
            };
            const auto [x, y] = position(plan.centreX[i], plan.centreY[i]);
            times.push_back(t);
            xs.push_back(x);
            ys.push_back(y);
            if (rotation == 0.0) { // the position falls as the centre rises: the bounds swap
                xLo.push_back(position(plan.upperX[i], 0.5).first);
                xHi.push_back(position(plan.lowerX[i], 0.5).first);
                yLo.push_back(position(0.5, plan.upperY[i]).second);
                yHi.push_back(position(0.5, plan.lowerY[i]).second);
            }
        };
        if (segmentStart < plan.times[a]) {
            push(segmentStart, a);
        }
        for (std::size_t i = a; i < b; ++i) {
            push(plan.times[i], i);
        }
        const bool last = s + 2 == starts.size();
        const bool bounded = xLo.size() == times.size();
        const Curve x = fitCurve(times, xs, options.tolerancePixels, bounded ? std::span<const double>(xLo) : std::span<const double>{},
                                 bounded ? std::span<const double>(xHi) : std::span<const double>{}, !last);
        const Curve y = fitCurve(times, ys, options.tolerancePixels, bounded ? std::span<const double>(yLo) : std::span<const double>{},
                                 bounded ? std::span<const double>(yHi) : std::span<const double>{}, !last);
        xCurve.keys.insert(xCurve.keys.end(), x.keys.begin(), x.keys.end());
        yCurve.keys.insert(yCurve.keys.end(), y.keys.begin(), y.keys.end());
        scaleCurve.keys.push_back(Keyframe{times.front(), scale, Interpolation::Hold, {}, {}});
    }
    // A constant stays a constant: an editor sees keyframes only where the framing moves.
    const auto finish = [](Curve curve) -> ParamValue {
        const bool flat = std::ranges::all_of(curve.keys, [&](const Keyframe& k) { return k.value == curve.keys.front().value; });
        if (curve.keys.size() == 1 || flat) {
            return ParamValue(curve.keys.front().value);
        }
        return ParamValue(std::move(curve));
    };
    props[ParamId::PositionX] = finish(std::move(xCurve));
    props[ParamId::PositionY] = finish(std::move(yCurve));
    props[ParamId::Scale] = finish(std::move(scaleCurve));
    clip.setProperties(props);
    std::size_t keys = 0;
    for (const ParamId id : {ParamId::PositionX, ParamId::PositionY, ParamId::Scale}) {
        keys += props[id].curve() ? props[id].curve()->keys.size() : 0;
    }
    return keys;
}

// --- Nested and multicam clips (0.2) ---------------------------------------------------------------------------------

CanvasPlacement canvasPlacement(const Clip& clip, DisplayGeometry picture, double canvasWidth, double canvasHeight, Ticks time) {
    const ClipProperties& p = clip.properties();
    const double scale = p.value(ParamId::Scale, time);
    CanvasPlacement out;
    if (canvasWidth <= 0.0 || canvasHeight <= 0.0) {
        return out;
    }
    out.factorX = scale * picture.width / canvasWidth;
    out.factorY = scale * picture.height / canvasHeight;
    out.offsetX = (canvasWidth / 2.0 + p.value(ParamId::PositionX, time) - scale * (picture.width / 2.0 + p.value(ParamId::AnchorX, time))) / canvasWidth;
    out.offsetY = (canvasHeight / 2.0 + p.value(ParamId::PositionY, time) - scale * (picture.height / 2.0 + p.value(ParamId::AnchorY, time))) / canvasHeight;
    return out;
}

const Track* nestedPictureTrack(const ProjectState& state, const Clip& clip) {
    const Sequence* inner = clip.isNested() && clip.stream.kind == StreamKind::Video ? state.findSequence(clip.sequence) : nullptr;
    if (!inner) {
        return nullptr;
    }
    if (inner->multicam) {
        const std::size_t angle = clip.angles.first();
        return angle < inner->multicam->angles.size() ? inner->findTrack(inner->multicam->angles[angle].video) : nullptr;
    }
    const Track* track = nullptr;
    for (const TrackPtr& candidate : inner->videoTracks) {
        if (candidate->clips.empty() || !candidate->enabled) {
            continue;
        }
        if (track) {
            return nullptr;
        }
        track = candidate.get();
    }
    return track;
}

namespace {

using AnalysisOf = std::function<const SubjectAnalysis*(const MediaItem&, int stream)>;

/// The subjects of what a nested or multicam video clip shows, in its nested sequence's time and on its canvas: the
/// samples of the media clips on its picture track inside the clip's window, moved to the nested time each shows them
/// at, their boxes placed through the leaf clip's Motion (Position, Scale, Anchor at that time; rotation is not
/// followed). A new leaf clip starts a new shot. Retimed leaves and media without analysis are left out (warnings).
std::optional<SubjectAnalysis> nestedSubjects(const ProjectState& state, const Clip& clip, const AnalysisOf& analysisOf,
                                              std::vector<std::string>& warnings) {
    const Sequence* inner = state.findSequence(clip.sequence);
    const Track* track = nestedPictureTrack(state, clip);
    if (!inner || inner->width <= 0 || inner->height <= 0) {
        return std::nullopt;
    }
    const std::string label = "'" + (clip.name().empty() ? inner->name : clip.name()) + "'";
    if (!track) {
        warnings.push_back(label + " shows layers of a nested sequence: its subjects are not followed (it stays centred)");
        return std::nullopt;
    }
    const Ticks windowStart = clip.isFreeze() ? clip.sourceIn : std::min(clip.sourceIn, clip.sourceOut());
    const Ticks windowEnd = clip.isFreeze() ? clip.sourceIn + Ticks{1} : std::max(clip.sourceIn, clip.sourceOut());
    const double cw = static_cast<double>(inner->width);
    const double ch = static_cast<double>(inner->height);
    SubjectAnalysis out;
    out.frameWidth = inner->width;
    out.frameHeight = inner->height;
    out.start = windowStart;
    out.end = windowEnd;
    bool any = false;
    for (const Clip& leaf : track->clips) {
        if (!leaf.enabled || leaf.isNested() || leaf.end() <= windowStart || leaf.start() >= windowEnd) {
            continue;
        }
        const MediaItem* media = state.findMedia(leaf.media);
        if (!media || media->isGenerator() || media->info.video.empty() || leaf.stream.kind != StreamKind::Video) {
            continue;
        }
        if (leaf.speed.num != leaf.speed.den || leaf.reversed || leaf.isRemapped() || leaf.isFreeze()) {
            warnings.push_back("'" + media->name + "' is retimed inside " + label + ": its subjects are not followed there");
            continue;
        }
        const int stream = std::clamp(leaf.stream.index, 0, static_cast<int>(media->info.video.size()) - 1);
        const SubjectAnalysis* a = analysisOf(*media, stream);
        if (!a) {
            continue;
        }
        any = true;
        if (out.models.empty()) {
            out.models = a->models;
        }
        const DisplayGeometry d = displayGeometry(media->info.video[static_cast<std::size_t>(stream)]);
        const Ticks from = std::max(leaf.start(), windowStart);
        const Ticks to = std::min(leaf.end(), windowEnd);
        if (from > windowStart) {
            out.cuts.push_back(from); // another clip of the angle: a new shot
        }
        const auto nestedTime = [&](Ticks mediaTime) { return leaf.start() + (mediaTime - leaf.sourceIn); };
        for (const Ticks cut : a->cuts) {
            if (const Ticks t = nestedTime(cut); t > from && t < to) {
                out.cuts.push_back(t);
            }
        }
        for (const SubjectSample& s : a->samples) {
            const Ticks t = nestedTime(s.time);
            if (t < from || t >= to) {
                continue;
            }
            const CanvasPlacement on = canvasPlacement(leaf, d, cw, ch, leaf.curveTimeAt(t));
            const auto place = [&](const Box& b) {
                return Box{static_cast<float>(on.offsetX + on.factorX * b.x), static_cast<float>(on.offsetY + on.factorY * b.y),
                           static_cast<float>(on.factorX * b.w), static_cast<float>(on.factorY * b.h)};
            };
            SubjectSample moved = s;
            moved.time = t;
            for (std::vector<SubjectBox>* boxes : {&moved.faces, &moved.people}) {
                for (SubjectBox& b : *boxes) {
                    b.box = place(b.box);
                }
            }
            if (moved.salient) {
                moved.salient = place(*moved.salient);
            }
            out.samples.push_back(std::move(moved));
        }
    }
    if (!any) {
        return std::nullopt;
    }
    std::ranges::stable_sort(out.samples, {}, &SubjectSample::time);
    std::ranges::sort(out.cuts);
    out.cuts.erase(std::unique(out.cuts.begin(), out.cuts.end()), out.cuts.end());
    return out;
}

} // namespace

// --- Copying a sequence ----------------------------------------------------------------------------------------------

std::expected<SequenceCopy, Error> duplicateSequence(const ProjectState& state, SequenceId source, std::string name, int width,
                                                     int height) {
    const Sequence* original = state.findSequence(source);
    if (!original) {
        return std::unexpected(Error(ErrorCode::NotFound, "no sequence " + std::to_string(source.value)));
    }
    SequenceCopy out;
    out.state = state;
    IdGenerator& ids = out.state.ids;
    const auto copyEffects = [&](const EffectStack& stack) {
        EffectStack copy = stack;
        for (EffectInstance& e : copy) {
            e.id = ids.allocate<EffectInstanceId>();
        }
        return copy;
    };
    auto seq = std::make_shared<Sequence>(*original);
    seq->id = ids.allocate<SequenceId>();
    seq->name = std::move(name);
    seq->width = width;
    seq->height = height;
    seq->effects = copyEffects(original->effects);
    seq->audioEffects = copyEffects(original->audioEffects);
    seq->derivedFrom.reset();
    std::map<std::uint64_t, LinkGroupId> groups;
    const auto copyTrack = [&](const TrackPtr& track) {
        auto t = std::make_shared<Track>(*track);
        t->id = ids.allocate<TrackId>();
        t->effects = copyEffects(track->effects);
        std::vector<Clip> clips = track->clips.toVector();
        std::map<std::uint64_t, ClipId> local;
        for (Clip& clip : clips) {
            const ClipId fresh = ids.allocate<ClipId>();
            out.clips[clip.id.value] = fresh;
            local[clip.id.value] = fresh;
            clip.id = fresh;
            if (clip.linkGroup.isValid()) {
                auto [it, inserted] = groups.try_emplace(clip.linkGroup.value, LinkGroupId{});
                if (inserted) {
                    it->second = ids.allocate<LinkGroupId>();
                }
                clip.linkGroup = it->second;
            }
            if (!clip.effects().empty()) {
                clip.setEffects(copyEffects(clip.effects()));
            }
        }
        t->clips = ClipList(std::move(clips));
        for (Transition& tr : t->transitions) {
            tr.id = ids.allocate<TransitionId>();
            if (tr.outgoing.isValid()) {
                tr.outgoing = local.at(tr.outgoing.value);
            }
            if (tr.incoming.isValid()) {
                tr.incoming = local.at(tr.incoming.value);
            }
        }
        return TrackPtr(std::move(t));
    };
    for (TrackPtr& t : seq->videoTracks) {
        t = copyTrack(t);
    }
    for (TrackPtr& t : seq->audioTracks) {
        t = copyTrack(t);
    }
    for (CaptionTrack& captions : seq->captionTracks) {
        captions.id = ids.allocate<CaptionTrackId>();
    }
    out.sequence = seq->id;
    out.state.sequences.push_back(std::move(seq)); // the newest id: the list stays sorted
    return out;
}

// --- All of it -------------------------------------------------------------------------------------------------------

std::expected<ReframeResult, Error> reframeSequence(const ProjectState& state, SequenceId source, const ReframeOptions& options,
                                                    const SubjectSource& subjects, std::optional<SequenceId> replace) {
    const Sequence* original = state.findSequence(source);
    if (!original) {
        return std::unexpected(Error(ErrorCode::NotFound, "no sequence " + std::to_string(source.value)));
    }
    const Sequence* replaced = replace ? state.findSequence(*replace) : nullptr;
    if (replace && !replaced) {
        return std::unexpected(Error(ErrorCode::NotFound, "no sequence " + std::to_string(replace->value) + " to make again"));
    }
    if (!(options.smoothness >= 0.0 && options.smoothness <= 1.0) || options.aspect.width < 1 || options.aspect.height < 1) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "reframe options out of range"));
    }
    const auto [width, height] = outputSize(original->width, original->height, options);
    std::string name = replaced ? replaced->name : !options.name.empty() ? options.name : original->name + " (" + toString(options.aspect) + ")";
    auto copy = duplicateSequence(state, source, std::move(name), width, height);
    if (!copy) {
        return std::unexpected(copy.error());
    }
    ReframeResult result;
    result.sequence = copy->sequence;
    ProjectState next = std::move(copy->state);
    auto seq = std::make_shared<Sequence>(*next.findSequence(copy->sequence));
    seq->derivedFrom = SequenceDerivation{source, std::string(kGenerator), optionsToJson(options)};

    std::map<std::pair<std::uint64_t, int>, std::optional<vision::SubjectAnalysis>> analyses;
    std::set<std::uint64_t> warned;
    // A media stream's subjects, analysed once (null: none, a warning says why; Anchor mode needs none).
    const AnalysisOf analysisOf = [&](const MediaItem& media, int stream) -> const SubjectAnalysis* {
        auto& analysis = analyses[{media.id.value, stream}];
        if (!analysis && options.mode != ReframeMode::Anchor && subjects) {
            auto found = subjects(media, stream);
            if (found) {
                analysis = std::move(*found);
            } else if (warned.insert(media.id.value).second) {
                result.warnings.push_back("'" + media.name + "': no subject analysis (" + found.error().message + "); its clips stay centred");
            }
        }
        return analysis ? &*analysis : nullptr;
    };
    std::vector<Clip> blurred;
    for (TrackPtr& trackPtr : seq->videoTracks) {
        auto track = std::make_shared<Track>(*trackPtr);
        std::vector<Clip> clips = track->clips.toVector();
        bool changed = false;
        for (Clip& clip : clips) {
            DisplayGeometry display;
            const SubjectAnalysis* analysis = nullptr;
            std::optional<SubjectAnalysis> nestedAnalysis;
            if (clip.isNested()) {
                // 0.2: a nested or multicam clip is one picture, its nested sequence's canvas (the active angle for a
                // multicam clip): it gets the camera keyframes itself - angles stay switchable, the nest untouched - from
                // the subjects of the media it shows, mapped through the nest's time and placement.
                const Sequence* inner = next.findSequence(clip.sequence);
                if (clip.stream.kind != StreamKind::Video || !inner || inner->width <= 0 || inner->height <= 0) {
                    continue;
                }
                display = DisplayGeometry{static_cast<double>(inner->width), static_cast<double>(inner->height)};
                if (options.mode != ReframeMode::Anchor && subjects) {
                    nestedAnalysis = nestedSubjects(next, clip, analysisOf, result.warnings);
                    analysis = nestedAnalysis ? &*nestedAnalysis : nullptr;
                }
            } else {
                const MediaItem* media = next.findMedia(clip.media);
                if (!media || media->isGenerator() || clip.stream.kind != StreamKind::Video || media->info.video.empty()) {
                    continue; // titles and generated media keep their own layout
                }
                const int stream = std::clamp(clip.stream.index, 0, static_cast<int>(media->info.video.size()) - 1);
                display = displayGeometry(media->info.video[static_cast<std::size_t>(stream)]);
                analysis = analysisOf(*media, stream);
            }
            const TimeRange range = clip.isFreeze() ? TimeRange{clip.sourceIn, Ticks{1}}
                                                    : TimeRange::fromStartEnd(clip.sourceIn, clip.sourceOut());
            ClipPlan plan = planClip(analysis, range, display, width, height, options);
            result.keyframes += applyPlan(clip, plan, display, width, height, options);
            ++result.clipsReframed;
            changed = true;
            if (plan.zoomedOut && options.fit == FitMode::BlurFill) {
                Clip background = clip;
                ClipProperties props;
                props.set(ParamId::Scale, std::max(static_cast<double>(width) / display.width, static_cast<double>(height) / display.height));
                background.setProperties(props);
                EffectStack effects = background.effects();
                EffectInstance blur;
                blur.effect = "wabcut.blur.gaussian";
                blur.set("radius", ParamValue(std::max(8.0, 0.025 * static_cast<double>(height))));
                effects.push_back(std::move(blur));
                background.setEffects(std::move(effects));
                background.linkGroup = LinkGroupId{};
                background.setName(clip.name().empty() ? std::string("Blur fill") : clip.name() + " (blur fill)");
                blurred.push_back(std::move(background));
            }
            result.plans[clip.id.value] = std::move(plan);
        }
        if (changed) {
            track->clips = ClipList(std::move(clips));
            trackPtr = std::move(track);
        }
    }
    if (!blurred.empty()) {
        // One track under everything; clips that would overlap there (stacked letterboxed clips) keep bars.
        std::ranges::stable_sort(blurred, [](const Clip& a, const Clip& b) { return a.start() < b.start(); });
        std::vector<Clip> placed;
        for (Clip& clip : blurred) {
            if (!placed.empty() && clip.start() < placed.back().end()) {
                result.warnings.push_back("a blur fill was skipped where letterboxed clips overlap");
                continue;
            }
            clip.id = next.ids.allocate<ClipId>();
            EffectStack effects = clip.effects();
            for (EffectInstance& e : effects) {
                e.id = next.ids.allocate<EffectInstanceId>();
            }
            clip.setEffects(std::move(effects));
            placed.push_back(std::move(clip));
        }
        auto track = std::make_shared<Track>();
        track->id = next.ids.allocate<TrackId>();
        track->kind = TrackKind::Video;
        track->name = "Blur fill";
        track->clips = ClipList(std::move(placed));
        seq->videoTracks.insert(seq->videoTracks.begin(), std::move(track));
    }

    if (replaced) { // made again in place: the old sequence's id, name and bin
        seq->id = replaced->id;
        seq->bin = replaced->bin;
        std::erase_if(next.sequences, [&](const SequencePtr& s) { return s->id == copy->sequence || s->id == replaced->id; });
        const auto at = std::ranges::lower_bound(next.sequences, seq->id, {}, [](const SequencePtr& s) { return s->id; });
        next.sequences.insert(at, std::move(seq));
        result.sequence = replaced->id;
    } else {
        next = next.withSequence(std::move(seq));
    }
    result.state = std::move(next);
    return result;
}

} // namespace wabcut::ai::reframe
