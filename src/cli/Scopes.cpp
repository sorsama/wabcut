// wabcut-cli scopes <project.wabcut> --at <timecode|ticks> [--sequence <name|id>] [--clip <id>] [--out <png>]
//                   [--json <file>] [--width <px>] [--zoom 1|2|5]
//
// Renders one frame offscreen with the export renderer and writes its scopes (I.5, docs/color.md "Scopes"): a PNG
// with the picture, the luma waveform with an IRE / nits graticule, the RGB parade, the vectorscope with its 75 % /
// 100 % targets and skin-tone line, and the RGB + luma histogram; and the statistics as JSON (stdout, or --json).

#include "Scopes.h"

#include <wabcut/color/Scopes.h>
#include <wabcut/editor/ColorFrames.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/Timecode.h>

#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <numbers>
#include <string>
#include <string_view>

namespace {

using nlohmann::json;

double rounded(double v, double unit) { return std::round(v / unit) * unit; }

QImage imageOf(const std::vector<std::uint8_t>& rgba, int w, int h) {
    QImage image(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y) {
        std::memcpy(image.scanLine(y), rgba.data() + static_cast<std::size_t>(y) * w * 4, static_cast<std::size_t>(w) * 4);
    }
    return image;
}

/// Horizontal IRE lines (0, 25, 50, 75, 100) with IRE and BT.1886 nits labels over a waveform panel.
void graticule(QPainter& p, const QRect& r, bool labels) {
    p.setPen(QColor(255, 255, 255, 60));
    for (const int ire : {0, 25, 50, 75, 100}) {
        const int y = r.bottom() - static_cast<int>(std::lround(ire / 100.0 * (r.height() - 1)));
        p.drawLine(r.left(), y, r.right(), y);
        if (labels) {
            const double nits = 100.0 * std::pow(ire / 100.0, 2.4);
            p.drawText(r.left() + 4, y - 3, QStringLiteral("%1 IRE  %2 nits").arg(ire).arg(nits, 0, 'f', nits < 10 ? 1 : 0));
        }
    }
}

void vectorGraticule(QPainter& p, const QRect& r, double zoom) {
    const QPointF centre = r.center() + QPointF(0.5, 0.5);
    const double scale = r.width();
    const auto at = [&](double cb, double cr) { return centre + QPointF(cb * zoom * scale, -cr * zoom * scale); };
    // The panel spans chroma -0.5 / zoom .. 0.5 / zoom: the rim and the axes are its edges (as in the app's view).
    p.setPen(QColor(255, 255, 255, 50));
    p.drawEllipse(centre, 0.5 * scale, 0.5 * scale);
    p.drawLine(at(-0.5 / zoom, 0), at(0.5 / zoom, 0));
    p.drawLine(at(0, -0.5 / zoom), at(0, 0.5 / zoom));
    // Skin-tone line at 123 degrees.
    const double a = wabcut::color::kSkinToneAngle * std::numbers::pi / 180.0;
    p.setPen(QColor(255, 200, 150, 120));
    p.drawLine(centre, at(0.5 / zoom * std::cos(a), 0.5 / zoom * std::sin(a)));
    const wabcut::color::Rgb bars[6] = {{1, 0, 0}, {1, 0, 1}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}, {1, 1, 0}};
    const char* names[6] = {"R", "Mg", "B", "Cy", "G", "Yl"};
    for (int i = 0; i < 6; ++i) {
        for (const double amount : {0.75, 1.0}) {
            const auto t = wabcut::color::vectorscopeTarget(bars[i], amount);
            const QPointF c = at(t[0], t[1]);
            if (!QRectF(r).adjusted(-6, -6, 6, 6).contains(c)) {
                continue; // zoomed out of the panel
            }
            p.setPen(amount < 1.0 ? QColor(255, 255, 255, 150) : QColor(255, 255, 255, 70));
            p.drawRect(QRectF(c.x() - 4, c.y() - 4, 8, 8));
            if (amount == 1.0) {
                p.drawText(c + QPointF(6, -6), QString::fromLatin1(names[i]));
            }
        }
    }
}

} // namespace

