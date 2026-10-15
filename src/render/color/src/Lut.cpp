#include <wabcut/color/Lut.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <optional>

namespace wabcut::color {

namespace {

Error parseError(int line, const std::string& what) {
    return Error{ErrorCode::ParseError, std::format("line {}: {}", line, what)};
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && isSpace(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && isSpace(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

/// Splits on blanks (at most `max` tokens; more are reported as max + 1).
std::vector<std::string_view> tokens(std::string_view s, std::size_t max) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isSpace(s[i])) {
            ++i;
        }
        if (i >= s.size()) {
            break;
        }
        const std::size_t start = i;
        while (i < s.size() && !isSpace(s[i])) {
            ++i;
        }
        out.push_back(s.substr(start, i - start));
        if (out.size() > max) {
            break;
        }
    }
    return out;
}

std::optional<double> number(std::string_view s) {
    if (!s.empty() && s.front() == '+') {
        s.remove_prefix(1);
    }
    double v = 0.0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || end != s.data() + s.size() || !std::isfinite(v)) {
        return std::nullopt;
    }
    return v;
}

std::optional<int> integer(std::string_view s) {
    int v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || end != s.data() + s.size()) {
        return std::nullopt;
    }
    return v;
}

double lerp(double a, double b, double t) { return a + (b - a) * t; }

/// x mapped from [lo, hi] to [0, 1], clamped (NaN -> 0).
double normalized(double x, double lo, double hi) {
    const double t = (x - lo) / (hi - lo);
    return t >= 0.0 ? std::min(t, 1.0) : 0.0;
}

} // namespace

Rgb Lut::entry1d(int i) const {
    i = std::clamp(i, 0, size1d - 1);
    const std::size_t k = static_cast<std::size_t>(i) * 3;
    return {table1d[k], table1d[k + 1], table1d[k + 2]};
}

Rgb Lut::entry3d(int r, int g, int b) const {
    r = std::clamp(r, 0, size3d - 1);
    g = std::clamp(g, 0, size3d - 1);
    b = std::clamp(b, 0, size3d - 1);
    const std::size_t k = (static_cast<std::size_t>(b) * static_cast<std::size_t>(size3d) * static_cast<std::size_t>(size3d) +
                           static_cast<std::size_t>(g) * static_cast<std::size_t>(size3d) + static_cast<std::size_t>(r)) *
                          3;
    return {table3d[k], table3d[k + 1], table3d[k + 2]};
}

