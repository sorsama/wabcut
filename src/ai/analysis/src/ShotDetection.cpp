#include <wabcut/ai/analysis/ShotDetection.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <limits>

namespace wabcut::ai::analysis {

namespace {

/// One frame as the detectors keep it.
struct Frame {
    std::int64_t index = 0;
    Ticks pts;
    Ticks duration;
    std::vector<std::uint8_t> rgb;
    std::vector<std::uint8_t> hsv; ///< OpenCV 8-bit HSV: H 0..179, S and V 0..255
    double mean = 0.0;             ///< mean of all RGB values (PySceneDetect's frame intensity)
};

void toHsv(const std::vector<std::uint8_t>& rgb, std::vector<std::uint8_t>& hsv) {
    hsv.resize(rgb.size());
    for (std::size_t i = 0; i + 2 < rgb.size(); i += 3) {
        const int r = rgb[i];
        const int g = rgb[i + 1];
        const int b = rgb[i + 2];
        const int v = std::max({r, g, b});
        const int lo = std::min({r, g, b});
        const int delta = v - lo;
        const int s = v == 0 ? 0 : (255 * delta + v / 2) / v;
        int h = 0;
        if (delta > 0) {
            double hd = 0.0;
            if (v == r) {
                hd = 60.0 * (g - b) / delta;
            } else if (v == g) {
                hd = 120.0 + 60.0 * (b - r) / delta;
            } else {
                hd = 240.0 + 60.0 * (r - g) / delta;
            }
            if (hd < 0.0) {
                hd += 360.0;
            }
            h = static_cast<int>(std::lround(hd / 2.0)) % 180;
        }
        hsv[i] = static_cast<std::uint8_t>(h);
        hsv[i + 1] = static_cast<std::uint8_t>(s);
        hsv[i + 2] = static_cast<std::uint8_t>(v);
    }
}

/// Below this V (or S, for hue) the other HSV components are noise: a near-black pixel's hue and saturation, a grey
/// pixel's hue, jump with the slightest change.
constexpr int kStableLevel = 32;

/// PySceneDetect's content value: the mean of the per-channel mean absolute HSV differences (weights 1, 1, 1), with
/// hue compared only between saturated, lit pixels and saturation only between lit ones (so fades and grey
/// pictures do not look like cuts).
double contentValue(const Frame& a, const Frame& b) {
    std::int64_t sum[3] = {0, 0, 0};
    const std::size_t n = std::min(a.hsv.size(), b.hsv.size());
    for (std::size_t i = 0; i + 2 < n; i += 3) {
        const int va = a.hsv[i + 2];
        const int vb = b.hsv[i + 2];
        const bool lit = va >= kStableLevel && vb >= kStableLevel;
        if (lit && a.hsv[i + 1] >= kStableLevel && b.hsv[i + 1] >= kStableLevel) {
            const int dh = std::abs(static_cast<int>(a.hsv[i]) - static_cast<int>(b.hsv[i]));
            sum[0] += std::min(dh, 180 - dh); // hue is circular
        }
        if (lit) {
            sum[1] += std::abs(static_cast<int>(a.hsv[i + 1]) - static_cast<int>(b.hsv[i + 1]));
        }
        sum[2] += std::abs(va - vb);
    }
    const double pixels = static_cast<double>(n / 3);
    if (pixels <= 0.0) {
        return 0.0;
    }
    return (static_cast<double>(sum[0]) + static_cast<double>(sum[1]) + static_cast<double>(sum[2])) / (3.0 * pixels);
}

/// Mean absolute RGB difference.
double rgbDistance(const Frame& a, const Frame& b) {
    std::int64_t sum = 0;
    const std::size_t n = std::min(a.rgb.size(), b.rgb.size());
    for (std::size_t i = 0; i < n; ++i) {
        sum += std::abs(static_cast<int>(a.rgb[i]) - static_cast<int>(b.rgb[i]));
    }
    return n == 0 ? 0.0 : static_cast<double>(sum) / static_cast<double>(n);
}

/// Mean absolute difference between `mid` and the average of `a` and `b`.
double blendError(const Frame& a, const Frame& mid, const Frame& b) {
    std::int64_t sum = 0;
    const std::size_t n = std::min({a.rgb.size(), mid.rgb.size(), b.rgb.size()});
    for (std::size_t i = 0; i < n; ++i) {
        sum += std::abs(2 * static_cast<int>(mid.rgb[i]) - static_cast<int>(a.rgb[i]) - static_cast<int>(b.rgb[i]));
    }
    return n == 0 ? 0.0 : static_cast<double>(sum) / (2.0 * static_cast<double>(n));
}

int priority(CutKind kind) {
    switch (kind) {
    case CutKind::Cut:
        return 3;
    case CutKind::Fade:
        return 2;
    case CutKind::Dissolve:
        return 1;
    }
    return 0;
}

class Detector {
public:
    explicit Detector(const ShotOptions& o) : m_o(o) {
        m_flash = std::max(o.flashFrames, 0);
        m_window = std::max(o.adaptiveWindow, 1);
        m_half = std::max(o.dissolveFrames, 2) / 2;
        m_lookahead = std::max(m_flash, m_window);
        m_keep = static_cast<std::size_t>(std::max(2 * m_half, m_lookahead + m_flash + 2) + 2);
    }

