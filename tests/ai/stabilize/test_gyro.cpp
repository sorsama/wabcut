// 0.7 (card #130): stabilizing with the camera's gyro. The GoPro GPMF and Sony rtmd parsers on files built here byte by
// byte (and on two real GoPro payloads from the gpmf-parser repository, MIT / Apache-2.0: tests/data/gpmf), cut and
// corrupted at every length; the quaternion integral; the compensation of a known synthetic shake, checked by warping a
// textured picture exactly as the renderer's corner pin does and comparing it with the shake-free picture; Smoothness 0,
// No Motion, the rolling shutter's skew, frames beyond the recorded gyro; and Auto-sync finding a known offset, field of
// view and axis turn from the picture's motion.

#include "GyroShake.h"

#include <wabcut/ai/stabilize/GyroMotion.h>
#include <wabcut/ai/stabilize/Stabilize.h>
#include <wabcut/gyro/Gyro.h>
#include <wabcut/gyro/Rotation.h>
#include <wabcut/model/Time.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::gyro;
using namespace wabcut::ai::stabilize;
using Catch::Approx;

namespace {

// ---- Byte builders ------------------------------------------------------------------------------------------------------

using Bytes = std::vector<std::uint8_t>;

void put16(Bytes& b, unsigned v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}
void put32(Bytes& b, std::uint32_t v) {
    put16(b, v >> 16);
    put16(b, v & 0xFFFF);
}
void put64(Bytes& b, std::uint64_t v) {
    put32(b, static_cast<std::uint32_t>(v >> 32));
    put32(b, static_cast<std::uint32_t>(v));
}
void putFloat(Bytes& b, float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    put32(b, bits);
}

/// A GPMF item: key, type, sample size, repeat, payload padded to four bytes.
void klv(Bytes& out, const char (&key)[5], char type, unsigned size, unsigned repeat, const Bytes& payload) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(key[i]));
    }
    out.push_back(static_cast<std::uint8_t>(type));
    out.push_back(static_cast<std::uint8_t>(size));
    put16(out, repeat);
    out.insert(out.end(), payload.begin(), payload.end());
    while (out.size() % 4 != 0) {
        out.push_back(0);
    }
}
Bytes text(const std::string& s) { return Bytes(s.begin(), s.end()); }

struct GpmfStream {
    const char* key = "GYRO";
    std::vector<std::array<int, 3>> columns;
    int scale = 1000;
    std::string unit = "rad/s";
    std::string orin; ///< empty: none
    std::optional<std::uint64_t> stamp;
};

Bytes gpmfStream(const GpmfStream& s) {
    Bytes inner;
    if (s.stamp) {
        Bytes v;
        put64(v, *s.stamp);
        klv(inner, "STMP", 'J', 8, 1, v);
    }
    Bytes tsmp;
    put32(tsmp, static_cast<std::uint32_t>(s.columns.size()));
    klv(inner, "TSMP", 'L', 4, 1, tsmp);
    klv(inner, "STNM", 'c', 1, 9, text("Gyroscope"));
    klv(inner, "SIUN", 'c', static_cast<unsigned>(s.unit.size()), 1, text(s.unit));
    if (!s.orin.empty()) {
        klv(inner, "ORIN", 'c', 3, 1, text(s.orin));
    }
    Bytes scal;
    put16(scal, static_cast<unsigned>(s.scale));
    klv(inner, "SCAL", 's', 2, 1, scal);
    Bytes data;
    for (const auto& c : s.columns) {
        for (const int v : c) {
            put16(data, static_cast<unsigned>(v) & 0xFFFF);
        }
    }
    const std::string key = s.key;
    char k[5] = {key[0], key[1], key[2], key[3], 0};
    klv(inner, k, 's', 6, static_cast<unsigned>(s.columns.size()), data);
    Bytes out;
    klv(out, "STRM", 0, 4, static_cast<unsigned>(inner.size() / 4), inner);
    return out;
}

Bytes gpmfPacket(const std::string& name, const std::vector<Bytes>& streams) {
    Bytes inner;
    Bytes id;
    put32(id, 1);
    klv(inner, "DVID", 'L', 4, 1, id);
    klv(inner, "DVNM", 'c', 1, static_cast<unsigned>(name.size()), text(name));
    for (const Bytes& s : streams) {
        inner.insert(inner.end(), s.begin(), s.end());
    }
    Bytes out;
    klv(out, "DEVC", 0, 4, static_cast<unsigned>(inner.size() / 4), inner);
    return out;
}

std::vector<std::array<int, 3>> columnsOf(int n, std::array<int, 3> first, int step = 10) {
    std::vector<std::array<int, 3>> out;
    for (int i = 0; i < n; ++i) {
        out.push_back({first[0] + step * i, first[1] - step * i, first[2] + 2 * step * i});
    }
    return out;
}