std::expected<Lut, Error> parseCube(std::string_view text) {
    if (text.size() > kMaxCubeBytes) {
        return std::unexpected(Error{ErrorCode::ParseError, "the file is larger than a LUT can be (64 MB)"});
    }
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    Lut lut;
    bool sawTitle = false;
    bool sawDomainMin = false;
    bool sawDomainMax = false;
    bool sawRange1d = false;
    bool sawRange3d = false;
    std::size_t expected1d = 0;
    std::size_t expected3d = 0;
    std::size_t entries = 0; // data lines read
    int lineNumber = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t end = std::min(text.find('\n', pos), text.size());
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNumber;
        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        line = trim(line);
        if (line.empty()) {
            if (end >= text.size()) {
                break;
            }
            continue;
        }
        const char first = line.front();
        const bool isData = (first >= '0' && first <= '9') || first == '-' || first == '+' || first == '.';
        if (!isData) {
            if (entries > 0) {
                return std::unexpected(parseError(lineNumber, "a keyword after the table data"));
            }
            const auto t = tokens(line, 4);
            const std::string_view key = t.front();
            if (key == "TITLE") {
                if (sawTitle) {
                    return std::unexpected(parseError(lineNumber, "TITLE appears twice"));
                }
                sawTitle = true;
                std::string_view rest = trim(line.substr(5));
                if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') {
                    rest = rest.substr(1, rest.size() - 2);
                } else if (!rest.empty() && rest.front() == '"') {
                    return std::unexpected(parseError(lineNumber, "TITLE has no closing quote"));
                }
                lut.title = std::string(rest.substr(0, 256));
            } else if (key == "LUT_1D_SIZE" || key == "LUT_3D_SIZE") {
                const bool is3d = key == "LUT_3D_SIZE";
                if (t.size() != 2) {
                    return std::unexpected(parseError(lineNumber, std::string(key) + " needs one number"));
                }
                if ((is3d ? lut.size3d : lut.size1d) != 0) {
                    return std::unexpected(parseError(lineNumber, std::string(key) + " appears twice"));
                }
                const auto n = integer(t[1]);
                if (!n || *n < 2) {
                    return std::unexpected(parseError(lineNumber, std::string(key) + " must be a whole number of at least 2"));
                }
                const int limit = is3d ? kMaxLut3dSize : kMaxLut1dSize;
                if (*n > limit) {
                    return std::unexpected(parseError(
                        lineNumber, std::format("{} {} is too large: Wabcut reads {} LUTs of up to {} points{}", key, *n,
                                                is3d ? "3D" : "1D", limit, is3d ? " per side" : "")));
                }
                if (is3d) {
                    lut.size3d = *n;
                    expected3d = static_cast<std::size_t>(*n) * static_cast<std::size_t>(*n) * static_cast<std::size_t>(*n);
                } else {
                    lut.size1d = *n;
                    expected1d = static_cast<std::size_t>(*n);
                }
            } else if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX") {
                const bool isMin = key == "DOMAIN_MIN";
                bool& seen = isMin ? sawDomainMin : sawDomainMax;
                if (seen) {
                    return std::unexpected(parseError(lineNumber, std::string(key) + " appears twice"));
                }
                seen = true;
                if (t.size() != 4) {
                    return std::unexpected(parseError(lineNumber, std::string(key) + " needs three numbers"));
                }
                Rgb v{};
                for (std::size_t c = 0; c < 3; ++c) {
                    const auto x = number(t[c + 1]);
                    if (!x) {
                        return std::unexpected(parseError(lineNumber, std::string(key) + " has a value that is not a number"));
                    }
                    v[c] = *x;
                }
                (isMin ? lut.domain1dMin : lut.domain1dMax) = v;
                (isMin ? lut.domain3dMin : lut.domain3dMax) = v;
            } else if (key == "LUT_1D_INPUT_RANGE" || key == "LUT_3D_INPUT_RANGE") {
                const bool is3d = key == "LUT_3D_INPUT_RANGE";
                bool& seen = is3d ? sawRange3d : sawRange1d;
                if (seen) {
                    return std::unexpected(parseError(lineNumber, std::string(key) + " appears twice"));
                }
                seen = true;
                if (t.size() != 3) {
                    return std::unexpected(parseError(lineNumber, std::string(key) + " needs two numbers"));
                }
                const auto lo = number(t[1]);
                const auto hi = number(t[2]);
                if (!lo || !hi) {
                    return std::unexpected(parseError(lineNumber, std::string(key) + " has a value that is not a number"));
                }
                const Rgb low{*lo, *lo, *lo};
                const Rgb high{*hi, *hi, *hi};
                (is3d ? lut.domain3dMin : lut.domain1dMin) = low;
                (is3d ? lut.domain3dMax : lut.domain1dMax) = high;
            }
            // Other keywords (LUT_IN_VIDEO_RANGE, tool-specific ones) are skipped.
            if (end >= text.size()) {
                break;
            }
            continue;
        }
        // A data line.
        if (expected1d + expected3d == 0) {
            return std::unexpected(parseError(lineNumber, "table data before LUT_1D_SIZE or LUT_3D_SIZE"));
        }
        if (entries >= expected1d + expected3d) {
            return std::unexpected(parseError(lineNumber, std::format("more data lines than the {} the sizes call for",
                                                                      expected1d + expected3d)));
        }
        const auto t = tokens(line, 3);
        if (t.size() != 3) {
            return std::unexpected(parseError(lineNumber, "a data line needs exactly three numbers"));
        }
        if (entries == 0) {
            lut.table1d.reserve(expected1d * 3);
            lut.table3d.reserve(expected3d * 3);
        }
        std::vector<float>& table = entries < expected1d ? lut.table1d : lut.table3d;
        for (const std::string_view token : t) {
            const auto x = number(token);
            // The table holds floats: a value beyond float's range would become infinity there (and on the GPU), and
            // writeCube could not write it back (C.10 fuzzing).
            if (!x || !std::isfinite(static_cast<float>(*x))) {
                return std::unexpected(parseError(lineNumber, "\"" + std::string(token.substr(0, 32)) + "\" is not a finite number"));
            }
            table.push_back(static_cast<float>(*x));
        }
        ++entries;
        if (end >= text.size()) {
            break;
        }
    }
    if (expected1d + expected3d == 0) {
        return std::unexpected(Error{ErrorCode::ParseError, "no LUT_1D_SIZE or LUT_3D_SIZE: this is not a .cube LUT"});
    }
    if (entries != expected1d + expected3d) {
        return std::unexpected(Error{ErrorCode::ParseError, std::format("the table has {} data lines; its sizes call for {}",
                                                                        entries, expected1d + expected3d)});
    }
    for (int c = 0; c < 3; ++c) {
        const auto k = static_cast<std::size_t>(c);
        if (!(lut.domain1dMin[k] < lut.domain1dMax[k]) || !(lut.domain3dMin[k] < lut.domain3dMax[k])) {
            return std::unexpected(Error{ErrorCode::ParseError, "the input domain's minimum must be below its maximum"});
        }
    }
    return lut;
}