    std::expected<void, Error> push(std::optional<AnalysisFrame> input) {
        if (!input) {
            flush();
            return {};
        }
        Frame f;
        f.index = m_count++;
        f.pts = input->pts;
        f.duration = input->duration;
        f.rgb = std::move(input->rgb);
        if (m_width == 0) {
            m_width = input->width;
            m_height = input->height;
        } else if (input->width != m_width || input->height != m_height ||
                   f.rgb.size() != static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height) * 3) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "analysis frames changed size"});
        }
        toHsv(f.rgb, f.hsv);
        double total = 0.0;
        for (const std::uint8_t v : f.rgb) {
            total += v;
        }
        f.mean = f.rgb.empty() ? 0.0 : total / static_cast<double>(f.rgb.size());
        m_pts.push_back(f.pts);
        m_means.push_back(f.mean);
        m_end = f.pts + f.duration;
        m_content.push_back(m_frames.empty() ? 0.0 : contentValue(m_frames.back(), f));
        fade(f);
        m_frames.push_back(std::move(f));
        const std::int64_t n = m_count - 1;
        if (n - m_lookahead >= 1) {
            hardCut(n - m_lookahead, n);
        }
        if (n - 2 * m_half >= 0) {
            dissolve(n - m_half);
        }
        while (m_frames.size() > m_keep) {
            m_frames.pop_front();
        }
        return {};
    }

    ShotAnalysis result() {
        ShotAnalysis out;
        out.frames = m_count;
        out.start = m_pts.empty() ? Ticks{} : m_pts.front();
        out.end = m_end;
        closeDissolveRun();
        std::sort(m_cuts.begin(), m_cuts.end(), [](const ShotCut& a, const ShotCut& b) { return a.frame < b.frame; });
        const auto minGap = static_cast<std::int64_t>(std::max(m_o.minShotFrames, 1));
        for (const ShotCut& c : m_cuts) {
            if (c.frame <= 0) {
                continue;
            }
            if (!out.cuts.empty() && c.frame - out.cuts.back().frame < minGap) {
                ShotCut& prev = out.cuts.back();
                const bool stronger = priority(c.kind) > priority(prev.kind) ||
                                      (priority(c.kind) == priority(prev.kind) && c.confidence > prev.confidence);
                if (stronger) {
                    prev = c;
                }
                continue;
            }
            out.cuts.push_back(c);
        }
        return out;
    }