/// Sony: one item.
void sonyItem(Bytes& out, unsigned tag, const Bytes& data) {
    put16(out, tag);
    put16(out, static_cast<unsigned>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
}
Bytes be16Bytes(unsigned v) {
    Bytes b;
    put16(b, v);
    return b;
}
Bytes be32Bytes(std::uint32_t v) {
    Bytes b;
    put32(b, v);
    return b;
}

struct SonyFrame {
    std::vector<std::array<int, 3>> gyro;
    double scale = 100.0;           ///< readings per deg/s
    unsigned orientation = 0x0420;  ///< "XYZ"
    bool container = true;
};

Bytes sonyPacket(const SonyFrame& f, bool withLens = true) {
    Bytes inner;
    sonyItem(inner, 0xE438, Bytes{0}); // deg/s
    Bytes scale;
    putFloat(scale, static_cast<float>(f.scale));
    sonyItem(inner, 0xE439, scale);
    sonyItem(inner, 0xE43A, be16Bytes(f.orientation));
    Bytes gyro;
    put32(gyro, static_cast<std::uint32_t>(f.gyro.size()));
    put32(gyro, 6);
    for (const auto& r : f.gyro) {
        for (const int v : r) {
            put16(gyro, static_cast<unsigned>(v) & 0xFFFF);
        }
    }
    sonyItem(inner, 0xE43B, gyro);
    if (withLens) {
        sonyItem(inner, 0xE40E, be32Bytes(12000)); // readout 12 ms
        sonyItem(inner, 0xE40D, be32Bytes(8000));  // exposure 8 ms
        Bytes px;
        put16(px, 7680);
        put16(px, 4320);
        sonyItem(inner, 0xE405, px);
        Bytes pitch;
        put16(pitch, 4300);
        put16(pitch, 4300);
        sonyItem(inner, 0xE407, pitch); // 4.3 um
        Bytes crop;
        put32(crop, 3840);
        put32(crop, 2160);
        sonyItem(inner, 0xE40A, crop);
        sonyItem(inner, 0x8005, be16Bytes(0xC0F0)); // 240 x 10^-4 m = 24 mm
    }
    Bytes out(0x1C, 0);
    out[1] = 0x1C;
    const Bytes label = {0x06, 0x0E, 0x2B, 0x34, 0x02, 0x53, 0x01, 0x01, 0x0C, 0x02, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00};
    out.insert(out.end(), label.begin(), label.end());
    if (f.container) {
        sonyItem(out, 0x8300, inner);
    } else {
        out.insert(out.end(), inner.begin(), inner.end());
    }
    return out;
}

Bytes readFile(const std::string& name) {
    std::ifstream in(std::string(WABCUT_TEST_DATA_DIR) + "/gpmf/" + name, std::ios::binary);
    REQUIRE(in);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<Packet> asPackets(const std::vector<Bytes>& bytes, double duration, double first = 0.0) {
    std::vector<Packet> out;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out.push_back({first + duration * static_cast<double>(i), duration, std::span<const std::uint8_t>(bytes[i])});
    }
    return out;
}

void requireSane(const GyroTrack& t) {
    REQUIRE(t.times.size() == t.omega.size());
    for (std::size_t i = 0; i < t.times.size(); ++i) {
        REQUIRE(std::isfinite(t.times[i]));
        REQUIRE(std::isfinite(t.omega[i].x));
        REQUIRE(std::isfinite(t.omega[i].y));
        REQUIRE(std::isfinite(t.omega[i].z));
        if (i > 0) {
            REQUIRE(t.times[i] > t.times[i - 1]);
        }
    }
}

} // namespace

// ---- GoPro GPMF -----------------------------------------------------------------------------------------------------------

TEST_CASE("GPMF: a HERO8-style stream is read in the camera frame, scaled and timed by the camera clock", "[gyro][gpmf]") {
    // ORIN "zxY": column 0 is the camera's -Z, column 1 -X, column 2 +Y; omega = (-X, -Z, -Y) = (c1, c0, -c2).
    GpmfStream first;
    first.columns = {{100, 200, 300}, {110, 190, 320}, {120, 180, 340}, {130, 170, 360}};
    first.scale = 1000;
    first.orin = "zxY";
    first.stamp = 500'000; // microseconds
    GpmfStream second = first;
    second.stamp = 1'500'000;
    const std::vector<Bytes> packets = {gpmfPacket("HERO8 Black", {gpmfStream(first)}), gpmfPacket("HERO8 Black", {gpmfStream(second)})};
    const auto track = parseGpmf(asPackets(packets, 1.0, 10.0));
    REQUIRE(track);
    CHECK(track->camera == Camera::GoPro);
    CHECK(track->make == "GoPro");
    CHECK(track->model == "HERO8 Black");
    CHECK(track->axesChecked);
    REQUIRE(track->omega.size() == 8);
    CHECK(track->omega[0].x == Approx(0.2));
    CHECK(track->omega[0].y == Approx(0.1));
    CHECK(track->omega[0].z == Approx(-0.3));
    CHECK(track->omega[1].x == Approx(0.19));
    // STMP spaces the readings (the second packet starts 1 s after the first), anchored at the first packet's time.
    CHECK(track->times[0] == Approx(10.0));
    CHECK(track->times[1] == Approx(10.25));
    CHECK(track->times[4] == Approx(11.0));
    CHECK(track->times[7] == Approx(11.75).margin(0.3)); // the last packet's spacing is the mean's
    CHECK(track->rateHz == Approx(4.0).margin(0.5));
    requireSane(*track);
}

TEST_CASE("GPMF: a camera name that is not plain text is cut to printable ASCII, so a project holding it still saves", "[gyro][gpmf][security]") {
    GpmfStream stream;
    stream.columns = {{100, 200, 300}, {110, 190, 320}};
    stream.scale = 1000;
    stream.orin = "zxY";
    stream.stamp = 500'000;
    const std::string hostile = std::string("HERO\xFF\xFE\xFD") + std::string(100, 'x'); // bytes no encoding makes of text, then too long
    const std::vector<Bytes> packets = {gpmfPacket(hostile, {gpmfStream(stream)})};
    const auto track = parseGpmf(asPackets(packets, 1.0, 10.0));
    REQUIRE(track);
    CHECK(track->model.size() <= 64);
    CHECK(track->model.rfind("HERO", 0) == 0);
    for (const char c : track->model) {
        CHECK(static_cast<unsigned char>(c) >= 0x20);
        CHECK(static_cast<unsigned char>(c) < 0x7F);
    }
}

