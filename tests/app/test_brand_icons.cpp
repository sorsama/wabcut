// The app icon set (design language v3, the echo mark): PNGs for Linux, wabcut.ico (Windows) and wabcut.icns (macOS)
// in src/app/resources/brand/icons/, rendered with Qt's SVG renderer from three drawings of the mark sized for their
// pixels: wabcut-app-icon-small.svg (16-24 px: three clips), wabcut-app-icon-medium.svg (32-48 px: one echo) and
// wabcut-app-icon.svg (64 px and up: the full mark). The hidden test case [.icongen] (re)generates them:
//   wabcut_app_tests "[.icongen]"
// and the normal case checks that the committed files still match the SVGs.

#include "ShellFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

using namespace wabcut::test;

namespace {

QString brandDir() {
    return QDir::cleanPath(QStringLiteral(WABCUT_APP_QML_DIR "/../resources/brand"));
}

constexpr std::array kPngSizes{16, 24, 32, 48, 64, 128, 256, 512, 1024};
constexpr std::array kIcoSizes{16, 24, 32, 48, 64, 128, 256};
struct IcnsEntry {
    const char* type;
    int size;
};
// PNG-carrying icns entries (macOS 10.7+).
constexpr std::array<IcnsEntry, 9> kIcnsEntries{{{"icp4", 16},
                                                 {"icp5", 32},
                                                 {"icp6", 64},
                                                 {"ic07", 128},
                                                 {"ic08", 256},
                                                 {"ic09", 512},
                                                 {"ic10", 1024},
                                                 {"ic11", 32},
                                                 {"ic12", 64}}};

/// The drawing for an icon of `size` px.
QString sourceFor(int size) {
    if (size <= 24)
        return QStringLiteral("/wabcut-app-icon-small.svg");
    if (size <= 48)
        return QStringLiteral("/wabcut-app-icon-medium.svg");
    return QStringLiteral("/wabcut-app-icon.svg");
}

QImage render(int size) {
    QImageReader reader(brandDir() + sourceFor(size));
    reader.setScaledSize(QSize(size, size));
    QImage image = reader.read();
    return image.convertToFormat(QImage::Format_ARGB32);
}

/// How many pixels differ by more than `levels` in any channel (every pixel when the sizes differ).
int pixelsDiffering(const QImage& a, const QImage& b, int levels) {
    if (a.size() != b.size())
        return std::max(a.width() * a.height(), b.width() * b.height());
    int count = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y);
            const QRgb q = b.pixel(x, y);
            const int most = std::max({std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                                       std::abs(qBlue(p) - qBlue(q)), std::abs(qAlpha(p) - qAlpha(q))});
            count += most > levels ? 1 : 0;
        }
    }
    return count;
}

QByteArray png(const QImage& image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

QByteArray ico() {
    // ICONDIR, then one ICONDIRENTRY per size, then the PNG images (Windows Vista+ reads PNG entries).
    std::vector<QByteArray> images;
    for (const int s : kIcoSizes)
        images.push_back(png(render(s)));
    QByteArray out;
    const auto u16 = [&](quint16 v) {
        char b[2];
        qToLittleEndian(v, b);
        out.append(b, 2);
    };
    const auto u32 = [&](quint32 v) {
        char b[4];
        qToLittleEndian(v, b);
        out.append(b, 4);
    };
    u16(0);
    u16(1);
    u16(static_cast<quint16>(kIcoSizes.size()));
    quint32 offset = 6 + 16 * static_cast<quint32>(kIcoSizes.size());
    for (std::size_t i = 0; i < kIcoSizes.size(); ++i) {
        const int s = kIcoSizes[i];
        out.append(static_cast<char>(s >= 256 ? 0 : s));
        out.append(static_cast<char>(s >= 256 ? 0 : s));
        out.append('\0'); // palette
        out.append('\0'); // reserved
        u16(1);           // planes
        u16(32);          // bits per pixel
        u32(static_cast<quint32>(images[i].size()));
        u32(offset);
        offset += static_cast<quint32>(images[i].size());
    }
    for (const QByteArray& image : images)
        out.append(image);
    return out;
}

QByteArray icns() {
    QByteArray body;
    for (const IcnsEntry& e : kIcnsEntries) {
        const QByteArray image = png(render(e.size));
        body.append(e.type, 4);
        char len[4];
        qToBigEndian(static_cast<quint32>(image.size() + 8), len);
        body.append(len, 4);
        body.append(image);
    }
    QByteArray out("icns", 4);
    char len[4];
    qToBigEndian(static_cast<quint32>(body.size() + 8), len);
    out.append(len, 4);
    out.append(body);
    return out;
}

} // namespace

TEST_CASE("[.icongen] generate the app icon set from the SVGs", "[.icongen]") {
    const QString dir = brandDir() + QStringLiteral("/icons");
    REQUIRE(QDir().mkpath(dir));
    for (const int s : kPngSizes)
        REQUIRE(render(s).save(dir + QStringLiteral("/wabcut-%1.png").arg(s), "PNG"));
    QFile icoFile(dir + QStringLiteral("/wabcut.ico"));
    REQUIRE(icoFile.open(QIODevice::WriteOnly));
    icoFile.write(ico());
    icoFile.close();
    QFile icnsFile(dir + QStringLiteral("/wabcut.icns"));
    REQUIRE(icnsFile.open(QIODevice::WriteOnly));
    icnsFile.write(icns());
    icnsFile.close();
}

TEST_CASE("The app icon set is rendered from the brand SVGs", "[app][design][icons]") {
    const QString dir = brandDir() + QStringLiteral("/icons");
    for (const int s : kPngSizes) {
        INFO(s);
        const QImage committed = QImage(dir + QStringLiteral("/wabcut-%1.png").arg(s)).convertToFormat(QImage::Format_ARGB32);
        REQUIRE_FALSE(committed.isNull());
        CHECK(committed.size() == QSize(s, s));
        // Regenerate with "[.icongen]" after changing an SVG. Antialiasing may differ at a few edge pixels (Qt's
        // raster engine takes other SIMD paths on other CPUs, e.g. a CI runner); a stale icon differs at many.
        CHECK(pixelsDiffering(committed, render(s), 8) <= s * s / 50);
    }
    QFile icoFile(dir + QStringLiteral("/wabcut.ico"));
    REQUIRE(icoFile.open(QIODevice::ReadOnly));
    const QByteArray icoBytes = icoFile.readAll();
    CHECK(icoBytes.size() > 6);
    CHECK(qFromLittleEndian<quint16>(icoBytes.constData() + 4) == kIcoSizes.size());
    QFile icnsFile(dir + QStringLiteral("/wabcut.icns"));
    REQUIRE(icnsFile.open(QIODevice::ReadOnly));
    const QByteArray icnsBytes = icnsFile.readAll();
    CHECK(icnsBytes.left(4) == QByteArray("icns"));
    CHECK(qFromBigEndian<quint32>(icnsBytes.constData() + 4) == static_cast<quint32>(icnsBytes.size()));
    // The 16 px icon still shows the mark: not one flat colour, and both inks (violet and lime) are in it.
    const QImage small = render(16);
    bool varied = false;
    bool violet = false;
    bool lime = false;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const QColor c = small.pixelColor(x, y);
            varied = varied || (x > 0 && small.pixel(x, y) != small.pixel(x - 1, y));
            violet = violet || (c.blue() > 200 && c.red() > 120 && c.green() < 140);
            lime = lime || (c.green() > 200 && c.red() > 170 && c.blue() < 120);
        }
    }
    CHECK(varied);
    CHECK(violet);
    CHECK(lime);

}
