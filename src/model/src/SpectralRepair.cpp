#include <wabcut/model/SpectralRepair.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <locale>
#include <sstream>

namespace wabcut {

namespace {

constexpr double kMaxSeconds = 1.0e6;

/// A number as text with at most `decimals` decimals and no trailing zeros ("C" locale, whatever the process locale).
std::string num(double v, int decimals) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(decimals) << v;
    std::string s = out.str();
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') {
            s.pop_back();
        }
        if (!s.empty() && s.back() == '.') {
            s.pop_back();
        }
    }
    if (s == "-0" || s.empty()) {
        s = "0";
    }
    return s;
}

bool readNumber(std::string_view token, double& value) {
    if (token.empty()) {
        return false;
    }
    std::istringstream in{std::string(token)};
    in.imbue(std::locale::classic());
    double v = 0.0;
    in >> v;
    if (in.fail() || !in.eof() || !std::isfinite(v)) {
        return false;
    }
    value = v;
    return true;
}

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> out;
    std::size_t at = 0;
    while (at <= text.size()) {
        std::size_t next = text.find(separator, at);
        if (next == std::string_view::npos) {
            next = text.size();
        }
        if (next > at) {
            out.push_back(text.substr(at, next - at));
        }
        at = next + 1;
    }
    return out;
}

char kindChar(SpectralKind k) {
    switch (k) {
    case SpectralKind::Attenuate: return 'a';
    case SpectralKind::Heal: return 'h';
    case SpectralKind::NoiseFloor: return 'n';
    case SpectralKind::Tone: return 't';
    }
    return 'a';
}

char shapeChar(SpectralShape s) {
    switch (s) {
    case SpectralShape::Rectangle: return 'r';
    case SpectralShape::Lasso: return 'l';
    case SpectralShape::Brush: return 'b';
    }
    return 'r';
}

bool finitePath(const std::vector<SpectralPoint>& path) {
    return std::ranges::all_of(path, [](const SpectralPoint& p) {
        return std::isfinite(p.time) && std::isfinite(p.hz) && p.time >= -kMaxSeconds && p.time <= kMaxSeconds &&
               p.hz >= kMinSpectralHz && p.hz <= kMaxSpectralHz;
    });
}

} // namespace

std::string_view toString(SpectralKind kind) {
    switch (kind) {
    case SpectralKind::Attenuate: return "attenuate";
    case SpectralKind::Heal: return "heal";
    case SpectralKind::NoiseFloor: return "noiseFloor";
    case SpectralKind::Tone: return "tone";
    }
    return "attenuate";
}

std::optional<SpectralKind> spectralKindFromString(std::string_view text) {
    for (const SpectralKind k : {SpectralKind::Attenuate, SpectralKind::Heal, SpectralKind::NoiseFloor, SpectralKind::Tone}) {
        if (toString(k) == text) {
            return k;
        }
    }
    return std::nullopt;
}

std::string_view toString(SpectralShape shape) {
    switch (shape) {
    case SpectralShape::Rectangle: return "rectangle";
    case SpectralShape::Lasso: return "lasso";
    case SpectralShape::Brush: return "brush";
    }
    return "rectangle";
}

std::optional<SpectralShape> spectralShapeFromString(std::string_view text) {
    for (const SpectralShape s : {SpectralShape::Rectangle, SpectralShape::Lasso, SpectralShape::Brush}) {
        if (toString(s) == text) {
            return s;
        }
    }
    return std::nullopt;
}

