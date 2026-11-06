// The Warp Stabilizer's gyro method (GyroMotion.h): the camera's rotation as a compensating transform per frame.

#include <wabcut/ai/stabilize/GyroMotion.h>

#include <wabcut/gyro/Rotation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <optional>

namespace wabcut::ai::stabilize {

using gyro::Vec3;
using masks::Point2;

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kMaxSyncSeconds = 0.5;    // Auto-sync looks this far either way
constexpr double kSyncCoarseStep = 0.01;
constexpr double kSyncFineStep = 0.001;
constexpr double kUsableMatch = 0.5;       // below this (or 3 / sqrt(pairs), when that is higher) the gyro does not explain the
                                           // pictures well enough to take its numbers
constexpr double kAxisGain = 0.10;         // another axis turn must beat the recorded one by this much to replace it
constexpr int kMinPairs = 24;
constexpr double kMinImageMotion = 0.02;   // rms pixels per frame (analysis width) below which there is nothing to compare

struct AxisTurn {
    std::array<int, 3> source{};  // output row r takes input column source[r]
    std::array<int, 3> sign{};
};

const std::array<AxisTurn, kGyroAxisCount>& axisTurns() {
    static const std::array<AxisTurn, kGyroAxisCount> turns = [] {
        std::array<AxisTurn, kGyroAxisCount> out{};
        std::size_t n = 0;
        static constexpr std::array<std::array<int, 3>, 6> kPerms{{{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}}};
        static constexpr std::array<int, 6> kParity{1, -1, -1, 1, 1, -1}; // the permutation's own determinant
        for (std::size_t p = 0; p < kPerms.size(); ++p) {
            for (int bits = 0; bits < 8; ++bits) {
                const int sx = (bits & 1) ? -1 : 1;
                const int sy = (bits & 2) ? -1 : 1;
                const int sz = (bits & 4) ? -1 : 1;
                if (kParity[p] * sx * sy * sz != 1 || n >= out.size()) {
                    continue;
                }
                out[n++] = AxisTurn{kPerms[p], {sx, sy, sz}};
            }
        }
        return out;
    }();
    return turns;
}

double component(const Vec3& v, int index) { return index == 0 ? v.x : index == 1 ? v.y : v.z; }

Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator*(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }

gyro::GyroTrack turned(const gyro::GyroTrack& track, int axes) {
    gyro::GyroTrack out = track;
    for (Vec3& w : out.omega) {
        w = applyGyroAxes(axes, w);
    }
    return out;
}

Homography rotationHomography(const std::array<double, 9>& r, double focal, Point2 centre) {
    Homography k;
    k.m = {focal, 0.0, centre.x, 0.0, focal, centre.y, 0.0, 0.0, 1.0};
    Homography rot;
    rot.m = r;
    const auto inverse = k.inverted();
    return inverse ? k.after(rot.after(*inverse)) : Homography{};
}

std::array<double, 9> transposed(const std::array<double, 9>& m) {
    return {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]};
}

double fovFromFocal(double focalPixels, int width) { return 2.0 * std::atan(0.5 * width / focalPixels) * 180.0 / kPi; }
double focalFromFov(double degrees, int width) { return 0.5 * width / std::tan(0.5 * degrees * kPi / 180.0); }

// ---- Auto-sync ------------------------------------------------------------------------------------------------------

/// One frame interval of a window: the picture's motion over it (analysis pixels) and the interval's end times.
struct Pair {
    double t0 = 0.0;
    double t1 = 0.0;
    double dx = 0.0;
    double dy = 0.0;
    double roll = 0.0; ///< radians, clockwise on screen
};

struct WindowPairs {
    std::vector<Pair> pairs;
    gyro::RateIntegral integral;
};

struct Score {
    double match = -1.0;
    double focal = 0.0; ///< analysis pixels
    double xy = 0.0;
    double roll = 0.0;
};

