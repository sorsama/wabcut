#include <wabcut/playback/audio/ChannelLayout.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace wabcut::playback {

namespace {

constexpr int kL = kChannelL, kR = kChannelR, kC = kChannelC, kLfe = kChannelLfe, kLs = kChannelLs51, kRs = kChannelRs51;

/// The full-range speakers of 5.1 around the listener: (azimuth in degrees, channel), in ring order.
constexpr std::array<std::pair<double, int>, 5> kRing{{{-110.0, kLs}, {-30.0, kL}, {0.0, kC}, {30.0, kR}, {110.0, kRs}}};
/// 7.1: the rear pair behind the side pair (the channel order is a WAVE 7.1 file's: Lrs, Rrs, then Ls, Rs).
constexpr std::array<std::pair<double, int>, 7> kRing71{{{-150.0, kChannelLrs71},
                                                         {-90.0, kChannelLs71},
                                                         {-30.0, kL},
                                                         {0.0, kC},
                                                         {30.0, kR},
                                                         {90.0, kChannelRs71},
                                                         {150.0, kChannelRrs71}}};

double wrap180(double degrees) {
    double d = std::fmod(degrees, 360.0);
    if (d <= -180.0) {
        d += 360.0;
    } else if (d > 180.0) {
        d -= 360.0;
    }
    return d;
}

/// Clean values within float noise of 0 and 1, so the panner's defaults route exactly.
float snap(double v) {
    if (std::fabs(v) < 1e-6) {
        return 0.0f;
    }
    if (std::fabs(v - 1.0) < 1e-6) {
        return 1.0f;
    }
    return static_cast<float>(v);
}

} // namespace

bool ChannelMatrix::isIdentity() const {
    if (rows != cols) {
        return false;
    }
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            if (at(r, c) != (r == c ? 1.0f : 0.0f)) {
                return false;
            }
        }
    }
    return true;
}

ChannelMatrix ChannelMatrix::operator*(const ChannelMatrix& other) const {
    ChannelMatrix out(rows, other.cols);
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < other.cols; ++c) {
            double sum = 0.0;
            for (int k = 0; k < cols && k < other.rows; ++k) {
                sum += static_cast<double>(at(r, k)) * other.at(k, c);
            }
            out.at(r, c) = static_cast<float>(sum);
        }
    }
    return out;
}

void ChannelMatrix::apply(const float* in, float* out, std::size_t frames) const {
    const auto rc = static_cast<std::size_t>(rows);
    const auto cc = static_cast<std::size_t>(cols);
    for (std::size_t f = 0; f < frames; ++f) {
        const float* src = in + f * cc;
        float* dst = out + f * rc;
        for (std::size_t r = 0; r < rc; ++r) {
            const float* row = values.data() + r * cc;
            float sum = 0.0f;
            for (std::size_t c = 0; c < cc; ++c) {
                sum += row[c] * src[c];
            }
            dst[r] = sum;
        }
    }
}

namespace {

/// 7.1 -> 5.1: the side pair as it is, the rear pair folded in at -3 dB.
ChannelMatrix down71To51() {
    ChannelMatrix out(6, 8);
    for (const int k : {kL, kR, kC, kLfe}) {
        out.at(k, k) = 1.0f;
    }
    out.at(kLs, kChannelLs71) = 1.0f;
    out.at(kLs, kChannelLrs71) = kMinus3dB;
    out.at(kRs, kChannelRs71) = 1.0f;
    out.at(kRs, kChannelRrs71) = kMinus3dB;
    return out;
}

/// 5.1 -> 7.1: 5.1's surrounds to the side pair, the rear pair silent.
ChannelMatrix up51To71() {
    ChannelMatrix out(8, 6);
    for (const int k : {kL, kR, kC, kLfe}) {
        out.at(k, k) = 1.0f;
    }
    out.at(kChannelLs71, kLs) = 1.0f;
    out.at(kChannelRs71, kRs) = 1.0f;
    return out;
}

} // namespace

ChannelMatrix upmix51To71(bool copyToRear) {
    ChannelMatrix out = up51To71();
    if (copyToRear) {
        // Each surround is split over the side and the rear speaker of its side: constant power (-3 dB each).
        out.at(kChannelLs71, kLs) = kMinus3dB;
        out.at(kChannelLrs71, kLs) = kMinus3dB;
        out.at(kChannelRs71, kRs) = kMinus3dB;
        out.at(kChannelRrs71, kRs) = kMinus3dB;
    }
    return out;
}