std::optional<std::string> spectralEditProblem(const SpectralEdit& e) {
    const auto finite = [](double v) { return std::isfinite(v); };
    if (!finite(e.gainDb) || !finite(e.amount) || !finite(e.start) || !finite(e.end) || !finite(e.low) || !finite(e.high) ||
        !finite(e.radiusSeconds) || !finite(e.radiusOctaves) || !finite(e.widthHz)) {
        return "a number of the edit is not finite";
    }
    if (e.gainDb < kMinSpectralGainDb || e.gainDb > 0.0) {
        return "the level change is from " + num(kMinSpectralGainDb, 0) + " to 0 dB";
    }
    if (e.amount <= 0.0 || e.amount > 100.0) {
        return "the amount is from 1 to 100 percent";
    }
    if (e.path.size() > kMaxSpectralPoints) {
        return "the edit has more than " + std::to_string(kMaxSpectralPoints) + " points";
    }
    if (!finitePath(e.path)) {
        return "a point of the edit is outside the time and frequency range";
    }
    if (e.kind == SpectralKind::Tone) {
        if (e.path.empty()) {
            return "a tone needs the frequency to remove (a trace of at least one point)";
        }
        if (!(e.start < e.end) || e.start < -kMaxSeconds || e.end > kMaxSeconds) {
            return "the tone's time range must end after it starts";
        }
        if (e.harmonics < 1 || e.harmonics > 16) {
            return "a tone has from 1 to 16 harmonics";
        }
        if (e.widthHz < 1.0 || e.widthHz > 500.0) {
            return "the tone's width is from 1 to 500 Hz";
        }
        return std::nullopt;
    }
    switch (e.shape) {
    case SpectralShape::Rectangle:
        if (!(e.start < e.end) || e.start < -kMaxSeconds || e.end > kMaxSeconds) {
            return "the region must end after it starts";
        }
        if (!(e.low < e.high) || e.low < kMinSpectralHz || e.high > kMaxSpectralHz) {
            return "the region's top frequency must be above its bottom one (1 Hz to 192 kHz)";
        }
        break;
    case SpectralShape::Lasso:
        if (e.path.size() < 3) {
            return "a lasso has at least three points";
        }
        break;
    case SpectralShape::Brush:
        if (e.path.empty()) {
            return "a brush stroke has at least one point";
        }
        if (e.radiusSeconds <= 0.0 || e.radiusSeconds > 60.0 || e.radiusOctaves <= 0.0 || e.radiusOctaves > 10.0) {
            return "the brush radius is out of range";
        }
        break;
    }
    return std::nullopt;
}

SpectralEdit clampedSpectralEdit(SpectralEdit e) {
    const auto clean = [](double v, double fallback) { return std::isfinite(v) ? v : fallback; };
    e.gainDb = std::clamp(clean(e.gainDb, -24.0), kMinSpectralGainDb, 0.0);
    e.amount = std::clamp(clean(e.amount, 100.0), 1.0, 100.0);
    e.start = std::clamp(clean(e.start, 0.0), -kMaxSeconds, kMaxSeconds);
    e.end = std::clamp(clean(e.end, 0.0), -kMaxSeconds, kMaxSeconds);
    if (e.end < e.start) {
        std::swap(e.start, e.end);
    }
    if (e.end - e.start < 1.0e-3) {
        e.end = e.start + 1.0e-3;
    }
    e.low = std::clamp(clean(e.low, 100.0), kMinSpectralHz, kMaxSpectralHz);
    e.high = std::clamp(clean(e.high, 1000.0), kMinSpectralHz, kMaxSpectralHz);
    if (e.high < e.low) {
        std::swap(e.low, e.high);
    }
    if (e.high - e.low < 1.0) {
        e.high = std::min(e.low + 1.0, kMaxSpectralHz);
        if (e.high - e.low < 1.0) {
            e.low = e.high - 1.0;
        }
    }
    e.radiusSeconds = std::clamp(clean(e.radiusSeconds, 0.05), 1.0e-3, 60.0);
    e.radiusOctaves = std::clamp(clean(e.radiusOctaves, 0.25), 1.0e-2, 10.0);
    e.harmonics = std::clamp(e.harmonics, 1, 16);
    e.widthHz = std::clamp(clean(e.widthHz, 12.0), 1.0, 500.0);
    if (e.path.size() > kMaxSpectralPoints) {
        e.path.resize(kMaxSpectralPoints);
    }
    for (SpectralPoint& p : e.path) {
        p.time = std::clamp(clean(p.time, 0.0), -kMaxSeconds, kMaxSeconds);
        p.hz = std::clamp(clean(p.hz, 1000.0), kMinSpectralHz, kMaxSpectralHz);
    }
    return e;
}

