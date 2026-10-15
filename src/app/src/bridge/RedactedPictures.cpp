#include "RedactedPictures.h"

#include <wabcut/model/RedactionMedia.h>
#include <wabcut/platform/AtomicSharedPtr.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/render/Mask.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace wabcut::app::redacted {

namespace {

wabcut::platform::AtomicSharedPtr<const ProjectState>& slot() {
    static wabcut::platform::AtomicSharedPtr<const ProjectState> project;
    return project;
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    return (h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2))) * 0x100000001B3ull;
}

std::uint64_t hashDouble(double v) {
    std::uint64_t bits = 0;
    static_assert(sizeof bits == sizeof v);
    std::memcpy(&bits, &v, sizeof bits);
    return bits;
}

/// The stream a picture of `item` shows: its first video stream.
const VideoStreamInfo* streamOf(const MediaItem& item) {
    return item.info.video.empty() ? nullptr : &item.info.video.front();
}

/// Whether `r` marks the frame on screen at media time `t` (a frame of the stream's rate around it).
bool covers(const Redaction& r, Ticks t, Rate rate) {
    if (!r.enabled) {
        return false;
    }
    Ticks frame{1};
    if (rate.num > 0 && rate.den > 0) {
        frame = framesToTicks(1, rate, Rounding::Ceil).value_or(Ticks{1});
    }
    return r.source.overlaps(TimeRange{t - frame, frame + frame});
}

/// Separable box blur of RGBA8 rows, `radius` px, three passes (close to a Gaussian of sigma ~ radius).
void boxBlur(std::vector<std::uint8_t>& rgba, int w, int h, int radius) {
    if (radius < 1) {
        return;
    }
    std::vector<std::uint8_t> tmp(rgba.size());
    const auto pass = [&](const std::vector<std::uint8_t>& src, std::vector<std::uint8_t>& dst, bool horizontal) {
        const int lines = horizontal ? h : w;
        const int length = horizontal ? w : h;
        for (int line = 0; line < lines; ++line) {
            const auto at = [&](int i) -> std::size_t {
                const int c = std::clamp(i, 0, length - 1);
                return horizontal ? (static_cast<std::size_t>(line) * w + c) * 4 : (static_cast<std::size_t>(c) * w + line) * 4;
            };
            int sum[4] = {};
            for (int i = -radius; i <= radius; ++i) {
                for (int k = 0; k < 4; ++k) {
                    sum[k] += src[at(i) + k];
                }
            }
            const int n = 2 * radius + 1;
            for (int i = 0; i < length; ++i) {
                for (int k = 0; k < 4; ++k) {
                    dst[at(i) + k] = static_cast<std::uint8_t>(sum[k] / n);
                    sum[k] += src[at(i + radius + 1) + k] - src[at(i - radius) + k];
                }
            }
        }
    };
    for (int round = 0; round < 3; ++round) {
        pass(rgba, tmp, true);
        pass(tmp, rgba, false);
    }
}

/// Each `cell` x `cell` block of RGBA8 replaced by its mean.
void pixelate(std::vector<std::uint8_t>& rgba, int w, int h, int cell) {
    cell = std::max(1, cell);
    for (int y0 = 0; y0 < h; y0 += cell) {
        for (int x0 = 0; x0 < w; x0 += cell) {
            const int x1 = std::min(w, x0 + cell), y1 = std::min(h, y0 + cell);
            long sum[4] = {};
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    for (int k = 0; k < 4; ++k) {
                        sum[k] += rgba[(static_cast<std::size_t>(y) * w + x) * 4 + k];
                    }
                }
            }
            const long n = static_cast<long>(x1 - x0) * (y1 - y0);
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    for (int k = 0; k < 4; ++k) {
                        rgba[(static_cast<std::size_t>(y) * w + x) * 4 + k] = static_cast<std::uint8_t>(sum[k] / n);
                    }
                }
            }
        }
    }
}

std::uint8_t byteOf(double v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
}

} // namespace

void publish(ProjectSnapshot project) {
    slot().store(std::move(project));
}

ProjectSnapshot published() {
    return slot().load();
}

std::uint64_t keyOf(const ProjectState& project, MediaId media) {
    std::uint64_t key = 0;
    for (const Redaction* covering : redactionsCovering(project, media)) { // a subclip's master's too (review wave 5)
        const Redaction& r = *covering;
        if (!r.enabled) {
            continue;
        }
        std::uint64_t h = mix(r.id.value, static_cast<std::uint64_t>(r.style));
        h = mix(h, static_cast<std::uint64_t>(r.source.start.value));
        h = mix(h, static_cast<std::uint64_t>(r.source.duration.value));
        h = mix(h, hashDouble(r.strength));
        h = mix(h, hashDouble(r.colorR) ^ (hashDouble(r.colorG) << 1) ^ (hashDouble(r.colorB) << 2));
        h = mix(h, std::hash<std::string>{}(std::string(toString(r.region.shape))));
        if (r.region.matte) {
            // AI-14: a matte region's pictures change with its matte track and with the track's CONTENT: a refine or a
            // second propagation keeps the track's identity, so the source's revision of it is part of the key (else an
            // old, possibly under-redacted thumbnail would stay; AI-14 review).
            const MaskMatte& matte = *r.region.matte;
            h = mix(h, std::hash<std::string>{}(matte.track));
            h = mix(h, std::hash<std::string>{}(matte.model));
            h = mix(h, static_cast<std::uint64_t>(matte.stream));
            h = mix(h, static_cast<std::uint64_t>(matte.source.start.value));
            h = mix(h, static_cast<std::uint64_t>(matte.source.duration.value));
            if (const std::shared_ptr<render::MatteSource> source = render::matteSource()) {
                h = mix(h, source->revision(matte));
            }
        } else if (r.region.shape == MaskShape::Matte) {
            h = mix(h, 0x6d61747465ull); // a matte shape without a matte: the grown box
        }
        for (const MaskParam& param : r.region.params) {
            h = mix(h, std::hash<std::string>{}(param.key));
            if (const Curve* curve = param.value.curve()) {
                for (const Keyframe& k : curve->keys) {
                    h = mix(mix(h, static_cast<std::uint64_t>(k.time.value)), hashDouble(k.value));
                }
            } else {
                h = mix(h, hashDouble(param.value.constant()));
            }
        }
        key = mix(key, h) | 1u;
    }
    return key;
}

