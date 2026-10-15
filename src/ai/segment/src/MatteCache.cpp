#include <wabcut/ai/segment/MatteCache.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>
#include <optional>

namespace wabcut::ai::segment {

namespace {

/// The content revision of a track: its identity and every frame (time, presence, box, matte bytes). Stable across
/// reloads; changes when a refine or a second propagation merges new mattes under the same identity. Never 0.
std::uint64_t contentRevision(const MatteTrack& t) {
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h = (h ^ ((v >> (i * 8)) & 0xFF)) * 1099511628211ull;
        }
    };
    mix(std::hash<std::string>{}(t.identity()));
    mix(static_cast<std::uint64_t>(t.width));
    mix(static_cast<std::uint64_t>(t.height));
    for (const MatteFrame& f : t.frames) {
        mix(static_cast<std::uint64_t>(f.time.value));
        mix(f.present ? 1u : 0u);
        if (f.box) {
            mix(std::hash<double>{}(f.box->x) ^ (std::hash<double>{}(f.box->y) << 1) ^ (std::hash<double>{}(f.box->width) << 2) ^
                (std::hash<double>{}(f.box->height) << 3));
        }
        mix(f.rle.size());
        for (std::uint8_t b : f.rle) {
            h = (h ^ b) * 1099511628211ull;
        }
    }
    return h | 1u;
}

/// A field at most kMaxFieldSide texels on its longer side: a signed-distance field is smooth, so a smaller one samples
/// the same edges (distances are in frame heights, independent of the texel count) and costs a fraction to upload.
constexpr int kMaxFieldSide = 256;

std::vector<float> downsampled(const std::vector<float>& distance, int width, int height, int& outWidth, int& outHeight) {
    const double scale = static_cast<double>(kMaxFieldSide) / std::max(width, height);
    outWidth = std::max(1, static_cast<int>(std::lround(width * scale)));
    outHeight = std::max(1, static_cast<int>(std::lround(height * scale)));
    std::vector<float> out(static_cast<std::size_t>(outWidth) * static_cast<std::size_t>(outHeight));
    for (int y = 0; y < outHeight; ++y) {
        // Bilinear at the destination texel's centre (texel centres at (i + 0.5) / size on both grids).
        const double fy = std::clamp((y + 0.5) / outHeight * height - 0.5, 0.0, static_cast<double>(height - 1));
        const int y0 = static_cast<int>(fy);
        const int y1 = std::min(y0 + 1, height - 1);
        const double ty = fy - y0;
        for (int x = 0; x < outWidth; ++x) {
            const double fx = std::clamp((x + 0.5) / outWidth * width - 0.5, 0.0, static_cast<double>(width - 1));
            const int x0 = static_cast<int>(fx);
            const int x1 = std::min(x0 + 1, width - 1);
            const double tx = fx - x0;
            const auto at = [&](int xx, int yy) { return static_cast<double>(distance[static_cast<std::size_t>(yy) * width + xx]); };
            const double top = at(x0, y0) * (1.0 - tx) + at(x1, y0) * tx;
            const double bottom = at(x0, y1) * (1.0 - tx) + at(x1, y1) * tx;
            out[static_cast<std::size_t>(y) * outWidth + x] = static_cast<float>(top * (1.0 - ty) + bottom * ty);
        }
    }
    return out;
}

} // namespace

CacheMatteSource::CacheMatteSource(std::filesystem::path cacheRoot, std::size_t fields)
    : m_store(std::move(cacheRoot)), m_capacity(std::max<std::size_t>(fields, 4)) {}

std::shared_ptr<const CacheMatteSource::TrackEntry> CacheMatteSource::makeEntry(std::shared_ptr<const MatteTrack> track) {
    auto made = std::make_shared<TrackEntry>();
    if (track) {
        made->revision = contentRevision(*track);
        for (std::size_t i = 0; i < track->frames.size(); ++i) {
            if (track->frames[i].present) {
                made->present.push_back(i);
            }
        }
    }
    made->track = std::move(track);
    return made;
}

void CacheMatteSource::remember(std::shared_ptr<const MatteTrack> track) {
    if (!track) {
        return;
    }
    const std::string id = track->identity();
    auto known = makeEntry(std::move(track));
    std::lock_guard lock(m_mutex);
    m_tracks[id] = std::move(known);
    m_trackOrder.remove(id);
    m_trackOrder.push_front(id);
}

std::size_t CacheMatteSource::fieldsMade() const {
    std::lock_guard lock(m_mutex);
    return m_made;
}

std::shared_ptr<const CacheMatteSource::TrackEntry> CacheMatteSource::entry(const std::string& identity) {
    {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_tracks.find(identity); it != m_tracks.end()) {
            if (m_trackOrder.front() != identity) {
                m_trackOrder.remove(identity);
                m_trackOrder.push_front(identity);
            }
            return it->second;
        }
    }
    auto loaded = m_store.get(identity);
    std::shared_ptr<const MatteTrack> track = loaded && *loaded ? std::make_shared<const MatteTrack>(std::move(**loaded)) : nullptr;
    auto made = makeEntry(std::move(track)); // a miss is remembered too (no track), until remember() or a restart
    std::lock_guard lock(m_mutex);
    m_tracks[identity] = made;
    m_trackOrder.remove(identity);
    m_trackOrder.push_front(identity);
    while (m_trackOrder.size() > 8) {
        m_tracks.erase(m_trackOrder.back());
        m_trackOrder.pop_back();
    }
    return made;
}