SpectralBounds spectralBounds(const SpectralEdit& e) {
    SpectralBounds b;
    if (e.kind == SpectralKind::Tone) {
        b.start = e.start;
        b.end = e.end;
        double lo = kMaxSpectralHz;
        double hi = kMinSpectralHz;
        for (const SpectralPoint& p : e.path) {
            lo = std::min(lo, p.hz);
            hi = std::max(hi, p.hz);
        }
        b.low = std::max(kMinSpectralHz, lo - e.widthHz);
        b.high = std::min(kMaxSpectralHz, hi * e.harmonics + e.widthHz);
        return b;
    }
    if (e.shape == SpectralShape::Rectangle || e.path.empty()) {
        b.start = e.start;
        b.end = e.end;
        b.low = e.low;
        b.high = e.high;
        return b;
    }
    b.start = e.path.front().time;
    b.end = b.start;
    b.low = e.path.front().hz;
    b.high = b.low;
    for (const SpectralPoint& p : e.path) {
        b.start = std::min(b.start, p.time);
        b.end = std::max(b.end, p.time);
        b.low = std::min(b.low, p.hz);
        b.high = std::max(b.high, p.hz);
    }
    if (e.shape == SpectralShape::Brush) {
        b.start -= e.radiusSeconds;
        b.end += e.radiusSeconds;
        b.low = std::max(kMinSpectralHz, b.low * std::exp2(-e.radiusOctaves));
        b.high = std::min(kMaxSpectralHz, b.high * std::exp2(e.radiusOctaves));
    }
    return b;
}

std::string encodeSpectralEdit(const SpectralEdit& edit) {
    SpectralEdit e = edit;
    for (;;) {
        std::string out;
        out += kindChar(e.kind);
        out += shapeChar(e.shape);
        out += e.logFrequency ? 'g' : 'n';
        const auto add = [&out](const std::string& token) {
            out += ' ';
            out += token;
        };
        add(num(e.gainDb, 2));
        add(num(e.amount, 1));
        add(num(e.start, 5));
        add(num(e.end, 5));
        add(num(e.low, 2));
        add(num(e.high, 2));
        add(num(e.radiusSeconds, 4));
        add(num(e.radiusOctaves, 3));
        add(std::to_string(e.harmonics));
        add(num(e.widthHz, 2));
        add(std::to_string(e.path.size()));
        for (const SpectralPoint& p : e.path) {
            add(num(p.time, 4) + ":" + num(p.hz, 2));
        }
        if (out.size() <= kMaxEffectText || e.path.size() <= 3) {
            return out.size() <= kMaxEffectText ? out : out.substr(0, kMaxEffectText);
        }
        // Too long (many points): thin the path by dropping every second interior point, then try again.
        std::vector<SpectralPoint> thinned;
        for (std::size_t i = 0; i < e.path.size(); ++i) {
            if (i == 0 || i + 1 == e.path.size() || i % 2 == 0) {
                thinned.push_back(e.path[i]);
            }
        }
        e.path = std::move(thinned);
    }
}