Score scoreOf(const std::vector<WindowPairs>& windows, int axes, double offset) {
    double smg = 0.0, smm = 0.0, sgg = 0.0;
    double rmr = 0.0, rmm = 0.0, rrr = 0.0;
    for (const WindowPairs& w : windows) {
        for (const Pair& p : w.pairs) {
            const Vec3 phi = applyGyroAxes(axes, w.integral.between(p.t0 + offset, p.t1 + offset));
            // Content moves against the camera: turning right (+y) moves it left, turning down (+x) moves it up... in the picture
            // (y down) the displacement of the content per radian is (-f * phi_y, +f * phi_x), and it rolls against the camera's z.
            const double gx = -phi.y;
            const double gy = phi.x;
            const double gr = -phi.z;
            smg += p.dx * gx + p.dy * gy;
            smm += p.dx * p.dx + p.dy * p.dy;
            sgg += gx * gx + gy * gy;
            rmr += p.roll * gr;
            rmm += p.roll * p.roll;
            rrr += gr * gr;
        }
    }
    Score s;
    if (!(smm > 0.0) || !(sgg > 0.0) || !(smg > 0.0)) {
        return s;
    }
    s.xy = smg / std::sqrt(smm * sgg);
    s.focal = smg / sgg;
    // The picture's roll is a second, independent check of the axes (it is small and noisy: weighed half).
    const bool rollMeasured = rmm > 1e-8 && rrr > 1e-8;
    s.roll = rollMeasured ? rmr / std::sqrt(rmm * rrr) : 0.0;
    s.match = rollMeasured ? (s.xy + 0.5 * s.roll) / 1.5 : s.xy;
    return s;
}

} // namespace

gyro::Vec3 applyGyroAxes(int index, const gyro::Vec3& v) {
    const auto& turns = axisTurns();
    const AxisTurn& t = turns[static_cast<std::size_t>(std::clamp(index, 0, kGyroAxisCount - 1))];
    return {t.sign[0] * component(v, t.source[0]), t.sign[1] * component(v, t.source[1]), t.sign[2] * component(v, t.source[2])};
}

std::string describeGyroAxes(int index) {
    if (index <= 0 || index >= kGyroAxisCount) {
        return "as recorded";
    }
    const AxisTurn& t = axisTurns()[static_cast<std::size_t>(index)];
    static constexpr const char* kNames[3] = {"x", "y", "z"};
    std::string out;
    for (int r = 0; r < 3; ++r) { // output axis r is input axis source[r]
        if (!out.empty()) {
            out += ", ";
        }
        out += std::string(kNames[t.source[static_cast<std::size_t>(r)]]) + " -> " + (t.sign[static_cast<std::size_t>(r)] < 0 ? "-" : "") + kNames[r];
    }
    return out;
}

double defaultHorizontalFov(gyro::Camera camera) {
    switch (camera) {
    case gyro::Camera::GoPro: return 96.0; // the centre of a Wide lens measured on HERO5 / 6 footage (f = 0.46 x the width)
    case gyro::Camera::Sony: return 70.0;
    case gyro::Camera::None: break;
    }
    return 85.0;
}