std::uint64_t CacheMatteSource::revision(const MaskMatte& matte) {
    const auto e = entry(matte.track);
    return e && e->track ? e->revision : 0;
}

std::shared_ptr<const render::MatteField> CacheMatteSource::field(const TrackEntry& known, std::size_t index, double grow) {
    const MatteTrack& track = *known.track;
    // By the track's content revision and the frame's TIME: a refined or re-propagated track under the same identity
    // never serves an old field (indices move when frames are merged in).
    const std::string key = track.identity() + "@" + std::to_string(known.revision) + "#" + std::to_string(track.frames[index].time.value) +
                            "+" + std::to_string(std::lround(grow * 1e6));
    {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_fieldIndex.find(key); it != m_fieldIndex.end()) {
            m_fieldLru.splice(m_fieldLru.begin(), m_fieldLru, it->second); // O(1)
            return it->second->second;
        }
    }
    auto image = track.imageOf(track.frames[index]);
    if (!image) {
        return nullptr;
    }
    auto f = std::make_shared<render::MatteField>();
    std::vector<float> distance = signedDistance(*image, track.aspect);
    if (std::max(image->width, image->height) > kMaxFieldSide) {
        f->distance = downsampled(distance, image->width, image->height, f->width, f->height);
    } else {
        f->width = image->width;
        f->height = image->height;
        f->distance = std::move(distance);
    }
    if (grow > 0.0) {
        for (float& d : f->distance) {
            d += static_cast<float>(grow);
        }
    }
    f->half.reserve(f->distance.size());
    for (float d : f->distance) {
        f->half.push_back(render::toHalf(d));
    }
    f->identity = std::hash<std::string>{}(key) | 1u;
    f->fallback = grow > 0.0;
    std::lock_guard lock(m_mutex);
    if (const auto it = m_fieldIndex.find(key); it != m_fieldIndex.end()) {
        return it->second->second; // another thread made it meanwhile
    }
    ++m_made;
    m_fieldLru.emplace_front(key, f);
    m_fieldIndex[key] = m_fieldLru.begin();
    while (m_fieldLru.size() > m_capacity) {
        m_fieldIndex.erase(m_fieldLru.back().first);
        m_fieldLru.pop_back();
    }
    return f;
}

std::shared_ptr<const render::MatteField> CacheMatteSource::fieldAt(const MaskMatte& matte, Ticks mediaTime, double, render::MatteUse use) {
    const auto e = entry(matte.track);
    if (!e || !e->track || e->track->frames.empty()) {
        return nullptr; // the mask's bounding box stands in (grown for a redaction)
    }
    const MatteTrack& t = *e->track;
    const Ticks tolerance{t.frameDuration.value * 3 / 4};
    const auto byTime = [&t](std::size_t i) { return t.frames[i].time; };
    const auto it = std::lower_bound(t.frames.begin(), t.frames.end(), mediaTime, [](const MatteFrame& f, Ticks v) { return f.time < v; });
    std::size_t nearest = it == t.frames.end() ? t.frames.size() - 1 : static_cast<std::size_t>(it - t.frames.begin());
    if (it != t.frames.begin() && it != t.frames.end() && (mediaTime - (it - 1)->time) < (it->time - mediaTime)) {
        --nearest;
    }
    const Ticks gap{std::abs((t.frames[nearest].time - mediaTime).value)};
    if (use == render::MatteUse::Effect) {
        // The matte at the frame (an absent object: an empty matte, so the effect covers nothing); outside the
        // propagated span the bounding box stands in.
        return gap <= tolerance ? field(*e, nearest, 0.0) : nullptr;
    }
    // A redaction: the matte at the frame when the model saw the object there; else the nearest present one, grown.
    if (gap <= tolerance && t.frames[nearest].present) {
        return field(*e, nearest, 0.0);
    }
    if (e->present.empty()) {
        return nullptr;
    }
    // The nearest present frame: a binary search over the present frames (O(log n), not a scan per call).
    const auto p = std::lower_bound(e->present.begin(), e->present.end(), mediaTime,
                                    [&byTime](std::size_t i, Ticks v) { return byTime(i) < v; });
    std::size_t best = p == e->present.end() ? e->present.back() : *p;
    if (p != e->present.begin()) {
        const std::size_t before = *(p - 1);
        if (p == e->present.end() || (mediaTime - byTime(before)) <= (byTime(*p) - mediaTime)) {
            best = before;
        }
    }
    const Ticks bestGap{std::abs((t.frames[best].time - mediaTime).value)};
    const double seconds = static_cast<double>(bestGap.value) / static_cast<double>(kTicksPerSecond);
    return field(*e, best, std::min(0.5, 0.01 + 0.15 * seconds));
}

} // namespace wabcut::ai::segment
