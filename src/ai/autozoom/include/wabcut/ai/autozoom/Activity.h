#pragma once

// Where things happen in a screen recording (AI-11 auto zoom, docs/ai.md "Creator superpowers"): the cursor, clicks,
// typing and scene changes over time, as fractions of the displayed picture.
//
// Three sources, best first:
//   1. a cursor sidecar written by the recorder: Wabcut's JSON format (below; documented in docs/ai.md);
//   2. a cursor log in CSV ("t,x,y[,event]" per line, seconds and pixels, optional header), the layout cursor-logging
//      scripts for OBS and similar recorders write (OBS itself records no cursor data);
//   3. local detection on the frames (ActivityDetector): frames at reduced resolution (640 px wide by default),
//      differenced block by block (8 x 8). Small changed blobs are cursor candidates; the cursor is where a blob
//      APPEARS (more local contrast now than before: an arrow or I-beam over a flat UI), so the old position left
//      behind is never mistaken for it. Larger changed regions near a
//      cursor that just stopped are clicks (a button lit, a menu opened); repeated small changes in one place while
//      the cursor rests are typing; a change of most of the picture is a scene change (another window or app).
//      Regions that change all the time (a playing video inside the recording) are ignored.
//
// Wabcut cursor sidecar (UTF-8 JSON, "<recording>.cursor.json" next to the file):
//   {"format": "wabcut.cursor", "version": 1, "width": 1920, "height": 1080, "offset": 0.0,
//    "events": [{"t": 1.25, "x": 640, "y": 360, "type": "move"},
//               {"t": 2.0,  "x": 652, "y": 371, "type": "click"},
//               {"t": 3.1,  "x": 900, "y": 410, "type": "key"}]}
//   t: seconds of the recording (plus "offset"); x, y: pixels of a width x height picture (or 0..1 when width/height
//   are absent); type: move, click, down, up, key, scroll. Unknown types are ignored. At most 2,000,000 events.

#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::autozoom {

enum class ActivityKind : std::uint8_t {
    Click,       ///< a click (sidecar) or a UI change right where the cursor stopped (detected)
    Key,         ///< typing
    Scroll,
    Dwell,       ///< the cursor came to rest after moving (weak interest)
    Change,      ///< the UI changed with no cursor to explain it (detected only)
    SceneChange, ///< most of the picture changed (another window, an app switch): zoom out
};
std::string_view toString(ActivityKind kind);

struct ActivityEvent {
    Ticks time;               ///< media (source) time
    ActivityKind kind = ActivityKind::Click;
    double x = 0.5;           ///< 0..1 of the displayed picture
    double y = 0.5;
    double width = 0.0;       ///< the region that changed, 0..1 (0 = a point)
    double height = 0.0;
    bool operator==(const ActivityEvent&) const = default;
};

struct CursorSample {
    Ticks time;
    double x = 0.5;
    double y = 0.5;
    bool operator==(const CursorSample&) const = default;
};

struct ActivityTrack {
    std::vector<ActivityEvent> events; ///< sorted by time
    std::vector<CursorSample> cursor;  ///< sorted by time
    std::string origin;                ///< "sidecar", "cursor-log" or "detected"
};

/// Parses a Wabcut cursor sidecar. `pictureWidth` / `pictureHeight` normalise pixel coordinates when the file names
/// no size. Errors: ParseError (not the format, bad values), InvalidArgument (too many events).
std::expected<ActivityTrack, Error> parseCursorSidecar(std::string_view json, int pictureWidth = 0, int pictureHeight = 0);
/// Parses a CSV cursor log ("t,x,y[,event]"; commas, semicolons or tabs; a header line is skipped; event is
/// move / click / down / up / key / scroll or empty). Errors: ParseError, InvalidArgument (no size to normalise with).
std::expected<ActivityTrack, Error> parseCursorCsv(std::string_view text, int pictureWidth, int pictureHeight);
/// Either format, by content.
std::expected<ActivityTrack, Error> parseCursorFile(std::string_view content, int pictureWidth, int pictureHeight);