GyroFit fitGyroToPictures(const gyro::GyroTrack& track, std::span<const ImageMotionWindow> windows, int sourceWidth) {
    GyroFit fit;
    if (track.times.size() < 2 || sourceWidth < 1) {
        fit.note = "the gyro has no readings to compare";
        return fit;
    }
    // The measured picture motion of every window as frame intervals, with the gyro's integral around each window.
    std::vector<WindowPairs> prepared;
    double analysisWidth = 0.0;
    double energy = 0.0;
    int pairCount = 0;
    for (const ImageMotionWindow& w : windows) {
        const MotionAnalysis& m = w.motion;
        if (m.width < 1 || m.height < 1 || m.frames.size() != w.times.size() || m.frames.size() < 2) {
            continue;
        }
        analysisWidth = m.width;
        const Point2 centre{(m.width - 1) / 2.0, (m.height - 1) / 2.0};
        WindowPairs wp{{}, gyro::RateIntegral(track, w.times.front().toSeconds() - kMaxSyncSeconds - 0.2,
                                              w.times.back().toSeconds() + kMaxSyncSeconds + 0.2)};
        for (std::size_t k = 1; k < m.frames.size(); ++k) {
            const FrameMotion& f = m.frames[k];
            if (f.lost || m.frames[k - 1].lost) {
                continue;
            }
            const Point2 moved = f.motion.map(centre);
            Pair p;
            p.t0 = w.times[k - 1].toSeconds();
            p.t1 = w.times[k].toSeconds();
            p.dx = moved.x - centre.x;
            p.dy = moved.y - centre.y;
            p.roll = std::atan2(0.5 * (f.motion.m[3] - f.motion.m[1]), 0.5 * (f.motion.m[0] + f.motion.m[4]));
            if (!std::isfinite(p.dx) || !std::isfinite(p.dy) || !std::isfinite(p.roll) || !(p.t1 > p.t0)) {
                continue;
            }
            energy += p.dx * p.dx + p.dy * p.dy;
            ++pairCount;
            wp.pairs.push_back(p);
        }
        if (!wp.pairs.empty() && !wp.integral.empty()) {
            prepared.push_back(std::move(wp));
        }
    }
    fit.pairs = pairCount;
    if (pairCount < kMinPairs || prepared.empty() || !(analysisWidth > 0.0)) {
        fit.note = "too few frames to compare the gyro with the picture";
        return fit;
    }
    if (std::sqrt(energy / pairCount) < kMinImageMotion) {
        fit.note = "the picture hardly moves, so there is nothing to compare the gyro with";
        return fit;
    }
    fit.measured = true;

    const auto searchOffset = [&](int axes, double from, double to, double step, double& bestOffset) {
        Score best;
        for (double offset = from; offset <= to + 1e-9; offset += step) {
            const Score s = scoreOf(prepared, axes, offset);
            if (s.match > best.match) {
                best = s;
                bestOffset = offset;
            }
        }
        return best;
    };
    // The axes as recorded first, then every other turn; a turn replaces the recorded one only if it is clearly better.
    double nominalOffset = 0.0;
    const Score nominal = searchOffset(0, -kMaxSyncSeconds, kMaxSyncSeconds, kSyncCoarseStep, nominalOffset);
    int bestAxes = 0;
    double bestOffset = nominalOffset;
    Score best = nominal;
    for (int axes = 1; axes < kGyroAxisCount; ++axes) {
        double offset = 0.0;
        const Score s = searchOffset(axes, -kMaxSyncSeconds, kMaxSyncSeconds, kSyncCoarseStep, offset);
        if (s.match > best.match) {
            best = s;
            bestAxes = axes;
            bestOffset = offset;
        }
    }
    if (bestAxes != 0 && !(best.match >= nominal.match + kAxisGain)) {
        bestAxes = 0;
        best = nominal;
        bestOffset = nominalOffset;
    }
    // Refine the offset to a millisecond around the coarse optimum.
    double refined = bestOffset;
    const Score fine = searchOffset(bestAxes, bestOffset - kSyncCoarseStep, bestOffset + kSyncCoarseStep, kSyncFineStep, refined);
    if (fine.match >= best.match) {
        best = fine;
        bestOffset = refined;
    }
    fit.match = std::clamp(best.match, 0.0, 1.0);
    fit.offsetSeconds = bestOffset;
    fit.axes = bestAxes;
    const double focalSource = best.focal * static_cast<double>(sourceWidth) / analysisWidth;
    const bool plausibleFocal = focalSource >= 0.15 * sourceWidth && focalSource <= 4.0 * sourceWidth;
    if (plausibleFocal) {
        fit.focalPixels = focalSource;
        fit.horizontalFovDegrees = fovFromFocal(focalSource, sourceWidth);
    }
    fit.usable = best.match >= std::max(kUsableMatch, 3.0 / std::sqrt(static_cast<double>(pairCount))) && plausibleFocal;
    char note[200];
    if (fit.usable) {
        std::snprintf(note, sizeof note, "synced to the picture: gyro %+.0f ms, field of view %.0f degrees, match %.0f%%%s",
                      fit.offsetSeconds * 1000.0, fit.horizontalFovDegrees, fit.match * 100.0,
                      bestAxes != 0 ? ", axes turned from the camera's table" : "");
    } else {
        std::snprintf(note, sizeof note,
                      "the gyro matches the picture only weakly (%.0f%%): in-camera stabilization on, or too little camera motion",
                      fit.match * 100.0);
    }
    fit.note = note;
    return fit;
}