TEST_CASE("GPMF: without an ORIN the order is the camera's (HERO5 Z,X,Y; HERO6 and 7 Y,-X,Z) and degrees convert", "[gyro][gpmf]") {
    GpmfStream s;
    s.columns = {{100, 200, 300}};
    s.scale = 1000;
    const auto hero5 = parseGpmf(asPackets({gpmfPacket("Camera", {gpmfStream(s)})}, 1.0));
    REQUIRE(hero5);
    CHECK(hero5->model.empty()); // the HERO5 calls itself "Camera"
    // columns (Z, X, Y) = (0.1, 0.2, 0.3): omega = (-X, -Z, -Y)
    CHECK(hero5->omega[0].x == Approx(-0.2));
    CHECK(hero5->omega[0].y == Approx(-0.1));
    CHECK(hero5->omega[0].z == Approx(-0.3));
    const auto hero6 = parseGpmf(asPackets({gpmfPacket("Hero6 Black", {gpmfStream(s)})}, 1.0));
    REQUIRE(hero6);
    // columns (Y, -X, Z) = (0.1, 0.2, 0.3): X = -0.2, Y = 0.1, Z = 0.3 -> omega = (0.2, -0.3, -0.1)
    CHECK(hero6->omega[0].x == Approx(0.2));
    CHECK(hero6->omega[0].y == Approx(-0.3));
    CHECK(hero6->omega[0].z == Approx(-0.1));
    GpmfStream degrees = s;
    degrees.unit = "deg/s";
    const auto deg = parseGpmf(asPackets({gpmfPacket("Camera", {gpmfStream(degrees)})}, 1.0));
    REQUIRE(deg);
    CHECK(deg->omega[0].x == Approx(-0.2 * std::numbers::pi / 180.0));
}

TEST_CASE("GPMF: a track without a gyroscope is refused with a reason, an accelerometer is read beside the gyro", "[gyro][gpmf]") {
    GpmfStream accel;
    accel.key = "ACCL";
    accel.columns = {{1, 2, 3}, {4, 5, 6}};
    const auto none = parseGpmf(asPackets({gpmfPacket("HERO8 Black", {gpmfStream(accel)})}, 1.0));
    REQUIRE_FALSE(none);
    CHECK(none.error().code == ErrorCode::ParseError);
    CHECK_FALSE(none.error().message.empty());
    GpmfStream gyroStream;
    gyroStream.columns = {{1, 2, 3}, {4, 5, 6}};
    const auto both = parseGpmf(asPackets({gpmfPacket("HERO8 Black", {gpmfStream(gyroStream), gpmfStream(accel)})}, 1.0));
    REQUIRE(both);
    CHECK(both->omega.size() == 2);
    CHECK(both->accel.size() == 2);
    CHECK(both->accelTimes.size() == 2);
    CHECK(parseGpmf({}).error().code == ErrorCode::ParseError);
}

TEST_CASE("GPMF: real GoPro payloads (HERO5 and HERO6, gpmf-parser samples) read as the camera recorded them", "[gyro][gpmf]") {
    const Bytes hero5 = readFile("hero5.raw");
    const auto track = parseGpmf(asPackets({hero5}, 1.0));
    REQUIRE(track);
    CHECK(track->times.size() == 399);
    CHECK(track->accel.size() == 199);
    CHECK(track->rateHz == Approx(399.0).margin(0.01));
    // First reading, columns (Z, X, Y) = (0.04926764, 0.04074567, 0.02796272): omega = (-X, -Z, -Y).
    CHECK(track->omega[0].x == Approx(-0.04074567).margin(1e-6));
    CHECK(track->omega[0].y == Approx(-0.04926764).margin(1e-6));
    CHECK(track->omega[0].z == Approx(-0.02796272).margin(1e-6));
    // The camera was upright: the accelerometer feels gravity up, which is -y in the camera frame (y is down).
    double meanX = 0.0, meanY = 0.0, meanZ = 0.0;
    for (const Vec3& a : track->accel) {
        meanX += a.x;
        meanY += a.y;
        meanZ += a.z;
    }
    const double n = static_cast<double>(track->accel.size());
    CHECK(meanY / n < -9.0);
    CHECK(meanY / n > -10.5);
    CHECK(std::abs(meanX / n) < 3.0);
    CHECK(std::abs(meanZ / n) < 3.0);
    requireSane(*track);

    const auto hero6 = parseGpmf(asPackets({readFile("hero6.raw")}, 1.0));
    REQUIRE(hero6);
    CHECK(hero6->model == "Hero6 Black");
    CHECK(hero6->times.size() == 204);
    // Columns (Y, -X, Z) = (-0.64483493, -0.18370607, -0.27050053): X = 0.18370607, Y = -0.64483493, Z = -0.27050053.
    CHECK(hero6->omega[0].x == Approx(-0.18370607).margin(1e-6));
    CHECK(hero6->omega[0].y == Approx(0.27050053).margin(1e-6));
    CHECK(hero6->omega[0].z == Approx(0.64483493).margin(1e-6));
}