std::optional<SpectralEdit> decodeSpectralEdit(std::string_view text) {
    const std::vector<std::string_view> t = split(text, ' ');
    if (t.size() < 12 || t[0].size() != 3) {
        return std::nullopt;
    }
    SpectralEdit e;
    switch (t[0][0]) {
    case 'a': e.kind = SpectralKind::Attenuate; break;
    case 'h': e.kind = SpectralKind::Heal; break;
    case 'n': e.kind = SpectralKind::NoiseFloor; break;
    case 't': e.kind = SpectralKind::Tone; break;
    default: return std::nullopt;
    }
    switch (t[0][1]) {
    case 'r': e.shape = SpectralShape::Rectangle; break;
    case 'l': e.shape = SpectralShape::Lasso; break;
    case 'b': e.shape = SpectralShape::Brush; break;
    default: return std::nullopt;
    }
    if (t[0][2] != 'g' && t[0][2] != 'n') {
        return std::nullopt;
    }
    e.logFrequency = t[0][2] == 'g';
    double harmonics = 0.0;
    double count = 0.0;
    if (!readNumber(t[1], e.gainDb) || !readNumber(t[2], e.amount) || !readNumber(t[3], e.start) || !readNumber(t[4], e.end) ||
        !readNumber(t[5], e.low) || !readNumber(t[6], e.high) || !readNumber(t[7], e.radiusSeconds) ||
        !readNumber(t[8], e.radiusOctaves) || !readNumber(t[9], harmonics) || !readNumber(t[10], e.widthHz) ||
        !readNumber(t[11], count)) {
        return std::nullopt;
    }
    e.harmonics = static_cast<int>(harmonics);
    if (count < 0.0 || count > static_cast<double>(kMaxSpectralPoints) || t.size() != 12 + static_cast<std::size_t>(count)) {
        return std::nullopt;
    }
    for (std::size_t i = 12; i < t.size(); ++i) {
        const std::size_t colon = t[i].find(':');
        SpectralPoint p;
        if (colon == std::string_view::npos || !readNumber(t[i].substr(0, colon), p.time) ||
            !readNumber(t[i].substr(colon + 1), p.hz)) {
            return std::nullopt;
        }
        e.path.push_back(p);
    }
    if (spectralEditProblem(e)) {
        return std::nullopt;
    }
    return e;
}

bool isSpectralEditKey(std::string_view key) {
    return key.size() == 3 && key[0] == 'e' && key[1] >= '0' && key[1] <= '9' && key[2] >= '0' && key[2] <= '9';
}

std::vector<EffectData> packSpectralEdits(std::span<const SpectralEdit> edits) {
    std::vector<EffectData> out;
    std::string chunk;
    const auto flush = [&] {
        if (chunk.empty() || out.size() >= 63) {
            return;
        }
        char key[8];
        std::snprintf(key, sizeof key, "e%02d", static_cast<int>(out.size()));
        out.push_back(EffectData{key, chunk});
        chunk.clear();
    };
    std::size_t taken = 0;
    for (const SpectralEdit& edit : edits) {
        if (taken++ >= kMaxSpectralEdits) {
            break;
        }
        const std::string line = encodeSpectralEdit(edit);
        if (!chunk.empty() && chunk.size() + 1 + line.size() > kMaxEffectText) {
            flush();
        }
        if (!chunk.empty()) {
            chunk += '\n';
        }
        chunk += line;
    }
    flush();
    return out;
}

std::vector<SpectralEdit> unpackSpectralEdits(const EffectInstance& instance) {
    std::vector<SpectralEdit> out;
    for (const EffectData& d : instance.data) {
        if (!isSpectralEditKey(d.key) || d.text() == nullptr) {
            continue;
        }
        for (const std::string_view line : split(*d.text(), '\n')) {
            if (out.size() >= kMaxSpectralEdits) {
                return out;
            }
            if (auto e = decodeSpectralEdit(line)) {
                out.push_back(std::move(*e));
            }
        }
    }
    return out;
}

std::string describeSpectralEdit(const SpectralEdit& e) {
    std::string out;
    const SpectralBounds b = spectralBounds(e);
    switch (e.kind) {
    case SpectralKind::Attenuate: out = "Attenuate " + num(e.gainDb, 1) + " dB"; break;
    case SpectralKind::Heal: out = "Heal"; break;
    case SpectralKind::NoiseFloor: out = "Fill with noise floor"; break;
    case SpectralKind::Tone: out = "Remove tone " + num(e.path.empty() ? 0.0 : e.path.front().hz, 1) + " Hz"; break;
    }
    out += ", " + num(b.start, 2) + "-" + num(b.end, 2) + " s";
    if (e.kind != SpectralKind::Tone) {
        out += ", " + num(b.low, 0) + "-" + num(b.high, 0) + " Hz";
    } else if (e.harmonics > 1) {
        out += ", " + std::to_string(e.harmonics) + " harmonics";
    }
    return out;
}

} // namespace wabcut
