#include <wabcut/ai/autozoom/Activity.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ranges>
#include <string>

namespace wabcut::ai::autozoom {

using Json = nlohmann::json;

namespace {

constexpr int kBlock = 8;
constexpr std::size_t kMaxEvents = 2'000'000;

Ticks fromSeconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

/// Adds Dwell events where the cursor came to rest for at least `rest` after moving, and sorts the events.
void finishTrack(ActivityTrack& track, Ticks rest = Ticks{kTicksPerSecond / 2}) {
    std::ranges::stable_sort(track.cursor, [](const CursorSample& a, const CursorSample& b) { return a.time < b.time; });
    bool moved = false;
    for (std::size_t i = 1; i < track.cursor.size(); ++i) {
        const CursorSample& a = track.cursor[i - 1];
        const CursorSample& b = track.cursor[i];
        const double distance = std::hypot(b.x - a.x, b.y - a.y);
        if (distance > 0.004) {
            moved = true;
            continue;
        }
        // Still since a: is the rest long enough (until the next move or the end)?
        if (moved) {
            std::size_t k = i;
            while (k + 1 < track.cursor.size() && std::hypot(track.cursor[k + 1].x - a.x, track.cursor[k + 1].y - a.y) <= 0.004) {
                ++k;
            }
            if (track.cursor[k].time - a.time >= rest || k + 1 == track.cursor.size()) {
                track.events.push_back(ActivityEvent{a.time + rest, ActivityKind::Dwell, a.x, a.y, 0.0, 0.0});
            }
            moved = false;
        }
    }
    std::ranges::stable_sort(track.events, [](const ActivityEvent& a, const ActivityEvent& b) { return a.time < b.time; });
}

/// The cursor's position at `t` (the last sample at or before it), if any.
std::optional<CursorSample> cursorAt(const std::vector<CursorSample>& cursor, Ticks t) {
    const auto it = std::ranges::upper_bound(cursor, t, {}, &CursorSample::time);
    if (it == cursor.begin()) {
        return std::nullopt;
    }
    return *std::prev(it);
}

void addTyped(ActivityTrack& track, const std::string& type, Ticks t, double x, double y, bool hasPoint) {
    if (type == "move" || type.empty()) {
        track.cursor.push_back(CursorSample{t, x, y});
        return;
    }
    ActivityKind kind;
    if (type == "click" || type == "down" || type == "mousedown" || type == "leftclick" || type == "rightclick") {
        kind = ActivityKind::Click;
    } else if (type == "key" || type == "keydown" || type == "type") {
        kind = ActivityKind::Key;
    } else if (type == "scroll" || type == "wheel") {
        kind = ActivityKind::Scroll;
    } else {
        return; // "up" and unknown types
    }
    if (hasPoint) {
        track.cursor.push_back(CursorSample{t, x, y});
    } else if (const auto at = cursorAt(track.cursor, t)) {
        x = at->x;
        y = at->y;
    }
    track.events.push_back(ActivityEvent{t, kind, std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0), 0.0, 0.0});
}

} // namespace

std::string_view toString(ActivityKind kind) {
    switch (kind) {
    case ActivityKind::Click:
        return "click";
    case ActivityKind::Key:
        return "key";
    case ActivityKind::Scroll:
        return "scroll";
    case ActivityKind::Dwell:
        return "dwell";
    case ActivityKind::Change:
        return "change";
    case ActivityKind::SceneChange:
        return "sceneChange";
    }
    return "click";
}