ChannelMatrix formatMatrix(ChannelFormat from, int fromChannels, ChannelFormat to, int toChannels) {
    const int n = std::max(fromChannels, 1);
    const int m = std::max(toChannels, 1);
    ChannelMatrix out(m, n);
    const auto passthrough = [&] {
        for (int k = 0; k < std::min(n, m); ++k) {
            out.at(k, k) = 1.0f;
        }
    };
    if (from == ChannelFormat::Adaptive || to == ChannelFormat::Adaptive || from == to) {
        passthrough();
        return out;
    }
    if (from == ChannelFormat::Surround71) { // 0.7: through 5.1
        const ChannelMatrix to51 = down71To51();
        return to == ChannelFormat::Surround51 ? to51 : formatMatrix(ChannelFormat::Surround51, 6, to, m) * to51;
    }
    if (to == ChannelFormat::Surround71) {
        const ChannelMatrix from51 = up51To71();
        return from == ChannelFormat::Surround51 ? from51 : from51 * formatMatrix(from, n, ChannelFormat::Surround51, 6);
    }
    switch (from) {
    case ChannelFormat::Mono:
        if (to == ChannelFormat::Stereo) {
            out.at(kL, 0) = kMinus3dB;
            out.at(kR, 0) = kMinus3dB;
        } else if (to == ChannelFormat::Surround51) {
            out.at(kC, 0) = 1.0f;
        }
        break;
    case ChannelFormat::Stereo:
        if (to == ChannelFormat::Mono) {
            out.at(0, kL) = 0.5f;
            out.at(0, kR) = 0.5f;
        } else if (to == ChannelFormat::Surround51) {
            out.at(kL, kL) = 1.0f;
            out.at(kR, kR) = 1.0f;
        }
        break;
    case ChannelFormat::Surround51:
        if (to == ChannelFormat::Stereo) { // ITU-R BS.775
            out.at(kL, kL) = 1.0f;
            out.at(kL, kC) = kMinus3dB;
            out.at(kL, kLs) = kMinus3dB;
            out.at(kR, kR) = 1.0f;
            out.at(kR, kC) = kMinus3dB;
            out.at(kR, kRs) = kMinus3dB;
        } else if (to == ChannelFormat::Mono) {
            out.at(0, kL) = 0.5f;
            out.at(0, kR) = 0.5f;
            out.at(0, kC) = kMinus3dB;
            out.at(0, kLs) = 0.5f * kMinus3dB;
            out.at(0, kRs) = 0.5f * kMinus3dB;
        }
        break;
    case ChannelFormat::Adaptive:
    case ChannelFormat::Surround71: // both answered above
        break;
    }
    return out;
}

namespace {

/// The VBAP gains over the speakers of `ring` (azimuth, channel; in ring order, sorted from -180 to +180) at azimuth
/// `degrees`; the unfocused sound is shared by the ring's N speakers.
template <std::size_t N>
std::array<float, 8> ringGains(const std::array<std::pair<double, int>, N>& ring, double degrees, double focus) {
    const double theta = wrap180(degrees);
    const double f = std::clamp(focus, 0.0, 1.0);
    // The speaker pair around theta (the ring wraps from its last speaker to its first through the back).
    std::size_t a = ring.size() - 1;
    std::size_t b = 0;
    for (std::size_t i = 0; i + 1 < ring.size(); ++i) {
        if (theta >= ring[i].first && theta <= ring[i + 1].first) {
            a = i;
            b = i + 1;
            break;
        }
    }
    const auto unit = [](double deg) {
        const double r = deg * std::numbers::pi / 180.0;
        return std::array<double, 2>{std::sin(r), std::cos(r)};
    };
    const auto p = unit(theta);
    const auto la = unit(ring[a].first);
    const auto lb = unit(ring[b].first);
    // Solve ga * la + gb * lb = p.
    const double det = la[0] * lb[1] - la[1] * lb[0];
    double ga = (p[0] * lb[1] - p[1] * lb[0]) / det;
    double gb = (la[0] * p[1] - la[1] * p[0]) / det;
    ga = std::max(ga, 0.0);
    gb = std::max(gb, 0.0);
    const double norm = std::sqrt(ga * ga + gb * gb);
    if (norm > 0.0) {
        ga /= norm;
        gb /= norm;
    }
    std::array<double, 8> power{};
    power[static_cast<std::size_t>(ring[a].second)] = ga * ga;
    power[static_cast<std::size_t>(ring[b].second)] = gb * gb;
    std::array<float, 8> out{};
    for (const auto& [azimuth, channel] : ring) {
        (void)azimuth;
        const auto c = static_cast<std::size_t>(channel);
        out[c] = snap(std::sqrt(f * power[c] + (1.0 - f) / static_cast<double>(N)));
    }
    return out;
}

} // namespace

std::array<float, 8> surroundGainsFor(int outputs, double degrees, double focus) {
    return outputs >= 8 ? ringGains(kRing71, degrees, focus) : ringGains(kRing, degrees, focus);
}

std::array<float, 6> surroundGains(double degrees, double focus) {
    const std::array<float, 8> g = ringGains(kRing, degrees, focus);
    return {g[0], g[1], g[2], g[3], g[4], g[5]};
}

ChannelMatrix surroundPanMatrix(int inputs, double x, double y, double lfeGain) {
    const int n = std::clamp(inputs, 1, 2);
    ChannelMatrix out(6, n);
    surroundPanGains(n, x, y, lfeGain, out.values.data());
    return out;
}

void surroundPanGains(int inputs, double x, double y, double lfeGain, float* out) {
    surroundPanGains(6, inputs, x, y, lfeGain, out);
}

void surroundPanGains(int outputs, int inputs, double x, double y, double lfeGain, float* out) {
    const int n = std::clamp(inputs, 1, 2);
    const int rows = outputs >= 8 ? 8 : 6;
    const auto at = [&](int r, int c) -> float& {
        return out[static_cast<std::size_t>(r) * static_cast<std::size_t>(n) + static_cast<std::size_t>(c)];
    };
    const double cx = std::clamp(x, -1.0, 1.0);
    const double cy = std::clamp(y, -1.0, 1.0);
    const double distance = std::min(1.0, std::hypot(cx, cy));
    const double theta = distance > 1e-9 ? std::atan2(cx, cy) * 180.0 / std::numbers::pi : 0.0;
    for (int c = 0; c < n; ++c) {
        const double offset = n == 2 ? (c == 0 ? -30.0 : 30.0) : 0.0;
        const std::array<float, 8> g = surroundGainsFor(rows, theta + offset, distance);
        for (int r = 0; r < rows; ++r) {
            at(r, c) = g[static_cast<std::size_t>(r)];
        }
        at(kLfe, c) = static_cast<float>(lfeGain / n);
    }
}

} // namespace wabcut::playback
