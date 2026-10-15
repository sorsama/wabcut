#include <wabcut/color/Looks.h>

#include <wabcut/model/MediaLocator.h>

#include <array>
#include <cmath>
#include <functional>
#include <map>

namespace wabcut::color {

namespace {

constexpr std::array<BuiltinLut, 7> kBuiltins{{
    {"teal-orange", "Teal & Orange", false,
     "Cool shadows, warm skin and highlights, a gentle S-curve.", "Wabcut (generated; project licence)"},
    {"warm-film", "Warm Film", false, "Lifted blacks, warm highlights, softer colour - a faded print.",
     "Wabcut (generated; project licence)"},
    {"cool-night", "Cool Night", false, "Darker, blue and less saturated: day for night.",
     "Wabcut (generated; project licence)"},
    {"bleach-bypass", "Bleach Bypass", false, "High contrast, low saturation, the silver kept in the print.",
     "Wabcut (generated; project licence)"},
    {"mono-silver", "Mono Silver", false, "Black and white with a cool silver tone and an S-curve.",
     "Wabcut (generated; project licence)"},
    {"slog3-sgamut3cine-to-rec709", "Sony S-Log3 (S-Gamut3.Cine) to Rec.709", true,
     "For legal-range S-Log3 / S-Gamut3.Cine footage: decodes the log curve, converts the gamut and rolls off "
     "highlights to Rec.709.",
     "Transfer function and primaries from Sony's published \"Technical Summary for S-Gamut3.Cine/S-Log3 and "
     "S-Gamut3/S-Log3\" (math only, no Sony LUT); rendering by Wabcut (project licence)."},
    {"vlog-vgamut-to-rec709", "Panasonic V-Log (V-Gamut) to Rec.709", true,
     "For legal-range V-Log / V-Gamut footage: decodes the log curve, converts the gamut and rolls off highlights to "
     "Rec.709.",
     "Transfer function and primaries from Panasonic's published \"V-Log/V-Gamut Reference Manual\" (math only, no "
     "Panasonic LUT); rendering by Wabcut (project licence)."},
}};

using LookFn = std::function<Rgb(const Rgb&)>;

Lut generate(int size, const LookFn& fn, std::string title) {
    Lut lut;
    lut.title = std::move(title);
    lut.size3d = size;
    lut.table3d.resize(static_cast<std::size_t>(size) * size * size * 3);
    std::size_t k = 0;
    for (int b = 0; b < size; ++b) {
        for (int g = 0; g < size; ++g) {
            for (int r = 0; r < size; ++r) {
                const Rgb in{static_cast<double>(r) / (size - 1), static_cast<double>(g) / (size - 1),
                             static_cast<double>(b) / (size - 1)};
                const Rgb out = fn(in);
                for (std::size_t c = 0; c < 3; ++c) {
                    lut.table3d[k++] = static_cast<float>(std::clamp(out[c], -0.5, 1.5));
                }
            }
        }
    }
    return lut;
}

/// A smooth S-curve about 0.5 of strength `k` (0 = identity), monotone.
double sCurve(double x, double k) {
    const double t = std::clamp(x, 0.0, 1.0);
    const double s = t * t * (3.0 - 2.0 * t);
    return x + k * (s - t);
}

Rgb withSaturation(const Rgb& c, double s) {
    const double y = luma(c);
    return {y + (c[0] - y) * s, y + (c[1] - y) * s, y + (c[2] - y) * s};
}

Rgb tealOrange(const Rgb& in) {
    Rgb c{sCurve(in[0], 0.35), sCurve(in[1], 0.35), sCurve(in[2], 0.35)};
    const double y = std::clamp(luma(c), 0.0, 1.0);
    // Teal (190 deg) in the shadows, orange (30 deg) in the highlights.
    c = c + hueChroma(190.0 / 360.0) * (0.09 * (1.0 - y) * (1.0 - y)) + hueChroma(30.0 / 360.0) * (0.07 * y * y);
    // A little extra saturation for warm hues (skin, sunsets).
    const Hsv hsv = toHsv(in);
    const double warm = std::exp(-std::pow((hsv.h - 0.07) / 0.06, 2.0));
    return withSaturation(c, 1.0 + 0.12 * warm);
}

Rgb warmFilm(const Rgb& in) {
    Rgb c{sCurve(in[0], 0.2), sCurve(in[1], 0.2), sCurve(in[2], 0.2)};
    for (double& v : c) {
        v = 0.045 + v * (0.97 - 0.045); // lifted blacks, softened whites
    }
    const double y = std::clamp(luma(c), 0.0, 1.0);
    c = c + hueChroma(38.0 / 360.0) * (0.06 * y);
    return withSaturation(c, 0.88);
}

Rgb coolNight(const Rgb& in) {
    Rgb lin = decode709(in);
    lin = lin * 0.55;
    Rgb c = encode709(lin);
    c = withSaturation(c, 0.6);
    const double y = std::clamp(luma(c), 0.0, 1.0);
    c = c + hueChroma(220.0 / 360.0) * (0.10 * (1.0 - 0.5 * y));
    return {sCurve(c[0], 0.25), sCurve(c[1], 0.25), sCurve(c[2], 0.25)};
}

Rgb bleachBypass(const Rgb& in) {
    const double y = luma(in);
    Rgb c{};
    for (std::size_t i = 0; i < 3; ++i) {
        // Overlay of the image with its luma, mixed in.
        const double a = in[i];
        const double overlay = y < 0.5 ? 2.0 * a * y : 1.0 - 2.0 * (1.0 - a) * (1.0 - y);
        c[i] = a + (overlay - a) * 0.6;
    }
    c = withSaturation(c, 0.55);
    return {sCurve(c[0], 0.3), sCurve(c[1], 0.3), sCurve(c[2], 0.3)};
}

Rgb monoSilver(const Rgb& in) {
    const double y = sCurve(luma(in), 0.4);
    const Rgb grey{y, y, y};
    return grey + hueChroma(205.0 / 360.0) * (0.025 * (1.0 - std::abs(2.0 * y - 1.0)));
}

/// RGB -> XYZ of xy primaries with a D65 white.
Mat3 rgbToXyz(double rx, double ry, double gx, double gy, double bx, double by) {
    const double wx = 0.3127, wy = 0.3290;
    const auto xyz = [](double x, double y) { return Rgb{x / y, 1.0, (1.0 - x - y) / y}; };
    const Rgb r = xyz(rx, ry), g = xyz(gx, gy), b = xyz(bx, by), w = xyz(wx, wy);
    const Mat3 m{r[0], g[0], b[0], r[1], g[1], b[1], r[2], g[2], b[2]};
    const Rgb s = mul(inverse(m), w);
    return {m[0] * s[0], m[1] * s[1], m[2] * s[2], m[3] * s[0], m[4] * s[1], m[5] * s[2], m[6] * s[0], m[7] * s[1], m[8] * s[2]};
}

Rgb logInput(const Rgb& v, double (*decode)(double), const Mat3& toRec709) {
    Rgb lin{};
    for (std::size_t i = 0; i < 3; ++i) {
        const double code = (64.0 + 876.0 * v[i]) / 1023.0; // legal-range file -> full 10-bit code value
        lin[i] = decode(code);
    }
    lin = mul(toRec709, lin);
    for (double& x : lin) {
        x = highlightRolloff(std::max(x, 0.0));
    }
    return encode709(lin);
}

} // namespace

std::span<const BuiltinLut> builtinLuts() { return kBuiltins; }

double highlightRolloff(double linear) {
    constexpr double knee = 0.18;
    if (linear <= knee) {
        return linear;
    }
    return knee + (1.0 - knee) * (1.0 - std::exp(-(linear - knee) / (1.0 - knee)));
}

double slog3ToLinear(double code) {
    const double cv = code * 1023.0;
    return cv >= 171.2102946929 ? std::pow(10.0, (cv - 420.0) / 261.5) * (0.18 + 0.01) - 0.01
                                : (cv - 95.0) * 0.01125000 / (171.2102946929 - 95.0);
}

double linearToSlog3(double linear) {
    return linear >= 0.01125000 ? (420.0 + std::log10((linear + 0.01) / (0.18 + 0.01)) * 261.5) / 1023.0
                                : (linear * (171.2102946929 - 95.0) / 0.01125000 + 95.0) / 1023.0;
}

double vlogToLinear(double code) {
    constexpr double b = 0.00873, c = 0.241514, d = 0.598206;
    return code < 0.181 ? (code - 0.125) / 5.6 : std::pow(10.0, (code - d) / c) - b;
}

double linearToVlog(double linear) {
    constexpr double b = 0.00873, c = 0.241514, d = 0.598206;
    return linear < 0.01 ? 5.6 * linear + 0.125 : c * std::log10(linear + b) + d;
}

Mat3 gamutToRec709(double rx, double ry, double gx, double gy, double bx, double by) {
    const Mat3 rec709 = rgbToXyz(0.64, 0.33, 0.30, 0.60, 0.15, 0.06);
    return multiply(inverse(rec709), rgbToXyz(rx, ry, gx, gy, bx, by));
}

std::shared_ptr<const Lut> builtinLut(std::string_view name) {
    if (name.starts_with(kBuiltinPrefix)) {
        name.remove_prefix(kBuiltinPrefix.size());
    }
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const Lut>, std::less<>> cache;
    std::lock_guard lock(mutex);
    if (const auto it = cache.find(name); it != cache.end()) {
        return it->second;
    }
    const auto found = std::find_if(kBuiltins.begin(), kBuiltins.end(), [&](const BuiltinLut& b) { return b.name == name; });
    if (found == kBuiltins.end()) {
        return nullptr;
    }
    LookFn fn;
    int size = 33;
    if (name == "teal-orange") {
        fn = tealOrange;
    } else if (name == "warm-film") {
        fn = warmFilm;
    } else if (name == "cool-night") {
        fn = coolNight;
    } else if (name == "bleach-bypass") {
        fn = bleachBypass;
    } else if (name == "mono-silver") {
        fn = monoSilver;
    } else if (name == "slog3-sgamut3cine-to-rec709") {
        const Mat3 m = gamutToRec709(0.766, 0.275, 0.225, 0.800, 0.089, -0.087);
        fn = [m](const Rgb& v) { return logInput(v, slog3ToLinear, m); };
        size = 65;
    } else if (name == "vlog-vgamut-to-rec709") {
        const Mat3 m = gamutToRec709(0.730, 0.280, 0.165, 0.840, 0.100, -0.030);
        fn = [m](const Rgb& v) { return logInput(v, vlogToLinear, m); };
        size = 65;
    }
    auto lut = std::make_shared<const Lut>(generate(size, fn, std::string(found->displayName)));
    cache.emplace(std::string(name), lut);
    return lut;
}

LutLibrary& LutLibrary::shared() {
    static LutLibrary library;
    return library;
}

std::shared_ptr<const Lut> LutLibrary::load(const std::filesystem::path& file, std::string* error) {
    std::error_code ec;
    const auto size = static_cast<std::int64_t>(std::filesystem::file_size(file, ec));
    if (ec) {
        if (error) {
            *error = "the LUT file is missing";
        }
        return nullptr;
    }
    const auto mtime = std::filesystem::last_write_time(file, ec).time_since_epoch().count();
    const std::string key = pathToUtf8(file.lexically_normal());
    {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_entries.find(key); it != m_entries.end() && it->second.size == size && it->second.mtime == mtime) {
            if (error) {
                *error = it->second.error;
            }
            return it->second.lut;
        }
    }
    Entry entry;
    entry.size = size;
    entry.mtime = static_cast<std::int64_t>(mtime);
    if (auto lut = loadCube(file)) {
        entry.lut = std::make_shared<const Lut>(std::move(*lut));
    } else {
        entry.error = lut.error().message;
    }
    if (error) {
        *error = entry.error;
    }
    std::lock_guard lock(m_mutex);
    m_entries[key] = entry;
    return entry.lut;
}