std::expected<ActivityTrack, Error> parseCursorSidecar(std::string_view text, int pictureWidth, int pictureHeight) {
    const Json json = Json::parse(text, nullptr, false);
    const auto bad = [](std::string what) { return std::unexpected(Error(ErrorCode::ParseError, "cursor sidecar: " + what)); };
    if (!json.is_object() || !json.contains("events") || !json["events"].is_array()) {
        return bad("expected an object with an \"events\" array");
    }
    if (json.contains("format") && (!json["format"].is_string() || json["format"].get<std::string>() != "wabcut.cursor")) {
        return bad("\"format\" is not \"wabcut.cursor\"");
    }
    if (json.contains("version") && (!json["version"].is_number_integer() || json["version"].get<int>() != 1)) {
        return bad("only version 1 is known");
    }
    if (json["events"].size() > kMaxEvents) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "cursor sidecar: more than 2,000,000 events"));
    }
    double width = pictureWidth > 0 ? pictureWidth : 0.0;
    double height = pictureHeight > 0 ? pictureHeight : 0.0;
    if (json.contains("width") && json["width"].is_number() && json["width"].get<double>() > 0.0) {
        width = json["width"].get<double>();
    }
    if (json.contains("height") && json["height"].is_number() && json["height"].get<double>() > 0.0) {
        height = json["height"].get<double>();
    }
    const double offset = json.contains("offset") && json["offset"].is_number() ? json["offset"].get<double>() : 0.0;
    if (!std::isfinite(offset) || std::abs(offset) > 86400.0) {
        return bad("\"offset\" is out of range");
    }
    ActivityTrack track;
    track.origin = "sidecar";
    for (const Json& e : json["events"]) {
        if (!e.is_object() || !e.contains("t") || !e["t"].is_number()) {
            return bad("every event needs a time \"t\" in seconds");
        }
        const double t = e["t"].get<double>() + offset;
        if (!std::isfinite(t) || t < 0.0 || t > 7.0 * 86400.0) {
            return bad("an event time is out of range");
        }
        const bool hasPoint = e.contains("x") && e["x"].is_number() && e.contains("y") && e["y"].is_number();
        double x = hasPoint ? e["x"].get<double>() : 0.5;
        double y = hasPoint ? e["y"].get<double>() : 0.5;
        if (hasPoint) {
            if (!std::isfinite(x) || !std::isfinite(y)) {
                return bad("an event position is not a number");
            }
            if (width > 0.0 && height > 0.0) {
                x /= width;
                y /= height;
            } else if (x > 1.0 || y > 1.0) {
                return std::unexpected(Error(ErrorCode::InvalidArgument, "cursor sidecar: pixel positions but no \"width\" / \"height\""));
            }
        }
        const std::string type = e.contains("type") && e["type"].is_string() ? lower(e["type"].get<std::string>()) : std::string("move");
        addTyped(track, type, fromSeconds(t), std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0), hasPoint);
    }
    finishTrack(track);
    return track;
}

std::expected<ActivityTrack, Error> parseCursorCsv(std::string_view text, int pictureWidth, int pictureHeight) {
    ActivityTrack track;
    track.origin = "cursor-log";
    std::size_t start = 0;
    std::size_t lineNumber = 0;
    std::size_t rows = 0;
    bool normalised = true;
    struct Row {
        double t, x, y;
        std::string type;
    };
    std::vector<Row> parsed;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string line(text.substr(start, end - start));
        start = end + 1;
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::vector<std::string> fields;
        std::string field;
        for (const char ch : line) {
            if (ch == ',' || ch == ';' || ch == '\t') {
                fields.push_back(field);
                field.clear();
            } else if (ch != ' ' && ch != '"') {
                field += ch;
            }
        }
        fields.push_back(field);
        char* stop = nullptr;
        const double t = fields.empty() ? 0.0 : std::strtod(fields[0].c_str(), &stop);
        if (fields.size() < 3 || fields[0].empty() || stop == fields[0].c_str()) {
            if (rows == 0 && lineNumber == 1 && !fields[0].empty() && stop == fields[0].c_str()) {
                continue; // a header ("time,x,y,event")
            }
            return std::unexpected(Error(ErrorCode::ParseError, "cursor log line " + std::to_string(lineNumber) + ": expected t,x,y"));
        }
        char* xEnd = nullptr;
        char* yEnd = nullptr;
        const double x = std::strtod(fields[1].c_str(), &xEnd);
        const double y = std::strtod(fields[2].c_str(), &yEnd);
        if (xEnd == fields[1].c_str() || yEnd == fields[2].c_str() || !std::isfinite(t) || !std::isfinite(x) || !std::isfinite(y) || t < 0.0) {
            return std::unexpected(Error(ErrorCode::ParseError, "cursor log line " + std::to_string(lineNumber) + ": bad numbers"));
        }
        normalised = normalised && x <= 1.0 && y <= 1.0;
        parsed.push_back(Row{t, x, y, fields.size() > 3 ? lower(fields[3]) : std::string()});
        if (++rows > kMaxEvents) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, "cursor log: more than 2,000,000 rows"));
        }
    }
    if (parsed.empty()) {
        return std::unexpected(Error(ErrorCode::ParseError, "cursor log: no rows of t,x,y"));
    }
    if (!normalised && (pictureWidth <= 0 || pictureHeight <= 0)) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "cursor log: pixel positions but the picture size is unknown"));
    }
    for (const Row& r : parsed) {
        const double x = normalised ? r.x : r.x / pictureWidth;
        const double y = normalised ? r.y : r.y / pictureHeight;
        addTyped(track, r.type, fromSeconds(r.t), std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0), true);
    }
    finishTrack(track);
    return track;
}