TEST_CASE("GPMF: every cut and a thousand corruptions of a payload end in an error or a sane track, never a crash", "[gyro][gpmf][security]") {
    GpmfStream s;
    s.columns = columnsOf(20, {5, 6, 7});
    s.orin = "zxY";
    s.stamp = 1000;
    GpmfStream a = s;
    a.key = "ACCL";
    const Bytes whole = gpmfPacket("HERO8 Black", {gpmfStream(s), gpmfStream(a)});
    for (std::size_t length = 0; length <= whole.size(); ++length) {
        const Bytes cut(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(length));
        const auto track = parseGpmf(asPackets({cut}, 1.0));
        if (track) {
            requireSane(*track);
        }
    }
    std::uint32_t seed = 12345;
    const auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    for (int round = 0; round < 1000; ++round) {
        Bytes damaged = whole;
        for (int k = 0; k < 1 + round % 6; ++k) {
            damaged[next() % damaged.size()] = static_cast<std::uint8_t>(next());
        }
        const auto track = parseGpmf(asPackets({damaged, damaged}, 1.0));
        if (track) {
            requireSane(*track);
        }
    }
    // A repeat count far beyond the data, and a nest that claims more than the packet holds.
    Bytes lie = whole;
    lie[whole.size() / 2] = 0xFF;
    const auto lied = parseGpmf(asPackets({lie}, 1.0));
    if (lied) {
        requireSane(*lied);
    }
}

// ---- Sony rtmd ---------------------------------------------------------------------------------------------------------------

TEST_CASE("Sony rtmd: readings are scaled to rad/s, put in the camera frame and spread over their frame", "[gyro][sony]") {
    SonyFrame f;
    f.gyro = {{100, 200, 300}, {-100, 0, 100}, {0, 0, 0}, {50, 50, 50}};
    const std::vector<Bytes> packets = {sonyPacket(f), sonyPacket(f)};
    const auto track = parseSonyRtmd(asPackets(packets, 0.04, 2.0), "ILCE-7M4");
    REQUIRE(track);
    CHECK(track->camera == Camera::Sony);
    CHECK(track->model == "ILCE-7M4");
    CHECK_FALSE(track->axesChecked); // Sony's axis table is not checked against footage
    REQUIRE(track->omega.size() == 8);
    // Orientation 0x420 = "XYZ": omega = (-x, -y, +z) of the raw readings, 100 per deg/s.
    const double deg = std::numbers::pi / 180.0;
    CHECK(track->omega[0].x == Approx(-1.0 * deg));
    CHECK(track->omega[0].y == Approx(-2.0 * deg));
    CHECK(track->omega[0].z == Approx(3.0 * deg));
    CHECK(track->omega[1].x == Approx(1.0 * deg));
    CHECK(track->times[0] == Approx(2.0));
    CHECK(track->times[1] == Approx(2.01));
    CHECK(track->times[4] == Approx(2.04)); // the second frame starts where the first ends
    CHECK(track->rateHz == Approx(100.0).margin(1.0));
    requireSane(*track);
}

TEST_CASE("Sony rtmd: the frame's read-out, exposure and lens give the rolling shutter and the field of view", "[gyro][sony]") {
    SonyFrame f;
    f.gyro = {{1, 2, 3}, {4, 5, 6}};
    const auto track = parseSonyRtmd(asPackets({sonyPacket(f)}, 0.04));
    REQUIRE(track);
    REQUIRE(track->readoutSeconds);
    CHECK(*track->readoutSeconds == Approx(0.012));
    REQUIRE(track->exposureSeconds);
    CHECK(*track->exposureSeconds == Approx(0.008));
    REQUIRE(track->focalLengthMm);
    CHECK(*track->focalLengthMm == Approx(24.0));
    REQUIRE(track->imagedWidthMm);
    CHECK(*track->imagedWidthMm == Approx(3840 * 4.3e-3)); // 16.512 mm
    REQUIRE(track->horizontalFovDegrees);
    CHECK(*track->horizontalFovDegrees == Approx(2.0 * std::atan(16.512 / 48.0) * 180.0 / std::numbers::pi));
    const auto plain = parseSonyRtmd(asPackets({sonyPacket(f, false)}, 0.04));
    REQUIRE(plain);
    CHECK_FALSE(plain->readoutSeconds);
    CHECK_FALSE(plain->horizontalFovDegrees);
}

TEST_CASE("Sony rtmd: items outside a container are read, other orientations turn the axes, a track without a gyro says so",
          "[gyro][sony]") {
    SonyFrame f;
    f.gyro = {{100, 200, 300}};
    f.container = false;
    f.orientation = 0x152; // "Yzx" (the RX100 VII)
    const auto track = parseSonyRtmd(asPackets({sonyPacket(f)}, 0.04));
    REQUIRE(track);
    // "Yzx" -> swap first two, flip the third's sign: "zYX": out = (-V.z, V.y, V.x); omega = (-out.y, -out.x, -out.z).
    const double deg = std::numbers::pi / 180.0;
    CHECK(track->omega[0].x == Approx(-2.0 * deg));
    CHECK(track->omega[0].y == Approx(3.0 * deg));
    CHECK(track->omega[0].z == Approx(-1.0 * deg));
    // A packet that is not rtmd at all (no 00 1C header) and an empty list.
    const Bytes junk(200, 0x55);
    const auto none = parseSonyRtmd(asPackets({junk}, 0.04));
    REQUIRE_FALSE(none);
    CHECK(none.error().code == ErrorCode::ParseError);
    CHECK_FALSE(parseSonyRtmd({}));
}