std::expected<Lut, Error> loadCube(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot open the LUT file"});
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > kMaxCubeBytes) {
        return std::unexpected(Error{ErrorCode::ParseError, "the file is larger than a LUT can be (64 MB)"});
    }
    in.seekg(0, std::ios::beg);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (size > 0 && !in.read(text.data(), size)) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot read the LUT file"});
    }
    return parseCube(text);
}

std::string writeCube(const Lut& lut) {
    std::string out;
    const auto line = [&](const std::string& s) {
        out += s;
        out += '\n';
    };
    if (!lut.title.empty()) {
        line(std::format("TITLE \"{}\"", lut.title));
    }
    const auto rgb = [](const Rgb& v) { return std::format("{:.9g} {:.9g} {:.9g}", v[0], v[1], v[2]); };
    if (lut.has1d() && lut.has3d()) {
        line(std::format("LUT_1D_INPUT_RANGE {:.9g} {:.9g}", lut.domain1dMin[0], lut.domain1dMax[0]));
        line(std::format("LUT_3D_INPUT_RANGE {:.9g} {:.9g}", lut.domain3dMin[0], lut.domain3dMax[0]));
    } else {
        const Rgb& lo = lut.has3d() ? lut.domain3dMin : lut.domain1dMin;
        const Rgb& hi = lut.has3d() ? lut.domain3dMax : lut.domain1dMax;
        if (lo != Rgb{0, 0, 0}) {
            line("DOMAIN_MIN " + rgb(lo));
        }
        if (hi != Rgb{1, 1, 1}) {
            line("DOMAIN_MAX " + rgb(hi));
        }
    }
    if (lut.has1d()) {
        line(std::format("LUT_1D_SIZE {}", lut.size1d));
    }
    if (lut.has3d()) {
        line(std::format("LUT_3D_SIZE {}", lut.size3d));
    }
    const auto data = [&](const std::vector<float>& table) {
        for (std::size_t i = 0; i + 2 < table.size(); i += 3) {
            line(std::format("{:.9g} {:.9g} {:.9g}", table[i], table[i + 1], table[i + 2]));
        }
    };
    data(lut.table1d);
    data(lut.table3d);
    return out;
}

Rgb apply1d(const Lut& lut, const Rgb& in) {
    if (!lut.has1d()) {
        return in;
    }
    Rgb out{};
    const double last = lut.size1d - 1;
    for (std::size_t c = 0; c < 3; ++c) {
        const double x = normalized(in[c], lut.domain1dMin[c], lut.domain1dMax[c]) * last;
        const int i0 = std::min(static_cast<int>(std::floor(x)), lut.size1d - 2);
        const double t = x - i0;
        const std::size_t k = static_cast<std::size_t>(i0) * 3 + c;
        out[c] = lerp(lut.table1d[k], lut.table1d[k + 3], t);
    }
    return out;
}