private:
    bool hardCuts() const { return m_o.detector != ShotDetector::Threshold; }
    bool fades() const { return m_o.detector == ShotDetector::Combined || m_o.detector == ShotDetector::Threshold; }
    bool dissolves() const { return m_o.detector == ShotDetector::Combined; }

    const Frame* frame(std::int64_t index) const {
        if (m_frames.empty()) {
            return nullptr;
        }
        const std::int64_t first = m_frames.front().index;
        if (index < first || index > m_frames.back().index) {
            return nullptr;
        }
        return &m_frames[static_cast<std::size_t>(index - first)];
    }

    void add(std::int64_t frameIndex, CutKind kind, double confidence) {
        if (frameIndex <= 0 || frameIndex >= static_cast<std::int64_t>(m_pts.size())) {
            return;
        }
        m_cuts.push_back(ShotCut{m_pts[static_cast<std::size_t>(frameIndex)], frameIndex, kind,
                                 static_cast<float>(std::clamp(confidence, 0.0, 0.99))});
    }

    /// The boundary before frame j, with frames up to `last` available.
    void hardCut(std::int64_t j, std::int64_t last) {
        if (!hardCuts() || j < 1) {
            return;
        }
        const double c = m_content[static_cast<std::size_t>(j)];
        double neighbours = 0.0;
        int count = 0;
        for (std::int64_t k = j - m_window; k <= j + m_window; ++k) {
            if (k != j && k >= 1 && k <= last) {
                neighbours += m_content[static_cast<std::size_t>(k)];
                ++count;
            }
        }
        const double average = count > 0 ? neighbours / count : 0.0;
        const double ratio = average > 1e-9 ? c / average : (c > 0.0 ? 1e9 : 0.0);
        const bool content = c >= m_o.contentThreshold;
        const bool adaptive = ratio >= m_o.adaptiveThreshold && c >= m_o.minContentValue;
        bool cut = false;
        switch (m_o.detector) {
        case ShotDetector::Content:
            cut = content;
            break;
        case ShotDetector::Adaptive:
            cut = adaptive;
            break;
        case ShotDetector::Combined:
            cut = adaptive || c >= 2.0 * m_o.contentThreshold;
            break;
        case ShotDetector::Threshold:
            break;
        }
        if (!cut || isFlash(j, last)) {
            return;
        }
        const double strength = std::min(1.0, (c - m_o.minContentValue) / (2.0 * m_o.contentThreshold));
        add(j, CutKind::Cut, 0.5 + 0.49 * std::max(0.0, strength));
    }

    /// A change at the boundary before j that returns to the picture before it (or one that comes back from a
    /// picture seen just before) within flashFrames frames.
    bool isFlash(std::int64_t j, std::int64_t last) const {
        if (m_flash <= 0) {
            return false;
        }
        const double similar = std::min(m_o.contentThreshold, m_o.minContentValue);
        const Frame* before = frame(j - 1);
        const Frame* after = frame(j);
        for (std::int64_t k = j + 1; before && k <= std::min(j + m_flash, last); ++k) {
            if (const Frame* f = frame(k); f && contentValue(*before, *f) < similar) {
                return true;
            }
        }
        for (std::int64_t m = j - 2; after && m >= std::max<std::int64_t>(0, j - 1 - m_flash); --m) {
            if (const Frame* f = frame(m); f && contentValue(*f, *after) < similar) {
                return true;
            }
        }
        return false;
    }

    /// ThresholdDetector: a run of dark frames between brighter ones.
    void fade(const Frame& f) {
        if (!fades()) {
            return;
        }
        const bool dark = f.mean < m_o.fadeThreshold;
        if (f.index == 0) {
            m_inFade = dark;
            m_startedDark = dark;
            m_fadeOut = 0;
            return;
        }
        if (!m_inFade && dark) {
            m_inFade = true;
            m_fadeOut = f.index;
        } else if (m_inFade && !dark) {
            m_inFade = false;
            if (!m_startedDark) {
                const double t = (1.0 + std::clamp(m_o.fadeBias, -1.0, 1.0)) / 2.0;
                const auto at = m_fadeOut + static_cast<std::int64_t>(std::llround(static_cast<double>(f.index - m_fadeOut) * t));
                add(std::clamp(at, m_fadeOut, f.index), CutKind::Fade, 0.9);
            }
            m_startedDark = false;
        }
    }

    /// The window [m - half, m + half] is a blend from its first to its last frame.
    void dissolve(std::int64_t m) {
        if (!dissolves()) {
            return;
        }
        const Frame* a = frame(m - m_half);
        const Frame* mid = frame(m);
        const Frame* b = frame(m + m_half);
        bool blend = a && mid && b;
        double quality = 0.0;
        if (blend) {
            for (std::int64_t k = m - m_half + 1; k <= m + m_half; ++k) {
                blend = blend && m_content[static_cast<std::size_t>(k)] < m_o.contentThreshold;
            }
            // Fades through black are the ThresholdDetector's: no dark frame in or around the window.
            const double dark = 2.0 * m_o.fadeThreshold;
            for (std::int64_t k = std::max<std::int64_t>(0, m - 3 * m_half); k <= m + m_half && blend; ++k) {
                blend = m_means[static_cast<std::size_t>(k)] >= dark;
            }
        }
        if (blend) {
            const double ends = rgbDistance(*a, *b);
            const double error = blendError(*a, *mid, *b);
            blend = ends >= m_o.dissolveThreshold && error <= 0.2 * ends;
            quality = ends > 0.0 ? 1.0 - error / (0.2 * ends) : 0.0;
        }
        if (blend) {
            if (m_runStart < 0) {
                m_runStart = m;
                m_runQuality = 0.0;
            }
            m_runEnd = m;
            m_runQuality = std::max(m_runQuality, quality);
        } else {
            closeDissolveRun();
        }
    }

    void closeDissolveRun() {
        if (m_runStart >= 0) {
            add((m_runStart + m_runEnd + 1) / 2, CutKind::Dissolve, 0.4 + 0.4 * std::clamp(m_runQuality, 0.0, 1.0));
        }
        m_runStart = -1;
    }

    void flush() {
        const std::int64_t last = m_count - 1;
        for (std::int64_t j = std::max<std::int64_t>(1, last - m_lookahead + 1); j <= last; ++j) {
            hardCut(j, last);
        }
    }

    const ShotOptions& m_o;
    int m_flash = 0;
    int m_window = 1;
    int m_half = 6;
    int m_lookahead = 1;
    std::size_t m_keep = 16;
    int m_width = 0;
    int m_height = 0;
    std::int64_t m_count = 0;
    std::deque<Frame> m_frames;
    std::vector<double> m_content;
    std::vector<Ticks> m_pts;
    std::vector<double> m_means;
    Ticks m_end;
    std::vector<ShotCut> m_cuts;
    bool m_inFade = false;
    bool m_startedDark = false;
    std::int64_t m_fadeOut = 0;
    std::int64_t m_runStart = -1;
    std::int64_t m_runEnd = -1;
    double m_runQuality = 0.0;
};

} // namespace

