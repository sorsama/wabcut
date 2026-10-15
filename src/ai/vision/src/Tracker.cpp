#include <wabcut/ai/vision/Tracker.h>

#include <algorithm>
#include <tuple>

namespace wabcut::ai::vision {

namespace {

constexpr float kAlpha = 0.6f; // how far an estimate moves to its measurement
constexpr float kBeta = 0.25f; // how much the velocity learns from the residual

Box predicted(const Track& t) {
    return Box{t.box.x + t.velocity.x, t.box.y + t.velocity.y, std::max(1e-6f, t.box.w + t.velocity.w),
               std::max(1e-6f, t.box.h + t.velocity.h)};
}

void correct(Track& t, const Detection& d) {
    const Box p = predicted(t);
    const Box residual{d.box.x - p.x, d.box.y - p.y, d.box.w - p.w, d.box.h - p.h};
    t.box = Box{p.x + kAlpha * residual.x, p.y + kAlpha * residual.y, p.w + kAlpha * residual.w, p.h + kAlpha * residual.h};
    t.velocity = Box{t.velocity.x + kBeta * residual.x, t.velocity.y + kBeta * residual.y, t.velocity.w + kBeta * residual.w,
                     t.velocity.h + kBeta * residual.h};
    t.score = d.score;
    ++t.hits;
    t.lost = 0;
}

/// Greedy IoU matching of `dets` (indices into detections) to the tracks not taken yet.
void match(std::vector<Track>& tracks, std::vector<bool>& trackTaken, std::span<const Detection> detections,
           const std::vector<std::size_t>& dets, float threshold, std::vector<int>& ids, std::vector<bool>& detTaken) {
    std::vector<std::tuple<float, std::size_t, std::size_t>> pairs; // (iou, track, detection)
    for (std::size_t t = 0; t < tracks.size(); ++t) {
        if (trackTaken[t]) {
            continue;
        }
        const Box p = predicted(tracks[t]);
        for (const std::size_t d : dets) {
            const float overlap = iou(p, detections[d].box);
            if (overlap >= threshold) {
                pairs.emplace_back(overlap, t, d);
            }
        }
    }
    std::ranges::stable_sort(pairs, [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
    for (const auto& [overlap, t, d] : pairs) {
        if (trackTaken[t] || detTaken[d]) {
            continue;
        }
        trackTaken[t] = true;
        detTaken[d] = true;
        correct(tracks[t], detections[d]);
        ids[d] = tracks[t].id;
    }
}

} // namespace

ByteTracker::ByteTracker(TrackerOptions options) : m_options(options) {}

void ByteTracker::reset() { m_tracks.clear(); }

std::vector<int> ByteTracker::update(std::span<const Detection> detections) {
    std::vector<int> ids(detections.size(), -1);
    std::vector<bool> detTaken(detections.size(), false);
    std::vector<bool> trackTaken(m_tracks.size(), false);
    std::vector<std::size_t> high;
    std::vector<std::size_t> low;
    for (std::size_t d = 0; d < detections.size(); ++d) {
        if (detections[d].score >= m_options.highScore) {
            high.push_back(d);
        } else if (detections[d].score >= m_options.lowScore) {
            low.push_back(d);
        }
    }
    match(m_tracks, trackTaken, detections, high, m_options.matchIou, ids, detTaken);
    match(m_tracks, trackTaken, detections, low, m_options.lowMatchIou, ids, detTaken);

    // Tracks left unmatched coast on their velocity; tentative ones end at once, confirmed ones after maxLost.
    std::vector<Track> next;
    for (std::size_t t = 0; t < m_tracks.size(); ++t) {
        Track track = m_tracks[t];
        if (!trackTaken[t]) {
            track.box = predicted(track);
            ++track.lost;
            if (!track.confirmed || track.lost > m_options.maxLost) {
                continue;
            }
        } else if (track.hits >= m_options.minHits) {
            track.confirmed = true;
        }
        next.push_back(track);
    }
    for (const std::size_t d : high) {
        if (detTaken[d]) {
            continue;
        }
        Track track;
        track.id = m_nextId++;
        track.box = detections[d].box;
        track.score = detections[d].score;
        track.hits = 1;
        track.confirmed = m_options.minHits <= 1;
        ids[d] = track.id;
        next.push_back(track);
    }
    m_tracks = std::move(next);
    return ids;
}

} // namespace wabcut::ai::vision