LutResolver LutLibrary::resolver(const ProjectState& state) {
    // Assets are few; the resolver keeps its own copy of their paths, so it outlives `state`.
    struct Known {
        std::uint64_t id;
        std::string path;
        bool offline;
    };
    std::vector<Known> known;
    for (const ProjectAsset& a : state.assets) {
        if (a.kind == kAssetKindLut) {
            known.push_back({a.id.value, a.locator.path, a.locator.offline});
        }
    }
    return [this, known = std::move(known)](std::string_view reference) -> std::shared_ptr<const Lut> {
        if (reference.starts_with(kBuiltinPrefix)) {
            return builtinLut(reference);
        }
        const AssetId id = assetOfReference(reference);
        for (const Known& k : known) {
            if (k.id == id.value) {
                if (k.offline || k.path.empty()) {
                    return nullptr;
                }
                // Fast path: the cache, without touching the disk (the evaluator calls this every frame).
                const std::filesystem::path file = pathFromUtf8(k.path).lexically_normal();
                {
                    std::lock_guard lock(m_mutex);
                    if (const auto it = m_entries.find(pathToUtf8(file)); it != m_entries.end() && it->second.lut) {
                        return it->second.lut;
                    }
                }
                return load(file);
            }
        }
        return nullptr;
    };
}

void LutLibrary::clear() {
    std::lock_guard lock(m_mutex);
    m_entries.clear();
}

} // namespace wabcut::color