/// A grey picture at analysis resolution.
struct GreyFrame {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> luma;
    Ticks time;
    bool valid() const { return width > 0 && height > 0 && luma.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height); }
};

/// Packed 8-bit RGB (`rgb` = width * height * 3) to a grey frame `width` wide (area average; height keeps the aspect).
GreyFrame toAnalysisFrame(const std::uint8_t* rgb, int width, int height, int analysisWidth, Ticks time);

struct DetectOptions {
    double fps = 10.0;           ///< samples per second of the recording
    int width = 640;             ///< analysis width
    int pixelThreshold = 24;     ///< luma change (0..255) that counts as changed
    double sceneChange = 0.4;    ///< changed fraction of the picture that is a scene change
    double cursorMaxSize = 0.06; ///< a cursor blob is at most this fraction of the picture width (and height x 16/9)
    double clickDelay = 0.7;     ///< seconds after the cursor stops within which a UI change near it is a click
    double clickRadius = 0.15;   ///< how near (fraction of the width) a UI change must be to the cursor
    double busyFraction = 0.6;   ///< a block that changed in this fraction of the last 3 s is background motion
};

/// Streaming detection: push frames in time order, then take the activity.
class ActivityDetector {
public:
    explicit ActivityDetector(DetectOptions options = {});
    /// Adds the next frame (same size as the first). Frames of another size are ignored.
    void push(const GreyFrame& frame);
    ActivityTrack finish();
    std::size_t frames() const { return m_frames; }

private:
    struct Blob {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0; ///< block bounds, inclusive
        int changed = 0;                    ///< changed pixels
        double cx = 0.0, cy = 0.0;          ///< centroid of changed pixels (pixels)
    };
    std::vector<Blob> blobs(const GreyFrame& previous, const GreyFrame& current);
    double appearance(const GreyFrame& previous, const GreyFrame& current, const Blob& blob) const;
    void report(ActivityEvent event);

    DetectOptions m_options;
    GreyFrame m_previous;
    std::size_t m_frames = 0;
    int m_blocksX = 0;
    int m_blocksY = 0;
    std::vector<std::uint16_t> m_busy;       ///< per block: frames of the recent window it changed in
    std::vector<std::vector<std::uint8_t>> m_history; ///< per frame of the recent window: changed blocks (1 per block)
    std::size_t m_historyAt = 0;
    /// "Never": far enough in the past that `t - kLongAgo` cannot overflow (Ticks::min() would).
    static constexpr Ticks kLongAgo{-kTicksPerSecond * 86400};
    std::optional<CursorSample> m_cursor;    ///< last known cursor position
    Ticks m_lastMove = kLongAgo;             ///< when the cursor last moved
    bool m_moving = false;
    std::optional<ActivityEvent> m_typing;   ///< current typing burst
    int m_typingFrames = 0;
    Ticks m_lastTypingChange = kLongAgo;
    Ticks m_lastKeyEvent = kLongAgo;
    Ticks m_lastClick = kLongAgo;
    bool m_dwelled = true;
    ActivityTrack m_track;
};

struct DetectStats {
    std::size_t frames = 0;
    double seconds = 0.0;      ///< wall time of reading + analysis
    double analysisSeconds = 0.0; ///< analysis alone
    double framesPerSecond = 0.0;
};

/// Reads the frame on screen at a source time, `width` pixels wide (grey). Errors stop the detection.
using FrameReader = std::function<std::expected<GreyFrame, Error>(Ticks sourceTime, int width)>;

/// Detects activity over [source.start, source.end()) at options.fps. `progress` (0..1) returning false cancels
/// (Cancelled). Errors: the reader's.
std::expected<ActivityTrack, Error> detectActivity(const FrameReader& read, TimeRange source, const DetectOptions& options = {},
                                                   const std::function<bool(double)>& progress = {}, DetectStats* stats = nullptr);

} // namespace wabcut::ai::autozoom