GyroSetup resolveGyroSetup(const gyro::GyroTrack& track, const GyroOptions& options, const GyroFit& fit, int sourceWidth) {
    GyroSetup s;
    s.fit = fit;
    s.synced = options.autoSync && fit.usable;
    s.offsetSeconds = options.offsetSeconds + (s.synced ? fit.offsetSeconds : 0.0);
    s.axes = s.synced ? fit.axes : 0;
    if (options.horizontalFovDegrees > 1.0) {
        s.horizontalFovDegrees = std::clamp(options.horizontalFovDegrees, 5.0, 175.0);
        s.fovFrom = "set";
    } else if (s.synced && fit.horizontalFovDegrees > 0.0) {
        s.horizontalFovDegrees = fit.horizontalFovDegrees;
        s.fovFrom = "picture";
    } else if (track.horizontalFovDegrees && *track.horizontalFovDegrees > 5.0) {
        s.horizontalFovDegrees = *track.horizontalFovDegrees;
        s.fovFrom = "file";
    } else {
        s.horizontalFovDegrees = defaultHorizontalFov(track.camera);
        s.fovFrom = "camera";
    }
    s.focalPixels = focalFromFov(s.horizontalFovDegrees, std::max(sourceWidth, 1));
    s.readoutSeconds = options.readoutSeconds > 0.0 ? options.readoutSeconds : track.readoutSeconds.value_or(0.0);
    return s;
}