std::expected<ActivityTrack, Error> parseCursorFile(std::string_view content, int pictureWidth, int pictureHeight) {
    std::size_t i = 0;
    while (i < content.size() && (content[i] == ' ' || content[i] == '\n' || content[i] == '\r' || content[i] == '\t')) {
        ++i;
    }
    if (content.substr(i).starts_with("\xEF\xBB\xBF")) {
        i += 3;
    }
    if (i < content.size() && content[i] == '{') {
        return parseCursorSidecar(content.substr(i), pictureWidth, pictureHeight);
    }
    return parseCursorCsv(content.substr(i), pictureWidth, pictureHeight);
}

GreyFrame toAnalysisFrame(const std::uint8_t* rgb, int width, int height, int analysisWidth, Ticks time) {
    GreyFrame out;
    out.time = time;
    if (!rgb || width <= 0 || height <= 0 || analysisWidth <= 0) {
        return out;
    }
    const int ow = std::min(analysisWidth, width);
    const int oh = std::max(1, static_cast<int>(std::lround(static_cast<double>(height) * ow / width)));
    out.width = ow;
    out.height = oh;
    out.luma.resize(static_cast<std::size_t>(ow) * static_cast<std::size_t>(oh));
    std::vector<int> xs(static_cast<std::size_t>(ow) + 1);
    for (int x = 0; x <= ow; ++x) {
        xs[static_cast<std::size_t>(x)] = static_cast<int>(static_cast<std::int64_t>(x) * width / ow);
    }
    for (int y = 0; y < oh; ++y) {
        const int y0 = static_cast<int>(static_cast<std::int64_t>(y) * height / oh);
        const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<std::int64_t>(y + 1) * height / oh));
        for (int x = 0; x < ow; ++x) {
            const int x0 = xs[static_cast<std::size_t>(x)];
            const int x1 = std::max(x0 + 1, xs[static_cast<std::size_t>(x) + 1]);
            std::uint32_t sum = 0;
            for (int sy = y0; sy < y1; ++sy) {
                const std::uint8_t* row = rgb + (static_cast<std::size_t>(sy) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x0)) * 3;
                for (int sx = x0; sx < x1; ++sx, row += 3) {
                    sum += 77u * row[0] + 150u * row[1] + 29u * row[2];
                }
            }
            const std::uint32_t count = static_cast<std::uint32_t>((x1 - x0) * (y1 - y0));
            out.luma[static_cast<std::size_t>(y) * static_cast<std::size_t>(ow) + static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>((sum / count) >> 8);
        }
    }
    return out;
}

// --- Detection -----------------------------------------------------------------------------------------------------

