#include <wabcut/ai/segment/ObjectMask.h>

#include <wabcut/ai/analysis/AnalysisCommands.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace wabcut::ai::segment {

namespace {

std::unexpected<Error> failure(ErrorCode code, std::string message) { return std::unexpected(Error(code, std::move(message))); }

double displayAspectOf(const VideoStreamInfo& v) {
    const double par = v.sampleAspect.isValid() ? v.sampleAspect.toDouble() : 1.0;
    return v.width > 0 && v.height > 0 ? v.width * par / v.height : 16.0 / 9.0;
}

/// The run's part of the cache key: how it went and what it refines.
std::string runKey(const ObjectRequest& r) {
    std::string key(toString(r.direction));
    if (r.acrossShots) {
        key += "+across";
    }
    if (r.base) {
        key += "|" + r.base->identity();
    }
    return key;
}

void splitCuts(const MatteTrack& track, Ticks anchor, ObjectResult& result) {
    for (const Ticks c : track.cuts) {
        if (c <= anchor) {
            result.stoppedBefore = result.stoppedBefore ? std::max(*result.stoppedBefore, c) : c;
        } else {
            result.stoppedAfter = result.stoppedAfter ? std::min(*result.stoppedAfter, c) : c;
        }
    }
}

// ---- Subjects -----------------------------------------------------------------------------------------------------

constexpr std::array<std::string_view, 80> kCoco = {
    "person",        "bicycle",      "car",        "motorcycle",    "airplane",     "bus",         "train",     "truck",
    "boat",          "traffic light", "fire hydrant", "stop sign",   "parking meter", "bench",      "bird",      "cat",
    "dog",           "horse",        "sheep",      "cow",           "elephant",     "bear",        "zebra",     "giraffe",
    "backpack",      "umbrella",     "handbag",    "tie",           "suitcase",     "frisbee",     "skis",      "snowboard",
    "sports ball",   "kite",         "baseball bat", "baseball glove", "skateboard", "surfboard",  "tennis racket", "bottle",
    "wine glass",    "cup",          "fork",       "knife",         "spoon",        "bowl",        "banana",    "apple",
    "sandwich",      "orange",       "broccoli",   "carrot",        "hot dog",      "pizza",       "donut",     "cake",
    "chair",         "couch",        "potted plant", "bed",         "dining table", "toilet",      "tv",        "laptop",
    "mouse",         "remote",       "keyboard",   "cell phone",    "microwave",    "oven",        "toaster",   "sink",
    "refrigerator",  "book",         "clock",      "vase",          "scissors",     "teddy bear",  "hair drier", "toothbrush"};

constexpr std::array<std::string_view, 11> kColours = {"red",  "orange", "yellow", "green", "cyan", "blue",
                                                       "purple", "pink", "white",  "black", "grey"};

/// The named colour a pixel reads as (index into kColours).
int colourOf(std::uint8_t r8, std::uint8_t g8, std::uint8_t b8) {
    const double r = r8 / 255.0, g = g8 / 255.0, b = b8 / 255.0;
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
    const double v = mx, s = mx > 0.0 ? (mx - mn) / mx : 0.0;
    if (v < 0.2) {
        return 9; // black
    }
    if (s < 0.2) {
        return v > 0.8 ? 8 : 10; // white, grey
    }
    double h = 0.0;
    const double d = mx - mn;
    if (mx == r) {
        h = 60.0 * std::fmod((g - b) / d + 6.0, 6.0);
    } else if (mx == g) {
        h = 60.0 * ((b - r) / d + 2.0);
    } else {
        h = 60.0 * ((r - g) / d + 4.0);
    }
    if (h < 15.0 || h >= 345.0) {
        return 0;
    }
    if (h < 40.0) {
        return 1;
    }
    if (h < 70.0) {
        return 2;
    }
    if (h < 160.0) {
        return 3;
    }
    if (h < 200.0) {
        return 4;
    }
    if (h < 255.0) {
        return 5;
    }
    if (h < 290.0) {
        return 6;
    }
    return 7;
}

/// The share of the box's central part (normalised box on `picture`) that reads as colour `colour`.
double colourShare(const search::Image& picture, const MatteBox& box, int colour) {
    const double x0 = box.x + box.width * 0.2, x1 = box.x + box.width * 0.8;
    const double y0 = box.y + box.height * 0.2, y1 = box.y + box.height * 0.8;
    int hits = 0, all = 0;
    for (int j = 0; j < 24; ++j) {
        for (int i = 0; i < 24; ++i) {
            const int x = std::clamp(static_cast<int>((x0 + (x1 - x0) * (i + 0.5) / 24.0) * picture.width), 0, picture.width - 1);
            const int y = std::clamp(static_cast<int>((y0 + (y1 - y0) * (j + 0.5) / 24.0) * picture.height), 0, picture.height - 1);
            const std::size_t at = (static_cast<std::size_t>(y) * picture.width + x) * 3;
            hits += colourOf(picture.rgb[at], picture.rgb[at + 1], picture.rgb[at + 2]) == colour ? 1 : 0;
            ++all;
        }
    }
    return all > 0 ? static_cast<double>(hits) / all : 0.0;
}

/// `picture` (any size, the frame stretched) resampled to its display aspect, `longSide` on the long side.
search::Image unstretched(const search::Image& picture, double aspect, int longSide) {
    const int w = aspect >= 1.0 ? longSide : std::max(1, static_cast<int>(std::lround(longSide * aspect)));
    const int h = aspect >= 1.0 ? std::max(1, static_cast<int>(std::lround(longSide / aspect))) : longSide;
    search::Image out;
    out.width = w;
    out.height = h;
    out.rgb.resize(static_cast<std::size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        const double sy = std::clamp((y + 0.5) * picture.height / h - 0.5, 0.0, picture.height - 1.0);
        const int y0 = static_cast<int>(sy);
        const int y1 = std::min(y0 + 1, picture.height - 1);
        const double fy = sy - y0;
        for (int x = 0; x < w; ++x) {
            const double sx = std::clamp((x + 0.5) * picture.width / w - 0.5, 0.0, picture.width - 1.0);
            const int x0 = static_cast<int>(sx);
            const int x1 = std::min(x0 + 1, picture.width - 1);
            const double fx = sx - x0;
            for (int c = 0; c < 3; ++c) {
                const auto at = [&](int xx, int yy) { return picture.rgb[(static_cast<std::size_t>(yy) * picture.width + xx) * 3 + c]; };
                const double v = (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
                out.rgb[(static_cast<std::size_t>(y) * w + x) * 3 + c] = static_cast<std::uint8_t>(std::clamp(std::lround(v), 0l, 255l));
            }
        }
    }
    return out;
}

// ---- Bounding-box keys --------------------------------------------------------------------------------------------

Keyframe holdKey(Ticks time, double value) {
    Keyframe k;
    k.time = time;
    k.value = value;
    k.interpolation = Interpolation::Hold;
    return k;
}

} // namespace

std::span<const std::string_view> cocoClassNames() { return kCoco; }

std::expected<MatteBox, Error> findSubject(vision::SubjectDetector& detector, const search::Image& picture, double aspect,
                                           std::string_view kind, std::string_view colour, std::string_view pick, std::stop_token stop) {
    const bool face = kind == "face";
    const auto cls = std::ranges::find(kCoco, kind);
    if (!face && cls == kCoco.end()) {
        return failure(ErrorCode::InvalidArgument, "no detector for \"" + std::string(kind) + "\": \"face\" or a COCO class (\"person\", \"car\", \"dog\", ...)");
    }
    const auto named = std::ranges::find(kColours, colour);
    if (!colour.empty() && named == kColours.end()) {
        return failure(ErrorCode::InvalidArgument, "unknown colour \"" + std::string(colour) + "\"");
    }
    if (!pick.empty() && pick != "largest" && pick != "center" && pick != "left" && pick != "right") {
        return failure(ErrorCode::InvalidArgument, "pick is largest, center, left or right");
    }
    if (picture.width <= 0 || picture.height <= 0) {
        return failure(ErrorCode::InvalidArgument, "no picture");
    }
    const search::Image natural = unstretched(picture, aspect > 0.0 ? aspect : 16.0 / 9.0, 640);
    const std::array<search::Image, 1> one{natural};
    auto found = detector.detect(one, stop);
    if (!found) {
        return std::unexpected(found.error());
    }
    if (found->empty()) {
        return failure(ErrorCode::NotFound, "the detector returned nothing");
    }
    const vision::FrameDetections& d = found->front();
    std::vector<MatteBox> boxes;
    const int classId = face ? -1 : static_cast<int>(cls - kCoco.begin());
    const auto addBox = [&](const vision::Detection& det) {
        const vision::Box b = vision::normalized(det.box, static_cast<float>(natural.width), static_cast<float>(natural.height));
        const double x0 = std::clamp<double>(b.x, 0.0, 1.0), y0 = std::clamp<double>(b.y, 0.0, 1.0);
        const double x1 = std::clamp<double>(b.x + b.w, 0.0, 1.0), y1 = std::clamp<double>(b.y + b.h, 0.0, 1.0);
        if (x1 > x0 && y1 > y0) {
            boxes.push_back(MatteBox{x0, y0, x1 - x0, y1 - y0});
        }
    };
    if (face) {
        std::ranges::for_each(d.faces, addBox);
    } else if (!d.objects.empty()) {
        for (const vision::Detection& det : d.objects) {
            if (det.classId == classId) {
                addBox(det);
            }
        }
    } else if (classId == vision::kCocoPerson) {
        std::ranges::for_each(d.people, addBox);
    }
    if (boxes.empty()) {
        return failure(ErrorCode::NotFound, "no " + std::string(kind) + " found on that frame");
    }
    const auto score = [&](const MatteBox& b) {
        const double cx = b.x + b.width / 2.0, cy = b.y + b.height / 2.0;
        if (pick == "center") {
            return -std::hypot((cx - 0.5) * aspect, cy - 0.5);
        }
        if (pick == "left") {
            return -cx;
        }
        if (pick == "right") {
            return cx;
        }
        return b.width * b.height;
    };
    std::vector<MatteBox> ranked = boxes;
    if (!colour.empty()) {
        const int wanted = static_cast<int>(named - kColours.begin());
        double best = -1.0;
        for (const MatteBox& b : boxes) {
            best = std::max(best, colourShare(picture, b, wanted));
        }
        if (best < 0.1) {
            return failure(ErrorCode::NotFound, "no " + std::string(colour) + " " + std::string(kind) + " found on that frame");
        }
        // The boxes nearly as much of that colour as the best one, then `pick` among them.
        std::erase_if(ranked, [&](const MatteBox& b) { return colourShare(picture, b, wanted) < best * 0.75; });
    }
    return *std::ranges::max_element(ranked, {}, score);
}

std::expected<ObjectResult, Error> segmentObject(SegmentServices& s, analysis::AnalysisProvider* shots, const MediaItem& media,
                                                 int videoStream, const ObjectRequest& r, std::stop_token stop,
                                                 const std::function<bool(double, const std::string&)>& checkpoint) {
    if (r.prompts.empty() || (r.prompts.back().points.empty() && !r.prompts.back().box)) {
        return failure(ErrorCode::InvalidArgument, "point at the object or draw a box around it");
    }
    if (videoStream < 0 || videoStream >= static_cast<int>(media.info.video.size())) {
        return failure(ErrorCode::InvalidArgument, "media " + std::to_string(media.id.value) + " has no such video stream");
    }
    if (s.model.empty() || !s.segmenter || !s.frames || !s.fingerprint) {
        return failure(ErrorCode::Unsupported, "no segmentation model is installed here (Settings > AI > Local models: SAM 2.1)");
    }
    auto fingerprint = s.fingerprint(media);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    const double aspect = displayAspectOf(media.info.video[static_cast<std::size_t>(videoStream)]);
    auto [width, height] = matteSize(aspect);
    if (r.base) {
        width = r.base->width;
        height = r.base->height;
    }
    MatteTrack key;
    key.fingerprint = *fingerprint;
    key.stream = videoStream;
    key.model = s.model;
    key.params = promptsHash(r.prompts, r.span.value_or(TimeRange{}), width, height, runKey(r));
    const std::string identity = key.identity();
    const MatteStore store(s.cacheRoot);
    const auto report = [&](SegmentProgress p) {
        if (s.progress) {
            s.progress(p);
        }
    };
    const auto finish = [&](std::shared_ptr<const MatteTrack> track, ObjectResult result) -> std::expected<ObjectResult, Error> {
        result.lost = static_cast<std::size_t>(std::ranges::count_if(track->frames, [](const MatteFrame& f) { return !f.present; }));
        if (!s.cacheRoot.empty() && !result.cached) {
            if (auto put = store.put(*track); !put) {
                return std::unexpected(put.error());
            }
        }
        if (s.remember) {
            s.remember(track);
        }
        result.track = std::move(track);
        report({1.0, "Done", false, {}, 0.0f, false});
        return result;
    };

    if (!s.cacheRoot.empty()) {
        if (auto cached = store.get(identity); cached && *cached) {
            ObjectResult result;
            result.cached = true;
            auto track = std::make_shared<const MatteTrack>(std::move(**cached));
            splitCuts(*track, r.prompts.back().time, result);
            return finish(std::move(track), std::move(result));
        }
    }

    PropagationOptions o;
    o.prompt = r.prompts.back();
    o.direction = r.direction;
    o.span = r.span;
    o.acrossShots = r.acrossShots;
    o.longSide = std::max(width, height);
    if (!r.acrossShots && shots) {
        report({0.0, "Finding the shot's cuts", false, {}, 0.0f, false});
        auto found = shots->shots(media, videoStream, analysis::ShotOptions{}, [&](double) { return !stop.stop_requested(); });
        if (!found) {
            return std::unexpected(found.error());
        }
        for (const analysis::ShotCut& c : found->cuts) {
            o.cuts.push_back(c.time);
        }
    }
    auto frames = s.frames(media, videoStream);
    if (!frames) {
        return std::unexpected(frames.error());
    }
    report({0.0, "Loading the segmentation model", false, {}, 0.0f, false});
    auto segmenter = s.segmenter(stop);
    if (!segmenter) {
        return std::unexpected(segmenter.error());
    }
    if (!*segmenter) {
        return failure(ErrorCode::Unsupported, "no segmentation model");
    }
    double done = 0.0;
    o.progress = [&](double fraction, const std::string& message) {
        done = fraction;
        report({fraction, message, false, {}, 0.0f, false});
        return !checkpoint || checkpoint(fraction, message);
    };
    o.frameDone = [&](const MatteFrame& f) { report({done, {}, true, f.time, f.confidence, f.present}); };
    auto run = propagate(**segmenter, **frames, o, stop);
    if (!run) {
        return std::unexpected(run.error());
    }
    if (std::ranges::none_of(run->track.frames, [](const MatteFrame& f) { return f.present; })) {
        return failure(ErrorCode::NotFound, "the model found no object there: click on it, or draw a box around it");
    }
    MatteTrack track = r.base ? *r.base : MatteTrack{};
    if (r.base) {
        track.merge(run->track);
        track.prompts = r.prompts;
    } else {
        track = std::move(run->track);
        track.prompts = r.prompts;
    }
    track.fingerprint = key.fingerprint;
    track.stream = key.stream;
    track.model = key.model;
    track.params = key.params;
    if (track.frames.size() > kMaxMatteFrames) {
        return failure(ErrorCode::InvalidArgument, "too many frames for one matte track: select within In / Out");
    }
    ObjectResult result;
    result.stoppedBefore = run->stoppedBefore;
    result.stoppedAfter = run->stoppedAfter;
    result.made = run->track.frames.size();
    result.seconds = run->seconds;
    result.framesPerSecond = run->framesPerSecond;
    result.device = run->device;
    return finish(std::make_shared<const MatteTrack>(std::move(track)), std::move(result));
}

void setMatteTrack(Mask& mask, const MatteTrack& track, MediaId media) {
    mask.shape = MaskShape::Matte;
    MaskMatte matte;
    matte.track = track.identity();
    matte.media = media;
    matte.stream = track.stream;
    matte.source = track.span();
    matte.model = track.model;
    mask.matte = std::move(matte);
    mask.path.clear();

    // The bounding box over windows of frames: each key holds the union of its window's boxes (a window where the model
    // lost the object holds the previous one), so the box always contains the object.
    const std::size_t n = track.frames.size();
    const std::size_t window = std::max<std::size_t>(1, (n + kMaxBoxKeys - 1) / kMaxBoxKeys);
    Curve cx, cy, sx, sy;
    std::optional<MatteBox> last;
    for (std::size_t i = 0; i < n; i += window) {
        std::optional<MatteBox> u;
        for (std::size_t k = i; k < std::min(n, i + window); ++k) {
            const MatteFrame& f = track.frames[k];
            if (!f.present || !f.box) {
                continue;
            }
            if (!u) {
                u = f.box;
            } else {
                const double x0 = std::min(u->x, f.box->x), y0 = std::min(u->y, f.box->y);
                const double x1 = std::max(u->x + u->width, f.box->x + f.box->width), y1 = std::max(u->y + u->height, f.box->y + f.box->height);
                u = MatteBox{x0, y0, x1 - x0, y1 - y0};
            }
        }
        if (!u) {
            u = last;
        }
        if (!u) {
            continue;
        }
        last = u;
        const Ticks t = track.frames[i].time;
        cx.keys.push_back(holdKey(t, u->x + u->width / 2.0));
        cy.keys.push_back(holdKey(t, u->y + u->height / 2.0));
        sx.keys.push_back(holdKey(t, std::max(u->width, 1e-3)));
        sy.keys.push_back(holdKey(t, std::max(u->height, 1e-3)));
    }
    if (cx.keys.empty()) {
        // Never seen: the whole frame stands in (a redaction then covers everything, the safe way).
        cx.keys.push_back(holdKey(track.frames.empty() ? Ticks{} : track.frames.front().time, 0.5));
        cy.keys = cx.keys;
        sx.keys = {holdKey(cx.keys.front().time, 1.0)};
        sy.keys = sx.keys;
    }
    mask.set("center.x", ParamValue(std::move(cx)));
    mask.set("center.y", ParamValue(std::move(cy)));
    mask.set("size.x", ParamValue(std::move(sx)));
    mask.set("size.y", ParamValue(std::move(sy)));
}

Mask objectMask(const MatteTrack& track, MediaId media, std::string name, double feather, double expansion) {
    Mask mask;
    mask.name = std::move(name);
    setMatteTrack(mask, track, media);
    mask.set("feather", ParamValue(feather));
    if (expansion != 0.0) {
        mask.set("expansion", ParamValue(expansion));
    }
    return mask;
}

} // namespace wabcut::ai::segment