Rgb apply3d(const Lut& lut, const Rgb& in, LutInterpolation interpolation) {
    if (!lut.has3d()) {
        return in;
    }
    const double last = lut.size3d - 1;
    std::array<int, 3> i0{};
    Rgb f{};
    for (std::size_t c = 0; c < 3; ++c) {
        const double x = normalized(in[c], lut.domain3dMin[c], lut.domain3dMax[c]) * last;
        i0[c] = std::min(static_cast<int>(std::floor(x)), lut.size3d - 2);
        f[c] = x - i0[c];
    }
    const auto at = [&](int dr, int dg, int db) { return lut.entry3d(i0[0] + dr, i0[1] + dg, i0[2] + db); };
    Rgb out{};
    if (interpolation == LutInterpolation::Trilinear) {
        const Rgb c000 = at(0, 0, 0), c100 = at(1, 0, 0), c010 = at(0, 1, 0), c110 = at(1, 1, 0);
        const Rgb c001 = at(0, 0, 1), c101 = at(1, 0, 1), c011 = at(0, 1, 1), c111 = at(1, 1, 1);
        for (std::size_t c = 0; c < 3; ++c) {
            const double x00 = lerp(c000[c], c100[c], f[0]);
            const double x10 = lerp(c010[c], c110[c], f[0]);
            const double x01 = lerp(c001[c], c101[c], f[0]);
            const double x11 = lerp(c011[c], c111[c], f[0]);
            out[c] = lerp(lerp(x00, x10, f[1]), lerp(x01, x11, f[1]), f[2]);
        }
        return out;
    }
    // Tetrahedral: the cube is split into six tetrahedra along its grey diagonal; the fractions' order picks one.
    const double fr = f[0], fg = f[1], fb = f[2];
    const Rgb c000 = at(0, 0, 0), c111 = at(1, 1, 1);
    Rgb a{}, b{};
    double wa = 0, wb = 0, w0 = 0, w1 = 0;
    if (fr > fg) {
        if (fg > fb) { // r > g > b
            a = at(1, 0, 0), b = at(1, 1, 0);
            w0 = 1 - fr, wa = fr - fg, wb = fg - fb, w1 = fb;
        } else if (fr > fb) { // r > b >= g
            a = at(1, 0, 0), b = at(1, 0, 1);
            w0 = 1 - fr, wa = fr - fb, wb = fb - fg, w1 = fg;
        } else { // b >= r > g
            a = at(0, 0, 1), b = at(1, 0, 1);
            w0 = 1 - fb, wa = fb - fr, wb = fr - fg, w1 = fg;
        }
    } else {
        if (fb > fg) { // b > g >= r
            a = at(0, 0, 1), b = at(0, 1, 1);
            w0 = 1 - fb, wa = fb - fg, wb = fg - fr, w1 = fr;
        } else if (fb > fr) { // g >= b > r
            a = at(0, 1, 0), b = at(0, 1, 1);
            w0 = 1 - fg, wa = fg - fb, wb = fb - fr, w1 = fr;
        } else { // g >= r >= b
            a = at(0, 1, 0), b = at(1, 1, 0);
            w0 = 1 - fg, wa = fg - fr, wb = fr - fb, w1 = fb;
        }
    }
    for (std::size_t c = 0; c < 3; ++c) {
        out[c] = w0 * c000[c] + wa * a[c] + wb * b[c] + w1 * c111[c];
    }
    return out;
}

Rgb applyLut(const Lut& lut, const Rgb& in, LutInterpolation interpolation) {
    return apply3d(lut, apply1d(lut, in), interpolation);
}

Lut identityLut3d(int size) {
    Lut lut;
    lut.size3d = std::clamp(size, 2, kMaxLut3dSize);
    const int n = lut.size3d;
    lut.table3d.resize(static_cast<std::size_t>(n) * n * n * 3);
    std::size_t k = 0;
    for (int b = 0; b < n; ++b) {
        for (int g = 0; g < n; ++g) {
            for (int r = 0; r < n; ++r) {
                lut.table3d[k++] = static_cast<float>(r) / static_cast<float>(n - 1);
                lut.table3d[k++] = static_cast<float>(g) / static_cast<float>(n - 1);
                lut.table3d[k++] = static_cast<float>(b) / static_cast<float>(n - 1);
            }
        }
    }
    return lut;
}

std::vector<float> resampled1d(const Lut& lut, int entries) {
    std::vector<float> out;
    if (!lut.has1d() || entries < 2) {
        return out;
    }
    if (entries == lut.size1d) {
        return lut.table1d;
    }
    out.resize(static_cast<std::size_t>(entries) * 3);
    for (int i = 0; i < entries; ++i) {
        const double x = static_cast<double>(i) / (entries - 1) * (lut.size1d - 1);
        const int i0 = std::min(static_cast<int>(std::floor(x)), lut.size1d - 2);
        const double t = x - i0;
        for (std::size_t c = 0; c < 3; ++c) {
            const std::size_t k = static_cast<std::size_t>(i0) * 3 + c;
            out[static_cast<std::size_t>(i) * 3 + c] = static_cast<float>(lerp(lut.table1d[k], lut.table1d[k + 3], t));
        }
    }
    return out;
}

} // namespace wabcut::color