bool apply(QImage& image, const ProjectState& project, MediaId media, Ticks time) {
    const MediaItem* item = project.findMedia(media);
    const VideoStreamInfo* stream = item ? streamOf(*item) : nullptr;
    if (!stream || image.isNull() || stream->width <= 0 || stream->height <= 0) {
        return false;
    }
    std::vector<const Redaction*> active;
    for (const Redaction* r : redactionsCovering(project, media)) { // a subclip's master's too (review wave 5)
        if (covers(*r, time, stream->frameRate)) {
            active.push_back(r);
        }
    }
    if (active.empty()) {
        return false;
    }
    if (image.format() != QImage::Format_RGBA8888) {
        image = image.convertToFormat(QImage::Format_RGBA8888);
    }
    const int w = image.width(), h = image.height();
    const int quarter = ((stream->rotationDegrees / 90) % 4 + 4) % 4;
    // The stored frame's size in this picture's pixels (display rotation undone).
    const int sw = quarter % 2 == 1 ? h : w;
    const int sh = quarter % 2 == 1 ? w : h;
    const double par = stream->sampleAspect.isValid() ? stream->sampleAspect.toDouble() : 1.0;
    const double aspect = static_cast<double>(stream->width) * par / stream->height;
    // Display pixel (x, y) -> stored pixel (u, v): the inverse of rotating the stored frame clockwise by quarter turns.
    const auto stored = [&](int x, int y) -> std::pair<int, int> {
        switch (quarter) {
        case 1: return {y, sh - 1 - x};
        case 2: return {sw - 1 - x, sh - 1 - y};
        case 3: return {sw - 1 - y, x};
        default: return {x, y};
        }
    };
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        std::memcpy(pixels.data() + static_cast<std::size_t>(y) * w * 4, image.constScanLine(y), static_cast<std::size_t>(w) * 4);
    }
    bool changed = false;
    for (const Redaction* r : active) {
        const auto spec = playback::redactionMaskAt(*r, time, aspect);
        if (!spec) {
            continue;
        }
        const std::vector<float> coverage = render::maskCoverageCpu(*spec, sw, sh);
        std::vector<std::uint8_t> effect = pixels;
        const double strengthPx = r->strength * sh; // frame heights -> pixels of this picture
        switch (r->style) {
        case RedactionStyle::Blur:
            boxBlur(effect, w, h, std::max(1, static_cast<int>(std::lround(strengthPx / 2.0))));
            break;
        case RedactionStyle::Mosaic:
            pixelate(effect, w, h, std::max(2, static_cast<int>(std::lround(strengthPx))));
            break;
        case RedactionStyle::Solid:
            for (std::size_t i = 0; i < effect.size(); i += 4) {
                effect[i] = byteOf(r->colorR);
                effect[i + 1] = byteOf(r->colorG);
                effect[i + 2] = byteOf(r->colorB);
                effect[i + 3] = 255;
            }
            break;
        }
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const auto [u, v] = stored(x, y);
                const float c = coverage[static_cast<std::size_t>(v) * sw + u];
                if (c <= 0.0f) {
                    continue;
                }
                const std::size_t o = (static_cast<std::size_t>(y) * w + x) * 4;
                for (int k = 0; k < 4; ++k) {
                    pixels[o + k] = static_cast<std::uint8_t>(std::lround(pixels[o + k] + (effect[o + k] - pixels[o + k]) * c));
                }
                changed = true;
            }
        }
    }
    for (int y = 0; y < h; ++y) {
        std::memcpy(image.scanLine(y), pixels.data() + static_cast<std::size_t>(y) * w * 4, static_cast<std::size_t>(w) * 4);
    }
    return changed;
}

namespace {

/// Media items of `project` whose file or proxy is `path`.
template <class F>
void forEachMediaOf(const ProjectState& project, const std::string& path, F&& f) {
    for (const MediaPtr& item : project.media) {
        if (item && (item->locator.path == path || (item->locator.proxy && item->locator.proxy->path == path))) {
            f(*item);
        }
    }
}

} // namespace

bool applyToFile(QImage& image, const std::string& utf8Path, Ticks time) {
    const ProjectSnapshot project = published();
    if (!project || project->redactions.empty()) {
        return false;
    }
    bool changed = false;
    // Items of one file sharing a redaction root (a master and its subclips) share their redactions: applied once.
    std::vector<MediaId> roots;
    forEachMediaOf(*project, utf8Path, [&](const MediaItem& item) {
        const MediaId root = redactionRoot(*project, item.id);
        if (std::ranges::find(roots, root) == roots.end()) {
            roots.push_back(root);
            changed = apply(image, *project, item.id, time) || changed;
        }
    });
    return changed;
}

std::uint64_t keyOfFile(const std::string& utf8Path) {
    const ProjectSnapshot project = published();
    std::uint64_t key = 0;
    if (project && !project->redactions.empty()) {
        forEachMediaOf(*project, utf8Path, [&](const MediaItem& item) {
            if (const std::uint64_t k = keyOf(*project, item.id)) {
                key = mix(key, k) | 1u;
            }
        });
    }
    return key;
}

} // namespace wabcut::app::redacted