Compensation stabilizeFromGyro(const gyro::GyroTrack& trackIn, std::span<const Ticks> frameTimes, const GyroSetup& setup,
                               const SmoothOptions& options, int sourceWidth, int sourceHeight) {
    const std::size_t n = frameTimes.size();
    if (n == 0 || sourceWidth < 1 || sourceHeight < 1 || trackIn.times.size() < 2) {
        return {};
    }
    std::optional<gyro::GyroTrack> turnedCopy;
    if (setup.axes > 0) {
        turnedCopy = turned(trackIn, setup.axes);
    }
    const gyro::GyroTrack& track = turnedCopy ? *turnedCopy : trackIn;
    const Point2 centre{(sourceWidth - 1) / 2.0, (sourceHeight - 1) / 2.0};
    const double frameRate = options.frameRate > 0.0 ? std::clamp(options.frameRate, 1e-3, 1000.0) : 30.0;

    // 1-2. The camera's orientation at the middle line's exposure of every frame.
    std::vector<double> at(n);
    for (std::size_t i = 0; i < n; ++i) {
        at[i] = frameTimes[i].toSeconds() + setup.offsetSeconds + 0.5 * setup.readoutSeconds;
    }
    const std::vector<gyro::Quat> q = gyro::orientationAt(track, at);
    constexpr double kReach = 0.25; // seconds beyond the readings a frame still counts as covered
    std::vector<char> covered(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        covered[i] = at[i] >= track.times.front() - kReach && at[i] <= track.times.back() + kReach;
    }

    // 3. The smoothed camera against the real one, per frame: a_i, a rotation vector in the real camera's frame.
    const bool cropping = options.framing == Framing::Crop || options.framing == Framing::CropAutoScale;
    const double tradeOff = cropping ? 0.5 + std::clamp(options.cropLessSmoothMore, 0.0, 1.0) : 1.0;
    const auto smoothnessOf = [&](std::size_t i) {
        const double s = options.smoothnessAt.size() == n ? options.smoothnessAt[i] : options.smoothness;
        return std::clamp(std::isfinite(s) ? s : 0.5, 0.0, 10.0) * tradeOff;
    };
    std::vector<Vec3> offset(n);
    std::size_t firstCovered = 0;
    while (firstCovered < n && !covered[firstCovered]) {
        ++firstCovered;
    }
    for (std::size_t i = 0; i < n && firstCovered < n; ++i) {
        if (!covered[i]) {
            continue;
        }
        if (options.noMotion) {
            offset[i] = gyro::toRotationVector(gyro::conjugate(q[i]) * q[firstCovered]);
            continue;
        }
        const double s = smoothnessOf(i);
        if (!(s > 0.0)) {
            continue; // 0 % leaves the frame as shot
        }
        const double sigma = std::max(s * frameRate, 0.5);
        const auto radius = static_cast<std::size_t>(std::min(std::ceil(3.0 * sigma), static_cast<double>(n)));
        const std::size_t lo = i > radius ? i - radius : 0;
        const std::size_t hi = std::min(n - 1, i + radius);
        double s0 = 0.0, s1 = 0.0, s2 = 0.0;
        Vec3 t0, t1;
        const gyro::Quat inverse = gyro::conjugate(q[i]);
        for (std::size_t j = lo; j <= hi; ++j) {
            if (!covered[j]) {
                continue;
            }
            const double d = static_cast<double>(j) - static_cast<double>(i);
            const double w = std::exp(-0.5 * (d / sigma) * (d / sigma));
            const Vec3 v = j == i ? Vec3{} : gyro::toRotationVector(inverse * q[j]);
            s0 += w;
            s1 += w * d;
            s2 += w * d * d;
            t0 = t0 + v * w;
            t1 = t1 + v * (w * d);
        }
        if (!(s0 > 0.0)) {
            continue;
        }
        const double det = s0 * s2 - s1 * s1;
        // The intercept of the local linear fit (the same estimator as the picture-based path: a pan keeps its speed to the
        // last frame); a window with no spread falls back to the weighted mean.
        offset[i] = det > 1e-9 * s0 * std::max(s2, 1.0) ? (t0 * s2 + t1 * (-s1)) * (1.0 / det) : t0 * (1.0 / s0);
    }

    // 4-5. The correction as a homography on the picture; a rolling shutter's skew comes out first.
    const double share = std::clamp(setup.readoutSeconds * frameRate, 0.0, 1.0);
    std::optional<gyro::RateIntegral> rates;
    if (share > 0.0) {
        rates.emplace(track, at.front() - 1.0, at.back() + 1.0);
    }
    std::vector<Homography> compensation(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (!covered[i]) {
            continue; // identity: as shot
        }
        const auto rotation = gyro::toMatrix(gyro::fromRotationVector(offset[i]));
        Homography h = rotationHomography(transposed(rotation), setup.focalPixels, centre);
        if (rates && !rates->empty()) {
            const double period = 1.0 / frameRate;
            const Vec3 omega = rates->between(at[i] - 0.5 * period, at[i] + 0.5 * period) * (1.0 / period); // rad/s over the frame
            const double vx = -setup.focalPixels * omega.y / frameRate; // the content's pixels per frame at the centre
            const double vy = setup.focalPixels * omega.x / frameRate;
            if (const auto skewInverse = rollingShutterSkew(share, vx, vy, sourceHeight).inverted()) {
                h = h.after(*skewInverse);
            }
        }
        if (std::all_of(h.m.begin(), h.m.end(), [](double v) { return std::isfinite(v); })) {
            compensation[i] = h;
        }
    }
    return frameCompensations(frameTimes, compensation, options, sourceWidth, sourceHeight, true);
}

std::string gyroSummaryText(const gyro::GyroTrack& track, const GyroSetup& setup, bool autoSyncAsked) {
    const auto clean = [](std::string s) {
        for (char& c : s) {
            if (c == ';' || c == '=' || c == '\n' || c == '\r') {
                c = ' ';
            }
        }
        return s;
    };
    const std::string camera = clean(track.make + (track.model.empty() ? std::string() : " " + track.model));
    const char* sync = !autoSyncAsked ? "off" : setup.synced ? "picture" : setup.fit.measured ? "weak" : "none";
    char text[640];
    std::snprintf(text, sizeof text,
                  "camera=%s;format=%s;rateHz=%.1f;readoutMs=%.2f;offsetMs=%.1f;fov=%.1f;fovFrom=%s;sync=%s;match=%.2f;axes=%s;"
                  "axesChecked=%d;note=%s",
                  camera.c_str(), clean(track.format).c_str(), track.rateHz, setup.readoutSeconds * 1000.0, setup.offsetSeconds * 1000.0,
                  setup.horizontalFovDegrees, setup.fovFrom.c_str(), sync, setup.fit.match, clean(describeGyroAxes(setup.axes)).c_str(),
                  track.axesChecked ? 1 : 0, clean(setup.fit.note).c_str());
    return text;
}

} // namespace wabcut::ai::stabilize
