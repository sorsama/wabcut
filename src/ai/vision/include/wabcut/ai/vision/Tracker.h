#pragma once

// Multi-object tracking for auto reframe, after ByteTrack (Zhang et al. 2022; the method, MIT, reimplemented here):
// every sample, tracks predict their box (constant velocity, an alpha-beta filter), high-score detections are matched
// to them first by IoU, then low-score detections extend the tracks left over (so a face that dims or blurs keeps its
// id). Unmatched high-score detections start tentative tracks, confirmed after `minHits` matches; a track survives
// `maxLost` unmatched samples (a short occlusion) before it ends. Matching is greedy by IoU, deterministic. Tracks
// reset at shot cuts (the caller calls reset()).

#include <wabcut/ai/vision/Detection.h>

#include <span>
#include <vector>

namespace wabcut::ai::vision {

struct TrackerOptions {
    float highScore = 0.5f;   ///< detections at or above match first and may start tracks
    float lowScore = 0.1f;    ///< below this a detection is ignored; in between it only extends tracks
    float matchIou = 0.3f;    ///< IoU with a track's prediction needed in the first stage
    float lowMatchIou = 0.5f; ///< and in the second stage (low-score detections)
    int maxLost = 15;         ///< unmatched samples a track survives
    int minHits = 2;          ///< matched samples before a track is confirmed
};

struct Track {
    int id = 0;
    Box box;           ///< last estimate
    Box velocity;      ///< per sample (dx, dy, dw, dh)
    float score = 0.0f;
    int hits = 0;
    int lost = 0;      ///< samples since the last match
    bool confirmed = false;
};

class ByteTracker {
public:
    explicit ByteTracker(TrackerOptions options = {});

    /// One sample: the id each detection was given (-1 when it matched nothing and could not start a track).
    std::vector<int> update(std::span<const Detection> detections);

    /// Live tracks (matched now, or lost for at most maxLost samples).
    const std::vector<Track>& tracks() const { return m_tracks; }
    /// Forgets every track (a shot cut); ids keep counting up, so they stay unique per analysis.
    void reset();

private:
    TrackerOptions m_options;
    std::vector<Track> m_tracks;
    int m_nextId = 1;
};

} // namespace wabcut::ai::vision