TEST_CASE("Sony rtmd: every cut and a thousand corruptions of a packet end in an error or a sane track", "[gyro][sony][security]") {
    SonyFrame f;
    f.gyro = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
    const Bytes whole = sonyPacket(f);
    for (std::size_t length = 0; length <= whole.size(); ++length) {
        const Bytes cut(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(length));
        const auto track = parseSonyRtmd(asPackets({cut, cut}, 0.04));
        if (track) {
            requireSane(*track);
        }
    }
    std::uint32_t seed = 987;
    const auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    for (int round = 0; round < 1000; ++round) {
        Bytes damaged = whole;
        for (int k = 0; k < 1 + round % 6; ++k) {
            damaged[0x1C + next() % (damaged.size() - 0x1C)] = static_cast<std::uint8_t>(next());
        }
        const auto track = parseSonyRtmd(asPackets({damaged, damaged}, 0.04));
        if (track) {
            requireSane(*track);
        }
    }
    // A container nested as deep as the bytes allow (each level is 4 bytes of header).
    Bytes deep(0x1C, 0);
    deep[1] = 0x1C;
    Bytes inner;
    sonyItem(inner, 0xE43A, be16Bytes(0x420));
    for (int level = 0; level < 600; ++level) {
        Bytes wrapper;
        sonyItem(wrapper, 0x8300, inner);
        inner = std::move(wrapper);
    }
    deep.insert(deep.end(), inner.begin(), inner.end());
    CHECK_FALSE(parseSonyRtmd(asPackets({deep}, 0.04))); // no gyro in it, and no stack overflow finding that out
}

TEST_CASE("Data tracks are classified: GoPro and Sony are read, DJI and others say why not", "[gyro]") {
    CHECK(classifyDataTrack("gpmd") == gyro::TrackKind::GoProGpmf);
    CHECK(classifyDataTrack("rtmd") == gyro::TrackKind::SonyRtmd);
    CHECK(isReadable(classifyDataTrack("gpmd")));
    CHECK(classifyDataTrack("djmd") == gyro::TrackKind::DjiMetadata);
    CHECK(classifyDataTrack("", "DJI meta") == gyro::TrackKind::DjiMetadata);
    CHECK_FALSE(isReadable(gyro::TrackKind::DjiMetadata));
    CHECK(unreadableReason(gyro::TrackKind::DjiMetadata).find("DJI") != std::string_view::npos);
    CHECK(classifyDataTrack("tmcd") == gyro::TrackKind::NotTelemetry);
    CHECK(unreadableReason(gyro::TrackKind::GoProGpmf).empty());
}

// ---- Rotations -------------------------------------------------------------------------------------------------------------

TEST_CASE("Quaternions: rotation vectors round-trip, matrices rotate, slerp goes along the shortest arc", "[gyro][rotation]") {
    for (const Vec3 v : {Vec3{0.1, -0.2, 0.3}, Vec3{1.5, 0.0, 0.0}, Vec3{0.0, 2.9, 0.0}, Vec3{-1.0, 1.0, 1.0}, Vec3{0, 0, 0}}) {
        const Vec3 back = toRotationVector(fromRotationVector(v));
        CHECK(back.x == Approx(v.x).margin(1e-9));
        CHECK(back.y == Approx(v.y).margin(1e-9));
        CHECK(back.z == Approx(v.z).margin(1e-9));
    }
    // A quarter turn about z takes x to y (right-handed).
    const auto m = toMatrix(fromRotationVector({0, 0, std::numbers::pi / 2.0}));
    CHECK(m[0] == Approx(0.0).margin(1e-12));
    CHECK(m[3] == Approx(1.0));
    CHECK(m[1] == Approx(-1.0));
    const Quat half = slerp(Quat{}, fromRotationVector({0, 0.8, 0}), 0.5);
    CHECK(toRotationVector(half).y == Approx(0.4));
    // The identity for damaged input.
    CHECK(normalized(Quat{0, 0, 0, 0}) == Quat{});
}

TEST_CASE("The orientation is the integral of the angular velocity; the integral is exact over any interval", "[gyro][rotation]") {
    GyroTrack track;
    for (int i = 0; i <= 200; ++i) {
        track.times.push_back(0.01 * i);
        track.omega.push_back({0.0, 0.5, 0.0}); // 0.5 rad/s about y for 2 s
    }
    const std::array<double, 5> at = {-1.0, 0.0, 1.0, 2.0, 5.0};
    const auto q = orientationAt(track, at);
    CHECK(toRotationVector(q[0]).y == Approx(0.0).margin(1e-12)); // before the first reading
    CHECK(toRotationVector(q[1]).y == Approx(0.0).margin(1e-12));
    CHECK(toRotationVector(q[2]).y == Approx(0.5).margin(1e-9));
    CHECK(toRotationVector(q[3]).y == Approx(1.0).margin(1e-9));
    CHECK(toRotationVector(q[4]).y == Approx(1.0).margin(1e-9)); // after the last: held
    const RateIntegral integral(track, 0.0, 2.0);
    CHECK(integral.between(0.5, 1.5).y == Approx(0.5).margin(1e-9));
    CHECK(integral.between(1.5, 0.5).y == Approx(-0.5).margin(1e-9));
    CHECK(integral.between(0.333, 0.7).y == Approx(0.5 * 0.367).margin(1e-9));
    CHECK(integral.between(5.0, 6.0).y == Approx(0.0).margin(1e-12));
}

// ---- The compensation of a known shake -------------------------------------------------------------------------------------

