// The Scopes panel's waveform in an HDR project (I.9): PQ code values drawn with a nits graticule, the HLG signal with a
// percent graticule (75 % = reference white), SDR with IRE as before. Paints ScopeView into images (no window) and keeps
// them as screenshots under <build>/test-output/ui/.

#include "ShellFixture.h"

#include "ScopesBridge.h"

#include <wabcut/color/ColorSpaces.h>
#include <wabcut/color/Scopes.h>

#include <catch2/catch_test_macros.hpp>

#include <QImage>
#include <QPainter>

#include <cmath>
#include <vector>

using namespace wabcut;

namespace {

/// An HDR picture as PQ code values: a luminance ramp from 0 to 1000 nits, a 203-nit grey band and a 1000-nit sky.
std::vector<float> pqPicture(int w, int h) {
    std::vector<float> rgb(static_cast<std::size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double u = (x + 0.5) / w;
            const double nits = y < h / 4 ? 1000.0 : y < h / 2 ? 203.0 : 1000.0 * u * u;
            const double code = color::nitsToPq(nits);
            float* p = rgb.data() + (static_cast<std::size_t>(y) * w + x) * 3;
            p[0] = p[1] = p[2] = static_cast<float>(code);
        }
    }
    return rgb;
}

QImage paintWaveform(const color::ScopeData& data) {
    app::ScopeHub::shared().deliver(data);
    app::ScopeView view;
    view.setKind(QStringLiteral("waveform"));
    view.setSize(QSizeF(720, 360));
    QImage image(720, 360, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::black);
    QPainter painter(&image);
    view.paint(&painter);
    painter.end();
    return image;
}

/// Lit pixels (any channel above `level`) in row y.
int litInRow(const QImage& image, int y, int level = 60) {
    int lit = 0;
    for (int x = 40; x < image.width() - 8; ++x) {
        const QRgb c = image.pixel(x, y);
        lit += qRed(c) > level || qGreen(c) > level || qBlue(c) > level ? 1 : 0;
    }
    return lit;
}

} // namespace

TEST_CASE("HDR waveform: a nits graticule for PQ, percent for HLG", "[app][scopes][hdr]") {
    constexpr int kW = 320;
    constexpr int kH = 180;
    const std::vector<float> rgb = pqPicture(kW, kH);
    color::ScopeData pq = color::computeScopes(rgb.data(), kW, kH, 3);
    pq.scale = 1;
    const QImage nits = paintWaveform(pq);
    REQUIRE(nits.save(test::outputDir() + QStringLiteral("/scopes_nits_waveform.png")));
    // The 203-nit band and the 1000-nit sky draw flat traces at their PQ levels (0.581 and 0.752 of the plot height).
    const double top = 6.0, bottom = 360.0 - 6.0;
    const auto rowOf = [&](double code) { return static_cast<int>(std::lround(bottom - (bottom - top - 1) * code)); };
    int at203 = 0, at1000 = 0;
    for (int dy = -2; dy <= 2; ++dy) {
        at203 = std::max(at203, litInRow(nits, rowOf(color::nitsToPq(203.0)) + dy));
        at1000 = std::max(at1000, litInRow(nits, rowOf(color::nitsToPq(1000.0)) + dy));
    }
    CHECK(at203 > 600);
    CHECK(at1000 > 600);

    color::ScopeData hlg = pq;
    hlg.scale = 2;
    REQUIRE(paintWaveform(hlg).save(test::outputDir() + QStringLiteral("/scopes_hlg_waveform.png")));
    color::ScopeData sdr = pq;
    sdr.scale = 0;
    REQUIRE(paintWaveform(sdr).save(test::outputDir() + QStringLiteral("/scopes_sdr_waveform.png")));
}