std::string_view toString(ShotDetector detector) {
    switch (detector) {
    case ShotDetector::Combined:
        return "combined";
    case ShotDetector::Content:
        return "content";
    case ShotDetector::Adaptive:
        return "adaptive";
    case ShotDetector::Threshold:
        return "threshold";
    }
    return "combined";
}

std::optional<ShotDetector> shotDetectorFromString(std::string_view name) {
    for (const auto d : {ShotDetector::Combined, ShotDetector::Content, ShotDetector::Adaptive, ShotDetector::Threshold}) {
        if (toString(d) == name) {
            return d;
        }
    }
    return std::nullopt;
}

std::string_view toString(CutKind kind) {
    switch (kind) {
    case CutKind::Cut:
        return "cut";
    case CutKind::Fade:
        return "fade";
    case CutKind::Dissolve:
        return "dissolve";
    }
    return "cut";
}

std::optional<CutKind> cutKindFromString(std::string_view name) {
    for (const auto k : {CutKind::Cut, CutKind::Fade, CutKind::Dissolve}) {
        if (toString(k) == name) {
            return k;
        }
    }
    return std::nullopt;
}

std::string ShotOptions::fingerprint() const {
    char buffer[256];
    std::snprintf(buffer, sizeof buffer, "%s c%.3f a%.3f/%d m%.3f f%.3f/%.3f fl%d d%d/%.3f s%d", std::string(toString(detector)).c_str(),
                  contentThreshold, adaptiveThreshold, adaptiveWindow, minContentValue, fadeThreshold, fadeBias,
                  flashFrames, dissolveFrames, dissolveThreshold, minShotFrames);
    return buffer;
}

std::expected<std::optional<AnalysisFrame>, Error> VectorFrameSource::next() {
    if (m_next >= m_frames.size()) {
        return std::optional<AnalysisFrame>{};
    }
    return std::optional<AnalysisFrame>{std::move(m_frames[m_next++])};
}

std::expected<ShotAnalysis, Error> detectShots(FrameSource& source, const ShotOptions& options,
                                               const AnalysisProgress& progress) {
    Detector detector(options);
    const std::optional<Ticks> total = source.duration();
    std::int64_t count = 0;
    while (true) {
        auto next = source.next();
        if (!next) {
            return std::unexpected(next.error());
        }
        const bool end = !next->has_value();
        Ticks pts = end ? Ticks{} : (*next)->pts;
        if (auto pushed = detector.push(std::move(*next)); !pushed) {
            return std::unexpected(pushed.error());
        }
        if (end) {
            break;
        }
        if (progress && ++count % 25 == 0) {
            const double fraction = total && total->value > 0
                                        ? std::clamp(static_cast<double>(pts.value) / static_cast<double>(total->value), 0.0, 1.0)
                                        : 0.0;
            if (!progress(fraction)) {
                return std::unexpected(Error{ErrorCode::Cancelled, "shot detection was stopped"});
            }
        }
    }
    return detector.result();
}

} // namespace wabcut::ai::analysis