namespace {

constexpr int kW = 320;
constexpr int kH = 180;
constexpr double kFocal = 160.0; // a 90 degree field of view across 320 pixels
constexpr double kFps = 25.0;

using namespace wabcut::test::gyro_shake;

std::vector<Ticks> frameTimes(int count) {
    std::vector<Ticks> out;
    for (int i = 0; i < count; ++i) {
        out.push_back(framesToTicks(i, kRate25));
    }
    return out;
}

double pattern(double u, double v) {
    return 0.5 + 0.2 * std::sin(0.08 * u + 0.5) * std::cos(0.069 * v) + 0.2 * std::sin(0.034 * (u + v)) + 0.1 * std::sin(0.014 * u - 0.027 * v);
}

/// What a camera of orientation `q` sees at pixel (x, y): the world pattern along that pixel's ray (pixel centres at
/// integers, the principal point at the middle).
double seen(const Quat& q, double x, double y) {
    const auto r = toMatrix(q);
    const double cx = (kW - 1) / 2.0, cy = (kH - 1) / 2.0;
    const double dx = (x - cx) / kFocal, dy = (y - cy) / kFocal, dz = 1.0;
    const double wx = r[0] * dx + r[1] * dy + r[2] * dz;
    const double wy = r[3] * dx + r[4] * dy + r[5] * dz;
    const double wz = r[6] * dx + r[7] * dy + r[8] * dz;
    return pattern(kFocal * wx / wz, kFocal * wy / wz);
}

/// The shaken frame warped by the compensation `h`, as the renderer's corner pin draws it: the output pixel p shows the source
/// at h^-1(p).
double compensated(const Quat& shaken, const Homography& h, double x, double y) {
    const auto inverse = h.inverted();
    REQUIRE(inverse);
    const masks::Point2 s = inverse->map({x, y});
    return seen(shaken, s.x, s.y);
}

/// Mean absolute difference between two renderings over the middle of the frame.
template <class A, class B>
double meanDifference(A&& a, B&& b) {
    double sum = 0.0;
    int n = 0;
    for (int y = 40; y < kH - 40; y += 4) {
        for (int x = 60; x < kW - 60; x += 4) {
            sum += std::abs(a(x, y) - b(x, y));
            ++n;
        }
    }
    return sum / n;
}

GyroSetup plainSetup() {
    GyroSetup s;
    s.focalPixels = kFocal;
    s.horizontalFovDegrees = 90.0;
    s.fovFrom = "set";
    return s;
}

SmoothOptions stabilizeOnly() {
    SmoothOptions o;
    o.framing = Framing::StabilizeOnly;
    o.frameRate = kFps;
    o.smoothness = 0.5;
    return o;
}

} // namespace

TEST_CASE("The gyro removes a known synthetic shake from a textured picture (the corner pin the renderer draws)", "[gyro][stabilize][render]") {
    const int frames = 75;
    const GyroTrack track = gyroOfShake(-0.5, 3.5, 400.0);
    const auto times = frameTimes(frames);
    const Compensation c = stabilizeFromGyro(track, times, plainSetup(), stabilizeOnly(), kW, kH);
    REQUIRE(c.corners.size() == static_cast<std::size_t>(frames));
    CHECK(c.autoScale == Approx(1.0).margin(0.2));
    double rawError = 0.0, fixedError = 0.0;
    int counted = 0;
    // The ends of the clip lean on fewer neighbours; judge the frames with a full second of context.
    for (int i = 25; i < frames - 25; i += 3) {
        const double t = static_cast<double>(i) / kFps;
        const Quat shaken = orientationOf(shakeAngles(t));
        const Quat ideal = orientationOf(panAngles(t));
        const Homography h = compensationAt(c, static_cast<std::size_t>(i), kW, kH);
        rawError += meanDifference([&](double x, double y) { return seen(shaken, x, y); }, [&](double x, double y) { return seen(ideal, x, y); });
        fixedError += meanDifference([&](double x, double y) { return compensated(shaken, h, x, y); }, [&](double x, double y) { return seen(ideal, x, y); });
        ++counted;
    }
    rawError /= counted;
    fixedError /= counted;
    INFO("mean error shaken " << rawError << ", stabilized " << fixedError);
    CHECK(rawError > 0.03); // the picture really was shaken
    CHECK(fixedError < 0.15 * rawError);
}

TEST_CASE("Gyro axes: a turn to the right moves the picture's content to the left", "[gyro][rotation]") {
    // Constant 1 rad/s about +y for 1 s: the camera turns right. The raw frame at t = 0.5 s shows the content f * tan(0.5)
    // pixels to the left; with no smoothing the compensation is the identity, with a long one it is the rotation back.
    GyroTrack track;
    for (int i = 0; i <= 400; ++i) {
        track.times.push_back(0.005 * i - 0.5);
        track.omega.push_back({0.0, 1.0, 0.0});
    }
    const Quat q = orientationAt(track, std::array<double, 1>{0.5})[0];
    const auto r = toMatrix(q);
    // The ray through the image centre, in the world: the content at the centre of the first frame (looking along z) is
    // at angle -1.0 from the camera now: its pixel is cx - f * tan(1.0) (to the left).
    const double cx = (kW - 1) / 2.0;
    const double worldX = 0.0, worldZ = 1.0;
    const double camX = r[0] * worldX + r[3] * 0.0 + r[6] * worldZ; // R^T * world = camera coordinates
    const double camZ = r[2] * worldX + r[5] * 0.0 + r[8] * worldZ;
    const double pixelX = cx + kFocal * camX / camZ;
    CHECK(pixelX < cx - 100.0);
    CHECK(pixelX == Approx(cx - kFocal * std::tan(1.0)).margin(1e-6));
}