ActivityDetector::ActivityDetector(DetectOptions options) : m_options(options) { m_track.origin = "detected"; }

void ActivityDetector::report(ActivityEvent event) {
    event.x = std::clamp(event.x, 0.0, 1.0);
    event.y = std::clamp(event.y, 0.0, 1.0);
    m_track.events.push_back(event);
}

std::vector<ActivityDetector::Blob> ActivityDetector::blobs(const GreyFrame& previous, const GreyFrame& current) {
    const int w = current.width;
    const int h = current.height;
    const std::size_t blockCount = static_cast<std::size_t>(m_blocksX) * static_cast<std::size_t>(m_blocksY);
    std::vector<int> count(blockCount, 0);
    std::vector<double> sumX(blockCount, 0.0);
    std::vector<double> sumY(blockCount, 0.0);
    const int threshold = m_options.pixelThreshold;
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* a = previous.luma.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        const std::uint8_t* b = current.luma.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        const std::size_t rowBlock = static_cast<std::size_t>(y / kBlock) * static_cast<std::size_t>(m_blocksX);
        for (int x = 0; x < w; ++x) {
            if (std::abs(static_cast<int>(a[x]) - static_cast<int>(b[x])) > threshold) {
                const std::size_t block = rowBlock + static_cast<std::size_t>(x / kBlock);
                ++count[block];
                sumX[block] += x;
                sumY[block] += y;
            }
        }
    }
    // Background motion: blocks that changed in most of the recent frames.
    const std::size_t window = std::max<std::size_t>(10, static_cast<std::size_t>(std::lround(3.0 * m_options.fps)));
    if (m_history.size() < window) {
        m_history.emplace_back(blockCount, 0);
        m_historyAt = m_history.size() - 1;
    } else {
        m_historyAt = (m_historyAt + 1) % window;
        for (std::size_t b = 0; b < blockCount; ++b) {
            m_busy[b] = static_cast<std::uint16_t>(m_busy[b] - m_history[m_historyAt][b]);
        }
    }
    std::vector<std::uint8_t>& now = m_history[m_historyAt];
    std::vector<std::uint8_t> changed(blockCount, 0);
    const auto busyLimit = static_cast<std::uint16_t>(std::ceil(m_options.busyFraction * static_cast<double>(window)));
    for (std::size_t b = 0; b < blockCount; ++b) {
        now[b] = count[b] >= 3 ? 1 : 0;
        m_busy[b] = static_cast<std::uint16_t>(m_busy[b] + now[b]);
        changed[b] = now[b] && !(m_history.size() >= window && m_busy[b] >= busyLimit) ? 1 : 0;
    }
    // 4-connected components of changed blocks.
    std::vector<Blob> out;
    std::vector<int> label(blockCount, -1);
    std::vector<std::size_t> stack;
    for (std::size_t start = 0; start < blockCount; ++start) {
        if (!changed[start] || label[start] >= 0) {
            continue;
        }
        Blob blob;
        blob.x0 = blob.x1 = static_cast<int>(start % static_cast<std::size_t>(m_blocksX));
        blob.y0 = blob.y1 = static_cast<int>(start / static_cast<std::size_t>(m_blocksX));
        double sx = 0.0;
        double sy = 0.0;
        stack.push_back(start);
        label[start] = static_cast<int>(out.size());
        while (!stack.empty()) {
            const std::size_t b = stack.back();
            stack.pop_back();
            const int bx = static_cast<int>(b % static_cast<std::size_t>(m_blocksX));
            const int by = static_cast<int>(b / static_cast<std::size_t>(m_blocksX));
            blob.x0 = std::min(blob.x0, bx);
            blob.x1 = std::max(blob.x1, bx);
            blob.y0 = std::min(blob.y0, by);
            blob.y1 = std::max(blob.y1, by);
            blob.changed += count[b];
            sx += sumX[b];
            sy += sumY[b];
            const auto visit = [&](int nx, int ny) {
                if (nx < 0 || ny < 0 || nx >= m_blocksX || ny >= m_blocksY) {
                    return;
                }
                const std::size_t n = static_cast<std::size_t>(ny) * static_cast<std::size_t>(m_blocksX) + static_cast<std::size_t>(nx);
                if (changed[n] && label[n] < 0) {
                    label[n] = static_cast<int>(out.size());
                    stack.push_back(n);
                }
            };
            visit(bx - 1, by);
            visit(bx + 1, by);
            visit(bx, by - 1);
            visit(bx, by + 1);
        }
        blob.cx = blob.changed > 0 ? sx / blob.changed : (blob.x0 + blob.x1 + 1) * kBlock / 2.0;
        blob.cy = blob.changed > 0 ? sy / blob.changed : (blob.y0 + blob.y1 + 1) * kBlock / 2.0;
        out.push_back(blob);
    }
    return out;
}