int runScopes(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: wabcut-cli scopes <project.wabcut> --at <timecode|ticks> [--sequence <name|id>] [--clip <id>]\n"
                             "                        [--out <png>] [--json <file>] [--width <px>] [--zoom 1|2|5]\n");
        return 2;
    }
    const std::filesystem::path projectFile = wabcut::pathFromUtf8(argv[2]);
    std::string at = "0";
    std::string sequenceArg;
    std::string out;
    std::string jsonOut;
    std::uint64_t clip = 0;
    int width = 960;
    double zoom = 1.0;
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--at" && i + 1 < argc) {
            at = argv[++i];
        } else if (arg == "--sequence" && i + 1 < argc) {
            sequenceArg = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            out = argv[++i];
        } else if (arg == "--json" && i + 1 < argc) {
            jsonOut = argv[++i];
        } else if (arg == "--clip" && i + 1 < argc) {
            clip = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--width" && i + 1 < argc) {
            width = std::clamp(std::atoi(argv[++i]), 64, 7680);
        } else if (arg == "--zoom" && i + 1 < argc) {
            zoom = std::atof(argv[++i]);
            if (zoom != 1.0 && zoom != 2.0 && zoom != 5.0) {
                std::fprintf(stderr, "--zoom is 1, 2 or 5\n");
                return 2;
            }
        } else {
            std::fprintf(stderr, "unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    auto project = wabcut::io::loadProject(projectFile);
    if (!project) {
        std::fprintf(stderr, "cannot load %s: %s\n", argv[2], project.error().message.c_str());
        return 2;
    }
    if (project->sequences.empty()) {
        std::fprintf(stderr, "the project has no sequence\n");
        return 2;
    }
    wabcut::SequencePtr sequence = project->sequences.front();
    if (!sequenceArg.empty()) {
        sequence = nullptr;
        for (const wabcut::SequencePtr& s : project->sequences) {
            if (s->name == sequenceArg || std::to_string(s->id.value) == sequenceArg) {
                sequence = s;
            }
        }
        if (!sequence) {
            std::fprintf(stderr, "no sequence named '%s'\n", sequenceArg.c_str());
            return 2;
        }
    }
    wabcut::Ticks time{};
    if (at.find(':') != std::string::npos || at.find(';') != std::string::npos) {
        auto t = wabcut::parseTimecode(at, sequence->frameRate, sequence->dropFrame);
        if (!t) {
            std::fprintf(stderr, "invalid --at '%s': %s\n", at.c_str(), t.error().message.c_str());
            return 2;
        }
        time = *t;
    } else {
        time = wabcut::Ticks{std::strtoll(at.c_str(), nullptr, 10)};
    }
    static int qtArgc = 1;
    static char* qtArgv[] = {argv[0], nullptr};
    std::unique_ptr<QGuiApplication> app;
    if (!QGuiApplication::instance()) {
        app = std::make_unique<QGuiApplication>(qtArgc, qtArgv);
    }
    wabcut::editor::ColorFrameService frames(projectFile);
    wabcut::color::FrameRequest request;
    request.time = time;
    request.solo = wabcut::ClipId{clip};
    request.maxWidth = width;
    auto image = frames.frame(*project, sequence->id, request);
    if (!image) {
        std::fprintf(stderr, "cannot render the frame: %s\n", image.error().message.c_str());
        return 2;
    }
    wabcut::color::ScopeSettings settings;
    settings.vectorZoom = zoom;
    const wabcut::color::ScopeData data = wabcut::color::computeScopes(image->rgba.data(), image->width, image->height, 4, settings);
    const wabcut::color::ScopeStats s = wabcut::color::scopeStats(data);
    const std::string tc = wabcut::ticksToTimecode(time, sequence->frameRate, sequence->dropFrame);
    const json stats{{"project", wabcut::pathToUtf8(projectFile)},
                     {"sequence", sequence->name},
                     {"time", tc},
                     {"width", image->width},
                     {"height", image->height},
                     {"exposure", s.exposure},
                     {"cast", s.cast},
                     {"luma", json{{"min", rounded(s.lumaMin, 0.1)}, {"max", rounded(s.lumaMax, 0.1)}, {"mean", rounded(s.lumaMean, 0.1)},
                                   {"p1", rounded(s.p01, 0.1)}, {"p5", rounded(s.p05, 0.1)}, {"median", rounded(s.p50, 0.1)},
                                   {"p95", rounded(s.p95, 0.1)}, {"p99", rounded(s.p99, 0.1)}}},
                     {"units", "IRE (0 = black, 100 = reference white)"},
                     {"clippedHighlights", rounded(s.clippedHigh, 0.0001)},
                     {"crushedBlacks", rounded(s.clippedLow, 0.0001)},
                     {"meanRgb", json::array({rounded(s.meanRgb[0], 0.001), rounded(s.meanRgb[1], 0.001), rounded(s.meanRgb[2], 0.001)})},
                     {"saturation", rounded(s.saturationMean, 0.1)},
                     {"castHue", rounded(s.castHue, 0.1)},
                     {"castStrength", rounded(s.castStrength, 0.01)},
                     {"pixels", s.pixels}};
    if (!out.empty()) {
        // Layout: picture | vectorscope on top; luma waveform | RGB parade; histogram.
        int ww = 0, wh = 0, pw = 0, ph = 0, vw = 0, vh = 0, hw = 0, hh = 0;
        const auto wave = wabcut::color::renderWaveform(data, 0, ww, wh);
        const auto parade = wabcut::color::renderWaveform(data, 1, pw, ph);
        const auto vector = wabcut::color::renderVectorscope(data, vw, vh);
        const auto hist = wabcut::color::renderHistogram(data, hw, hh);
        const int panelH = 256;
        QImage sheet(512 + 768 + 24, 2 * panelH + 128 + 48, QImage::Format_RGB888);
        sheet.fill(QColor(18, 18, 20));
        QPainter p(&sheet);
        std::vector<std::uint8_t> rgba8(image->rgba.size());
        for (std::size_t i = 0; i < rgba8.size(); ++i) {
            rgba8[i] = static_cast<std::uint8_t>(std::lround(std::clamp(image->rgba[i], 0.0f, 1.0f) * 255.0f));
            if (i % 4 == 3) {
                rgba8[i] = 255;
            }
        }
        const QRect pictureRect(8, 8, 512, panelH);
        p.drawImage(pictureRect, imageOf(rgba8, image->width, image->height).scaled(512, panelH, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        const QRect vectorRect(512 + 16 + 256, 8, panelH, panelH);
        p.drawImage(vectorRect, imageOf(vector, vw, vh));
        vectorGraticule(p, vectorRect, zoom);
        const QRect waveRect(8, panelH + 16, 512, panelH);
        p.drawImage(waveRect, imageOf(wave, ww, wh));
        graticule(p, waveRect, true);
        const QRect paradeRect(512 + 16, panelH + 16, 768, panelH);
        p.drawImage(paradeRect, imageOf(parade, pw, ph));
        graticule(p, paradeRect, false);
        const QRect histRect(8, 2 * panelH + 24, 512, 128);
        p.drawImage(histRect, imageOf(hist, hw, hh));
        p.setPen(QColor(220, 220, 220));
        p.drawText(QRect(512 + 16, 2 * panelH + 24, 768, 128), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                   QStringLiteral("%1 at %2\nexposure: %3, cast: %4\nluma IRE: median %5, p1 %6, p99 %7\nclipped %8 %%, crushed %9 %%")
                       .arg(QString::fromStdString(sequence->name), QString::fromStdString(tc), QString::fromStdString(s.exposure),
                            QString::fromStdString(s.cast))
                       .arg(s.p50, 0, 'f', 1)
                       .arg(s.p01, 0, 'f', 1)
                       .arg(s.p99, 0, 'f', 1)
                       .arg(s.clippedHigh * 100.0, 0, 'f', 2)
                       .arg(s.clippedLow * 100.0, 0, 'f', 2));
        p.end();
        if (!sheet.save(QString::fromStdString(out))) {
            std::fprintf(stderr, "cannot write %s\n", out.c_str());
            return 2;
        }
    }
    const std::string text = stats.dump(2) + "\n";
    if (!jsonOut.empty()) {
        std::ofstream file(wabcut::pathFromUtf8(jsonOut), std::ios::binary | std::ios::trunc);
        file << text;
        if (!file) {
            std::fprintf(stderr, "cannot write %s\n", jsonOut.c_str());
            return 2;
        }
    } else {
        std::fwrite(text.data(), 1, text.size(), stdout);
    }
    return 0;
}
