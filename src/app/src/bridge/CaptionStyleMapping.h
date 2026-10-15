#pragma once

// The Captions panel's style (CaptionsBridge::style(), a QVariantMap) as the compositor draws it: one mapping for the
// program monitor (PlaybackBridge) and burned-in exports (ExportController), so preview == export. Owner:
// Graphics/playback (I.4). Since C.7 the style is the shown caption track's model CaptionStyle (captionStyleMap in
// CaptionsBridge.h: fontFamily, fontWeight, fontSize as a fraction of the frame height, fill, outline, box, placement,
// safe area, width, spacing, karaoke colour), mapped as docs/internal/c7-caption-handoff.md says. M5m: the renderer has no
// shadow, alignment, custom placement or speaker names yet (custom placement draws at the bottom). The older map
// (font, size in px, color, box, boxColor, position) is still read when there is no model style.

#include <wabcut/render/Caption.h>

#include <QColor>
#include <QSettings>
#include <QString>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <string>

namespace wabcut::app {

inline std::array<double, 3> rgbOf(const QVariant& value, std::array<double, 3> fallback) {
    const QColor c(value.toString());
    if (!c.isValid()) {
        return fallback;
    }
    return {c.redF(), c.greenF(), c.blueF()};
}

/// The platform's caption font when the style names none.
inline std::string defaultCaptionFont() {
#if defined(Q_OS_WIN)
    return "Segoe UI";
#else
    return "";
#endif
}

inline render::CaptionStyle captionStyleFrom(const QVariantMap& style) {
    render::CaptionStyle s;
    if (style.contains(QStringLiteral("fontSize"))) {
        // C.7: the model style (handoff table: sizes are pixels of a 1080-line frame).
        const std::string family = style.value(QStringLiteral("fontFamily")).toString().toStdString();
        s.fontFamily = family.empty() ? defaultCaptionFont() : family;
        s.weight = std::clamp(style.value(QStringLiteral("fontWeight"), 600).toInt(), 100, 900);
        s.sizePx = std::clamp(style.value(QStringLiteral("fontSize")).toDouble(), 0.005, 0.5) * 1080.0;
        s.fill = rgbOf(style.value(QStringLiteral("fill")), s.fill);
        s.strokeWidth = std::clamp(style.value(QStringLiteral("outlineWidth")).toDouble(), 0.0, 0.5) * s.sizePx;
        s.stroke = rgbOf(style.value(QStringLiteral("outline")), s.stroke);
        s.box = style.value(QStringLiteral("box"), true).toBool();
        s.boxColor = rgbOf(style.value(QStringLiteral("boxColor")), s.boxColor);
        s.boxOpacity = std::clamp(style.value(QStringLiteral("boxOpacity"), 0.78).toDouble(), 0.0, 1.0);
        s.position = style.value(QStringLiteral("placement")).toString() == QLatin1String("top") ? render::CaptionPosition::Top
                                                                                               : render::CaptionPosition::Bottom;
        s.safeArea = std::clamp(style.value(QStringLiteral("safeArea"), 0.1).toDouble(), 0.0, 0.4);
        s.maxWidth = std::clamp(style.value(QStringLiteral("maxWidth"), 0.8).toDouble(), 0.1, 1.0);
        s.lineSpacing = std::clamp(style.value(QStringLiteral("lineSpacing"), 1.0).toDouble(), 0.5, 3.0);
        s.highlight = rgbOf(style.value(QStringLiteral("highlight")), s.highlight);
        return s;
    }
    const QString font = style.value(QStringLiteral("font")).toString();
#if defined(Q_OS_WIN)
    s.fontFamily = font == QLatin1String("serif")  ? "Georgia"
                   : font == QLatin1String("mono") ? "Cascadia Mono"
                                                   : "Segoe UI";
#else
    s.fontFamily = font == QLatin1String("serif") ? "serif" : font == QLatin1String("mono") ? "monospace" : "";
#endif
    s.weight = 600;
    s.sizePx = style.value(QStringLiteral("size"), 48.0).toDouble();
    s.fill = rgbOf(style.value(QStringLiteral("color")), {1.0, 1.0, 1.0});
    s.box = style.value(QStringLiteral("box"), true).toBool();
    s.boxColor = rgbOf(style.value(QStringLiteral("boxColor")), s.boxColor);
    s.boxOpacity = 0.78;
    // Without a box an outline keeps them legible over any picture.
    s.strokeWidth = s.box ? 0.0 : std::max(1.5, s.sizePx * 0.05);
    s.position = style.value(QStringLiteral("position")).toString() == QLatin1String("top") ? render::CaptionPosition::Top
                                                                                            : render::CaptionPosition::Bottom;
    return s;
}

/// Karaoke highlighting of drawn captions (the monitor's menu; burned-in exports follow it). QSettings key.
inline constexpr const char* kCaptionKaraokeKey = "captions/karaoke";
inline bool captionKaraoke() {
    return QSettings().value(QLatin1String(kCaptionKaraokeKey), false).toBool();
}

} // namespace wabcut::app