double ActivityDetector::appearance(const GreyFrame& previous, const GreyFrame& current, const Blob& blob) const {
    const int x0 = std::max(0, blob.x0 * kBlock - 2);
    const int y0 = std::max(0, blob.y0 * kBlock - 2);
    const int x1 = std::min(current.width, (blob.x1 + 1) * kBlock + 2);
    const int y1 = std::min(current.height, (blob.y1 + 1) * kBlock + 2);
    const auto deviation = [&](const GreyFrame& f) {
        double sum = 0.0;
        double sum2 = 0.0;
        const double n = static_cast<double>((x1 - x0) * (y1 - y0));
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const double v = f.luma[static_cast<std::size_t>(y) * static_cast<std::size_t>(f.width) + static_cast<std::size_t>(x)];
                sum += v;
                sum2 += v * v;
            }
        }
        const double mean = sum / n;
        return std::sqrt(std::max(0.0, sum2 / n - mean * mean));
    };
    return deviation(current) - deviation(previous);
}

void ActivityDetector::push(const GreyFrame& frame) {
    if (!frame.valid()) {
        return;
    }
    ++m_frames;
    if (!m_previous.valid()) {
        m_previous = frame;
        m_blocksX = (frame.width + kBlock - 1) / kBlock;
        m_blocksY = (frame.height + kBlock - 1) / kBlock;
        m_busy.assign(static_cast<std::size_t>(m_blocksX) * static_cast<std::size_t>(m_blocksY), 0);
        return;
    }
    if (frame.width != m_previous.width || frame.height != m_previous.height) {
        return;
    }
    const Ticks t = frame.time;
    const double w = frame.width;
    const double h = frame.height;
    const std::vector<Blob> found = blobs(m_previous, frame);
    int totalChanged = 0;
    for (const Blob& b : found) {
        totalChanged += b.changed;
    }
    if (static_cast<double>(totalChanged) >= m_options.sceneChange * w * h) {
        report(ActivityEvent{t, ActivityKind::SceneChange, 0.5, 0.5, 1.0, 1.0});
        m_typing.reset();
        m_typingFrames = 0;
        m_previous = frame;
        return;
    }
    const double cursorMax = m_options.cursorMaxSize * w;
    std::vector<const Blob*> cursorLike;
    std::vector<const Blob*> changes;
    for (const Blob& b : found) {
        const double bw = (b.x1 - b.x0 + 1) * kBlock;
        const double bh = (b.y1 - b.y0 + 1) * kBlock;
        if (b.changed < 4) {
            continue;
        }
        if (bw <= cursorMax && bh <= cursorMax * 16.0 / 9.0) {
            cursorLike.push_back(&b);
        } else {
            changes.push_back(&b);
        }
    }
    // The cursor moves as a pair of small changes: where it was and where it is now. A small change at the last known
    // position with another one within reach means it moved there (over text or a busy picture the new position need
    // not gain contrast); a small blob appearing on its own is the cursor only near where it was (or while none is
    // known yet, or to find it again after losing it, paired with one fading out). A typed character appears without
    // anything changing at the cursor.
    std::vector<double> appeared(cursorLike.size());
    for (std::size_t k = 0; k < cursorLike.size(); ++k) {
        appeared[k] = appearance(m_previous, frame, *cursorLike[k]);
    }
    const auto distance = [&](const Blob& a, double x, double y) { return std::hypot(a.cx / w - x, a.cy / h - y); };
    const double reach = 0.35; // of the width, per analysed frame
    std::optional<std::size_t> atOld;
    if (m_cursor) {
        for (std::size_t k = 0; k < cursorLike.size(); ++k) {
            const double d = distance(*cursorLike[k], m_cursor->x, m_cursor->y);
            if (d <= 0.03 && (!atOld || d < distance(*cursorLike[*atOld], m_cursor->x, m_cursor->y))) {
                atOld = k;
            }
        }
    }
    const Blob* cursor = nullptr;
    const auto pick = [&](auto&& eligible, double floor) {
        double best = floor;
        for (std::size_t k = 0; k < cursorLike.size(); ++k) {
            if (eligible(k) && appeared[k] > best) {
                best = appeared[k];
                cursor = cursorLike[k];
            }
        }
    };
    if (atOld) {
        pick([&](std::size_t k) { return k != *atOld && distance(*cursorLike[k], m_cursor->x, m_cursor->y) <= reach; }, -1.5);
    }
    if (!cursor && m_cursor) {
        pick([&](std::size_t k) { return distance(*cursorLike[k], m_cursor->x, m_cursor->y) <= 0.02; }, 1.5);
    }
    if (!cursor) {
        // No cursor yet, or lost: a blob appearing paired with one fading out within reach.
        pick([&](std::size_t k) {
            return std::ranges::any_of(std::views::iota(std::size_t{0}, cursorLike.size()), [&](std::size_t j) {
                return j != k && appeared[j] < -1.5 && distance(*cursorLike[k], cursorLike[j]->cx / w, cursorLike[j]->cy / h) <= reach;
            });
        }, 1.5);
    }
    bool movedNow = false;
    if (cursor) {
        const CursorSample sample{t, cursor->cx / w, cursor->cy / h};
        if (!m_cursor || std::hypot(sample.x - m_cursor->x, sample.y - m_cursor->y) > 0.004) {
            movedNow = true;
            m_lastMove = t;
            m_moving = true;
            m_dwelled = false;
        }
        m_cursor = sample;
        m_track.cursor.push_back(sample);
    }
    // Small changes that are not the cursor (typing) count with the others; the place the cursor left does not.
    for (std::size_t k = 0; k < cursorLike.size(); ++k) {
        const Blob* b = cursorLike[k];
        if (b == cursor || (movedNow && (atOld == k || appeared[k] < -1.5))) {
            continue;
        }
        changes.push_back(b);
    }
    const double restSeconds = m_cursor ? (t - m_lastMove).toSeconds() : 1e9;
    if (m_cursor && m_moving && !movedNow && restSeconds >= 0.4 && !m_dwelled) {
        report(ActivityEvent{t, ActivityKind::Dwell, m_cursor->x, m_cursor->y, 0.0, 0.0});
        m_dwelled = true;
        m_moving = false;
    }
    for (const Blob* b : changes) {
        const double rx0 = b->x0 * kBlock / w;
        const double ry0 = b->y0 * kBlock / h;
        const double rx1 = std::min(1.0, (b->x1 + 1) * kBlock / w);
        const double ry1 = std::min(1.0, (b->y1 + 1) * kBlock / h);
        const double cx = (rx0 + rx1) / 2.0;
        const double cy = (ry0 + ry1) / 2.0;
        if (m_cursor && !movedNow && restSeconds <= m_options.clickDelay) {
            // Distance from the cursor to the region (0 inside it).
            const double dx = std::max({rx0 - m_cursor->x, 0.0, m_cursor->x - rx1});
            const double dy = std::max({ry0 - m_cursor->y, 0.0, m_cursor->y - ry1});
            // One click per 0.6 s: the first character typed into a field just clicked is not another click.
            if (std::hypot(dx, dy) <= m_options.clickRadius && (t - m_lastClick).toSeconds() >= 0.6) {
                report(ActivityEvent{t, ActivityKind::Click, m_cursor->x, m_cursor->y, rx1 - rx0, ry1 - ry0});
                m_lastClick = t;
                continue;
            }
        }
        const bool small = rx1 - rx0 <= 0.15 && ry1 - ry0 <= 0.15;
        if (small) {
            // Typing: small changes again and again in one place.
            if (m_typing && std::hypot(cx - m_typing->x, cy - m_typing->y) < 0.1 && (t - m_lastTypingChange).toSeconds() < 1.0) {
                const double ux0 = std::min(m_typing->x - m_typing->width / 2, rx0);
                const double uy0 = std::min(m_typing->y - m_typing->height / 2, ry0);
                const double ux1 = std::max(m_typing->x + m_typing->width / 2, rx1);
                const double uy1 = std::max(m_typing->y + m_typing->height / 2, ry1);
                m_typing = ActivityEvent{t, ActivityKind::Key, (ux0 + ux1) / 2, (uy0 + uy1) / 2, ux1 - ux0, uy1 - uy0};
                ++m_typingFrames;
            } else {
                m_typing = ActivityEvent{t, ActivityKind::Key, cx, cy, rx1 - rx0, ry1 - ry0};
                m_typingFrames = 1;
            }
            m_lastTypingChange = t;
            if (m_typingFrames >= 3 && (t - m_lastKeyEvent).toSeconds() >= 1.0) {
                report(*m_typing);
                m_lastKeyEvent = t;
            }
            continue;
        }
        if (!m_cursor) {
            report(ActivityEvent{t, ActivityKind::Change, cx, cy, rx1 - rx0, ry1 - ry0});
        }
    }
    m_previous = frame;
}

