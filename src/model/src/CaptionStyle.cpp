// Caption styles (CaptionStyle.h): enum names, validation and the built-in presets.

#include <wabcut/model/CaptionStyle.h>

#include <wabcut/model/CaptionText.h>
#include <wabcut/model/Graphics.h>

#include <array>
#include <cmath>
#include <utility>

namespace wabcut {

namespace {

template <class E, std::size_t N>
std::optional<E> fromName(const std::array<std::pair<E, std::string_view>, N>& names, std::string_view text) {
    for (const auto& [value, name] : names) {
        if (name == text) {
            return value;
        }
    }
    return std::nullopt;
}

template <class E, std::size_t N>
std::string_view nameOf(const std::array<std::pair<E, std::string_view>, N>& names, E value) {
    for (const auto& [v, name] : names) {
        if (v == value) {
            return name;
        }
    }
    return names.front().second;
}

constexpr std::array<std::pair<CaptionPlacement, std::string_view>, 3> kPlacements{
    {{CaptionPlacement::Bottom, "bottom"}, {CaptionPlacement::Top, "top"}, {CaptionPlacement::Custom, "custom"}}};
constexpr std::array<std::pair<CaptionAlign, std::string_view>, 3> kAligns{
    {{CaptionAlign::Left, "left"}, {CaptionAlign::Center, "center"}, {CaptionAlign::Right, "right"}}};
constexpr std::array<std::pair<CaptionAnimation, std::string_view>, 4> kAnimations{
    {{CaptionAnimation::None, "none"},
     {CaptionAnimation::Pop, "pop"},
     {CaptionAnimation::Fade, "fade"},
     {CaptionAnimation::WordByWord, "wordByWord"}}};

bool within(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; }

bool validColor(const CaptionRgb& c) {
    return within(c[0], 0.0, 1.0) && within(c[1], 0.0, 1.0) && within(c[2], 0.0, 1.0);
}

char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool sameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

std::vector<CaptionPreset> makeBuiltins() {
    std::vector<CaptionPreset> out;
    {
        // White text with a thin outline and a soft shadow, no box: for clean footage.
        CaptionStyle s;
        s.box = false;
        s.outlineWidth = 0.06;
        s.shadowOpacity = 0.6;
        s.shadowDistance = 0.06;
        s.shadowSoftness = 0.12;
        out.push_back({"Clean", s});
    }
    {
        // Heavy text in a dark, rounded box: legible over anything.
        CaptionStyle s;
        s.fontWeight = 800;
        s.fontSize = 54.0 / 1080.0;
        s.boxColor = {0.0, 0.0, 0.0};
        s.boxOpacity = 0.85;
        s.boxPadding = 0.4;
        s.boxRadius = 0.22;
        out.push_back({"Bold Box", s});
    }
    {
        // Speaker names in their colours, a narrower left-aligned block in a light box.
        CaptionStyle s;
        s.fontWeight = 500;
        s.fontSize = 44.0 / 1080.0;
        s.alignment = CaptionAlign::Left;
        s.maxWidth = 0.7;
        s.boxOpacity = 0.6;
        s.showSpeakers = true;
        out.push_back({"Podcast", s});
    }
    {
        // Vertical short-form video: big heavy words with a thick outline, two thirds down, a yellow highlight.
        CaptionStyle s;
        s.fontWeight = 800;
        s.fontSize = 72.0 / 1080.0;
        s.box = false;
        s.outlineWidth = 0.12;
        s.shadowOpacity = 0.5;
        s.placement = CaptionPlacement::Custom;
        s.positionY = ParamValue(0.68);
        s.maxWidth = 0.85;
        s.highlight = {1.0, 0.86, 0.0};
        s.animation = CaptionAnimation::Pop;
        out.push_back({"Shorts", s});
    }
    {
        // Thai: a font with Thai glyphs, a lighter weight and taller lines for marks above and below the letters.
        CaptionStyle s;
        s.fontFamily = "Noto Sans Thai";
        s.fontWeight = 500;
        s.fontSize = 52.0 / 1080.0;
        s.lineSpacing = 1.3;
        s.boxPadding = 0.4;
        out.push_back({"Thai-friendly", s});
    }
    return out;
}

} // namespace

std::string_view toString(CaptionPlacement value) { return nameOf(kPlacements, value); }
std::string_view toString(CaptionAlign value) { return nameOf(kAligns, value); }
std::string_view toString(CaptionAnimation value) { return nameOf(kAnimations, value); }
std::optional<CaptionPlacement> captionPlacementFromString(std::string_view text) { return fromName(kPlacements, text); }
std::optional<CaptionAlign> captionAlignFromString(std::string_view text) { return fromName(kAligns, text); }
std::optional<CaptionAnimation> captionAnimationFromString(std::string_view text) { return fromName(kAnimations, text); }

CaptionStyle resolvedCaptionStyle(const CaptionStyle& style, const BrandKit* kit) {
    CaptionStyle out = style;
    if (!kit || style.brand.empty()) {
        return out;
    }
    const auto apply = [&](const std::string& name, CaptionRgb& target) {
        if (const BrandColor* c = name.empty() ? nullptr : kit->color(name)) {
            target = {c->rgba[0], c->rgba[1], c->rgba[2]};
        }
    };
    apply(style.brand.fill, out.fill);
    apply(style.brand.outline, out.outline);
    apply(style.brand.shadow, out.shadowColor);
    apply(style.brand.box, out.boxColor);
    apply(style.brand.highlight, out.highlight);
    if (const BrandFont* f = style.brand.font.empty() ? nullptr : kit->font(style.brand.font)) {
        out.fontFamily = f->family;
        out.fontWeight = f->weight;
    }
    return out;
}

std::optional<std::string> captionStyleProblem(const CaptionStyle& s) {
    for (const std::string* ref : {&s.brand.fill, &s.brand.outline, &s.brand.shadow, &s.brand.box, &s.brand.highlight,
                                   &s.brand.font}) {
        if (ref->size() > kMaxCaptionFontBytes || !isValidCaptionText(*ref) || ref->find('\n') != std::string::npos) {
            return "has a brand reference that is not one line of UTF-8 up to " + std::to_string(kMaxCaptionFontBytes) +
                   " bytes";
        }
    }
    if (s.fontFamily.size() > kMaxCaptionFontBytes || !isValidCaptionText(s.fontFamily) ||
        s.fontFamily.find('\n') != std::string::npos) {
        return "has a font family that is not one line of UTF-8 up to " + std::to_string(kMaxCaptionFontBytes) + " bytes";
    }
    if (s.fontWeight < 100 || s.fontWeight > 900) {
        return "has a font weight outside 100..900";
    }
    const std::pair<double, std::pair<double, double>> ranges[] = {
        {s.fontSize, {0.005, 0.5}},      {s.outlineWidth, {0.0, 0.5}},  {s.shadowOpacity, {0.0, 1.0}},
        {s.shadowDistance, {0.0, 2.0}},  {s.shadowAngle, {-360.0, 360.0}}, {s.shadowSoftness, {0.0, 2.0}},
        {s.boxOpacity, {0.0, 1.0}},      {s.boxPadding, {0.0, 2.0}},    {s.boxRadius, {0.0, 2.0}},
        {s.safeArea, {0.0, 0.4}},        {s.maxWidth, {0.1, 1.0}},      {s.lineSpacing, {0.5, 3.0}},
    };
    for (const auto& [value, range] : ranges) {
        if (!within(value, range.first, range.second)) {
            return "has a size, opacity, angle or spacing outside its range";
        }
    }
    if (!validColor(s.fill) || !validColor(s.outline) || !validColor(s.shadowColor) || !validColor(s.boxColor) ||
        !validColor(s.highlight)) {
        return "has a colour component outside 0..1";
    }
    if (!s.positionX.isValid(0.0, 1.0) || !s.positionY.isValid(0.0, 1.0)) {
        return "has a custom position outside the frame (0..1)";
    }
    return std::nullopt;
}

std::span<const CaptionPreset> builtinCaptionPresets() {
    static const std::vector<CaptionPreset> presets = makeBuiltins();
    return presets;
}

const CaptionPreset* findBuiltinCaptionPreset(std::string_view name) {
    for (const CaptionPreset& p : builtinCaptionPresets()) {
        if (sameName(p.name, name)) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace wabcut