TEST_CASE("Smoothness 0 leaves every frame exactly as shot; keyed Smoothness steadies only the part it is raised in", "[gyro][stabilize]") {
    const GyroTrack track = gyroOfShake(-0.5, 3.5, 400.0);
    const auto times = frameTimes(75);
    SmoothOptions off = stabilizeOnly();
    off.smoothness = 0.0;
    const Compensation none = stabilizeFromGyro(track, times, plainSetup(), off, kW, kH);
    REQUIRE(none.corners.size() == 75);
    for (const auto& corners : none.corners) {
        for (const double v : corners) {
            CHECK(v == Approx(0.0).margin(1e-9));
        }
    }
    SmoothOptions keyed = stabilizeOnly();
    keyed.smoothnessAt.assign(75, 0.0);
    for (std::size_t i = 38; i < 75; ++i) {
        keyed.smoothnessAt[i] = 0.5;
    }
    const Compensation half = stabilizeFromGyro(track, times, plainSetup(), keyed, kW, kH);
    REQUIRE(half.corners.size() == 75);
    double first = 0.0, second = 0.0;
    for (std::size_t i = 0; i < 75; ++i) {
        double size = 0.0;
        for (const double v : half.corners[i]) {
            size += std::abs(v);
        }
        (i < 38 ? first : second) += size;
    }
    CHECK(first == Approx(0.0).margin(1e-9)); // untouched where Smoothness is 0
    CHECK(second > 10.0);                      // corrected where it is raised
}

TEST_CASE("No Motion locks the camera to the first frame; frames beyond the recorded gyro are left as shot", "[gyro][stabilize]") {
    const GyroTrack track = gyroOfShake(-0.5, 2.0, 400.0); // the gyro ends at 2.0 s; the clip runs to 3.0 s
    const auto times = frameTimes(75);
    SmoothOptions lock = stabilizeOnly();
    lock.noMotion = true;
    const Compensation c = stabilizeFromGyro(track, times, plainSetup(), lock, kW, kH);
    REQUIRE(c.corners.size() == 75);
    // Frame 20 (0.8 s): the compensated picture is the first frame's view.
    const Quat shaken = orientationOf(shakeAngles(20.0 / kFps));
    const Homography h = compensationAt(c, 20, kW, kH);
    const Quat first = orientationOf(shakeAngles(0.0));
    const double fixed = meanDifference([&](double x, double y) { return compensated(shaken, h, x, y); }, [&](double x, double y) { return seen(first, x, y); });
    const double raw = meanDifference([&](double x, double y) { return seen(shaken, x, y); }, [&](double x, double y) { return seen(first, x, y); });
    INFO("raw " << raw << " locked " << fixed);
    CHECK(fixed < 0.1 * raw);
    // Past 2.25 s (the gyro's end plus a quarter second) the frames are not covered: identity.
    for (std::size_t i = 60; i < 75; ++i) {
        for (const double v : c.corners[i]) {
            CHECK(v == Approx(0.0).margin(1e-9));
        }
    }
}

TEST_CASE("A rolling-shutter camera's skew comes out: a fast pan skews the lines by the read-out's share of the motion", "[gyro][stabilize]") {
    GyroTrack track;
    for (int i = 0; i <= 800; ++i) {
        track.times.push_back(0.005 * i - 1.0);
        track.omega.push_back({0.0, 1.0, 0.0}); // 1 rad/s to the right: the content moves f / fps = 6.4 px per frame to the left
    }
    GyroSetup setup = plainSetup();
    setup.readoutSeconds = 0.02; // 20 ms of a 40 ms frame: the scan takes half of it
    SmoothOptions off = stabilizeOnly();
    off.smoothness = 0.0; // only the skew
    const auto times = frameTimes(25);
    const Compensation c = stabilizeFromGyro(track, times, setup, off, kW, kH);
    REQUIRE(c.corners.size() == 25);
    const auto& k = c.corners[12];
    // The last line was read 3.2 px of content motion after the first: the compensation moves the bottom corners that far right
    // of the top ones.
    CHECK(k[6] - k[0] == Approx(3.2).margin(0.35));   // bl.x - tl.x
    CHECK(k[4] - k[2] == Approx(3.2).margin(0.35));   // br.x - tr.x
    setup.readoutSeconds = 0.0;
    const Compensation global = stabilizeFromGyro(track, times, setup, off, kW, kH);
    for (const double v : global.corners[12]) {
        CHECK(v == Approx(0.0).margin(1e-9)); // a global shutter has no skew to take out
    }
}

// ---- Auto-sync ---------------------------------------------------------------------------------------------------------------

namespace {

/// The picture's motion between frames of the shake, as an image analysis would report it: previous picture -> this one.
ImageMotionWindow imageWindow(int first, int count) {
    ImageMotionWindow w;
    w.motion.width = kW;
    w.motion.height = kH;
    w.motion.method = Method::Similarity;
    const double cx = (kW - 1) / 2.0, cy = (kH - 1) / 2.0;
    Homography k;
    k.m = {kFocal, 0, cx, 0, kFocal, cy, 0, 0, 1};
    const Homography kInverse = *k.inverted();
    for (int i = 0; i < count; ++i) {
        const double t = static_cast<double>(first + i) / kFps;
        w.times.push_back(framesToTicks(first + i, kRate25));
        FrameMotion f;
        f.time = w.times.back();
        f.confidence = 1.0;
        if (i > 0) {
            const auto prev = toMatrix(orientationOf(shakeAngles(static_cast<double>(first + i - 1) / kFps)));
            const auto cur = toMatrix(orientationOf(shakeAngles(t)));
            // d_cur = R_cur^T R_prev d_prev
            std::array<double, 9> rel{};
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    for (int m = 0; m < 3; ++m) {
                        rel[static_cast<std::size_t>(3 * r + c)] += cur[static_cast<std::size_t>(3 * m + r)] * prev[static_cast<std::size_t>(3 * m + c)];
                    }
                }
            }
            // What the picture-based analysis reports (a similarity about the centre): the centre's displacement and the roll,
            // clockwise on screen.
            Homography rotation;
            rotation.m = rel;
            const masks::Point2 moved = k.after(rotation.after(kInverse)).map({cx, cy});
            const double roll = std::atan2(0.5 * (rel[3] - rel[1]), 0.5 * (rel[0] + rel[4]));
            const double a = std::cos(roll), d = std::sin(roll);
            f.motion.m = {a, -d, cx + (moved.x - cx) - (a * cx - d * cy), d, a, cy + (moved.y - cy) - (d * cx + a * cy), 0.0, 0.0, 1.0};
        }
        w.motion.frames.push_back(f);
    }
    return w;
}

} // namespace