ActivityTrack ActivityDetector::finish() {
    ActivityTrack out = std::move(m_track);
    std::ranges::stable_sort(out.events, [](const ActivityEvent& a, const ActivityEvent& b) { return a.time < b.time; });
    m_track = ActivityTrack{};
    m_track.origin = "detected";
    return out;
}

std::expected<ActivityTrack, Error> detectActivity(const FrameReader& read, TimeRange source, const DetectOptions& options,
                                                   const std::function<bool(double)>& progress, DetectStats* stats) {
    const auto started = std::chrono::steady_clock::now();
    double analysis = 0.0;
    ActivityDetector detector(options);
    const double fps = std::clamp(options.fps, 1.0, 60.0);
    const double length = source.duration.toSeconds();
    const std::size_t samples = static_cast<std::size_t>(std::floor(length * fps)) + 1;
    for (std::size_t k = 0; k < samples; ++k) {
        const Ticks t = source.start + fromSeconds(static_cast<double>(k) / fps);
        if (t >= source.end()) {
            break;
        }
        if (progress && !progress(static_cast<double>(k) / static_cast<double>(samples))) {
            return std::unexpected(Error(ErrorCode::Cancelled, "auto zoom analysis cancelled"));
        }
        auto frame = read(t, options.width);
        if (!frame) {
            if (frame.error().code == ErrorCode::NotFound && k > 0) {
                break; // past the last frame
            }
            return std::unexpected(frame.error());
        }
        frame->time = t;
        const auto a = std::chrono::steady_clock::now();
        detector.push(*frame);
        analysis += std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
    }
    ActivityTrack track = detector.finish();
    if (stats) {
        stats->frames = detector.frames();
        stats->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        stats->analysisSeconds = analysis;
        stats->framesPerSecond = stats->seconds > 0.0 ? static_cast<double>(stats->frames) / stats->seconds : 0.0;
    }
    return track;
}

} // namespace wabcut::ai::autozoom