TEST_CASE("Auto-sync finds the gyro's offset and the lens's field of view from the picture's own motion", "[gyro][stabilize][sync]") {
    const GyroTrack track = gyroOfShake(-1.0, 4.0, 400.0, 0.012); // the gyro's clock runs 12 ms behind the picture's
    const std::vector<ImageMotionWindow> windows = {imageWindow(0, 48), imageWindow(50, 48)}; // 0..1.9 s and 2.0..3.9 s
    const GyroFit fit = fitGyroToPictures(track, windows, kW);
    INFO(fit.note);
    REQUIRE(fit.measured);
    CHECK(fit.usable);
    CHECK(fit.match > 0.9);
    CHECK(fit.axes == 0);
    CHECK(fit.offsetSeconds == Approx(0.012).margin(0.002));
    CHECK(fit.horizontalFovDegrees == Approx(90.0).epsilon(0.04));
    // The numbers reach the setup: the user's offset is added, a set field of view wins over the picture's.
    GyroOptions options;
    options.offsetSeconds = 0.003;
    const GyroSetup setup = resolveGyroSetup(track, options, fit, kW);
    CHECK(setup.synced);
    CHECK(setup.offsetSeconds == Approx(0.015).margin(0.002));
    CHECK(setup.fovFrom == "picture");
    options.horizontalFovDegrees = 70.0;
    CHECK(resolveGyroSetup(track, options, fit, kW).fovFrom == "set");
    options.autoSync = false;
    options.horizontalFovDegrees = 0.0;
    const GyroSetup manual = resolveGyroSetup(track, options, fit, kW);
    CHECK_FALSE(manual.synced);
    CHECK(manual.offsetSeconds == Approx(0.003));
    CHECK(manual.fovFrom == "camera"); // no lens data in this track: the camera's usual
}

TEST_CASE("Auto-sync recognises gyro axes that are turned from the table and reports a picture that does not move", "[gyro][stabilize][sync]") {
    const std::vector<ImageMotionWindow> windows = {imageWindow(0, 48), imageWindow(50, 48)};
    // The camera's table was wrong: the readings arrive with their axes turned by turn 11.
    const GyroTrack wrong = gyroOfShake(-1.0, 4.0, 400.0, 0.0, 11);
    const GyroFit fit = fitGyroToPictures(wrong, windows, kW);
    INFO(fit.note);
    REQUIRE(fit.usable);
    CHECK(fit.axes != 0);
    for (const Vec3 v : {Vec3{1, 2, 3}, Vec3{-4, 5, 6}}) {
        const Vec3 back = applyGyroAxes(fit.axes, applyGyroAxes(11, v));
        CHECK(back.x == Approx(v.x));
        CHECK(back.y == Approx(v.y));
        CHECK(back.z == Approx(v.z));
    }
    // A picture that does not move gives nothing to compare with.
    ImageMotionWindow still = imageWindow(0, 48);
    for (FrameMotion& f : still.motion.frames) {
        f.motion = Homography{};
    }
    const GyroFit nothing = fitGyroToPictures(gyroOfShake(-1.0, 4.0, 400.0), std::vector<ImageMotionWindow>{still}, kW);
    CHECK_FALSE(nothing.measured);
    CHECK_FALSE(nothing.usable);
    CHECK(nothing.note.find("hardly moves") != std::string::npos);
    // And a gyro that is unrelated to the picture does not pass for a match.
    GyroTrack unrelated = gyroOfShake(-1.0, 4.0, 400.0);
    std::uint32_t seed = 5;
    for (Vec3& w : unrelated.omega) {
        seed = seed * 1664525u + 1013904223u;
        w = {static_cast<double>(seed >> 20) / 2048.0 - 1.0, static_cast<double>((seed >> 8) & 0xFFF) / 2048.0 - 1.0, 0.0};
    }
    const GyroFit random = fitGyroToPictures(unrelated, windows, kW);
    CHECK_FALSE(random.usable);
}

TEST_CASE("The 24 axis turns are proper rotations and the first leaves the readings as recorded", "[gyro]") {
    const Vec3 v{1.0, 2.0, 3.0};
    CHECK(applyGyroAxes(0, v) == v);
    CHECK(describeGyroAxes(0) == "as recorded");
    std::vector<std::array<double, 3>> seen;
    for (int i = 0; i < kGyroAxisCount; ++i) {
        const Vec3 a = applyGyroAxes(i, {1, 0, 0});
        const Vec3 b = applyGyroAxes(i, {0, 1, 0});
        const Vec3 c = applyGyroAxes(i, {0, 0, 1});
        // Right-handed: a x b = c.
        CHECK(a.y * b.z - a.z * b.y == Approx(c.x));
        CHECK(a.z * b.x - a.x * b.z == Approx(c.y));
        CHECK(a.x * b.y - a.y * b.x == Approx(c.z));
        const Vec3 t = applyGyroAxes(i, v);
        for (const auto& other : seen) {
            CHECK_FALSE((other[0] == t.x && other[1] == t.y && other[2] == t.z));
        }
        seen.push_back({t.x, t.y, t.z});
    }
}
