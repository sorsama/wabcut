// Sony 'rtmd' real-time metadata: the data track of XAVC S / HS / S-I MP4 files from Alpha, FX, ZV and RX cameras that
// carry a gyroscope (Gyro.h). One packet per video frame.
//
// Sony does not publish the layout. It is the one Gyroflow's open-source parser (MIT or Apache-2.0) documents:
// https://github.com/AdrianEddy/telemetry-parser, src/sony/mod.rs (parse_metadata, normalize_imu_orientation) and
// src/sony/rtmd_tags.rs (tag numbers), on top of SMPTE RDD 18's camera and lens sets.
//   * A packet starts with a header 00 1C ... of 0x1C bytes, then tag-length-value items: a 16-bit tag, a 16-bit length
//     and `length` bytes, big endian. Tag 0x060E starts a 16-byte SMPTE label (skipped); tag 0x8300 is a container whose
//     items follow inside its length.
//   * Gyroscope: 0xE43B readings (int32 count, int32 element size = 6, then count x three int16), 0xE438 the unit (0
//     deg/s, 1 rad/s), 0xE439 the scale (readings per unit, float32), 0xE43A the axis order (three 4-bit codes, the
//     lowest first: 0 X, 1 x, 2 Y, 3 y, 4 Z, 5 z; lower case is negative). Accelerometer: the same with 0xE44B, 0xE448,
//     0xE449, 0xE44A (unit 0 = g).
//   * Frame timing and lens: 0xE40E the sensor's readout time and 0xE40D the exposure (microseconds, int32), 0xE405 the
//     sensor size in pixels (2 x uint16), 0xE407 the pixel pitch (2 x int16 in 1/0xE406 m, nm by default), 0xE40A the
//     capture area's size (2 x uint32 in 1/0xE408 pixels), 0xE421 the lens table whose first field is the focal length
//     in nm, 0x8005 the lens's focal length ("Half Float (lens serial)" of RDD 18: a 12-bit mantissa and a decimal
//     exponent in metres).
// The readings of a packet are spread evenly over the frame the packet belongs to.
//
// Axes: the camera frame follows Gyroflow's normalisation of Sony's order (first two codes swapped, the third's sign
// flipped) read the way GoPro's orientation strings are, whose result was checked against footage (Gpmf.cpp). Sony's
// mapping is the documented one and NOT checked against footage here; Auto-sync compares it with the picture.

#include <wabcut/gyro/Gyro.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <numbers>

namespace wabcut::gyro {

namespace {

std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
std::int16_t s16(const std::uint8_t* p) { return static_cast<std::int16_t>(be16(p)); }
std::int32_t s32(const std::uint8_t* p) { return static_cast<std::int32_t>(be32(p)); }

constexpr std::size_t kHeader = 0x1C;

/// One IMU sensor's readings in a packet with the settings that were in force.
struct Imu {
    std::vector<Vec3> raw;
    bool radians = false;       ///< e438 / e448: rad/s (gyro) or m/s^2 (accel); else deg/s or g
    double scale = 0.0;         ///< readings per unit; 0 = unknown
    std::string orientation;    ///< three letters, lower case negative
};

struct Frame {
    double seconds = 0.0;
    double duration = 0.0;
    Imu gyro;
    Imu accel;
};

struct Lens {
    std::optional<double> readoutMicros;
    std::optional<double> exposureMicros;
    std::optional<std::pair<unsigned, unsigned>> sensorPixels;
    std::optional<std::pair<double, double>> pitchUnits; ///< pixel pitch x, y in 1/pitchUnit metres
    std::optional<double> pitchUnit;
    std::optional<std::pair<double, double>> cropSize;   ///< capture area, in 1/cropUnit pixels
    std::optional<double> cropUnit;
    std::optional<double> focalMetres;
};

/// RDD 18's lens-serial "half float": a 12-bit mantissa and a 4-bit decimal exponent (two's complement), metres.
double rdd18Distance(std::uint16_t v) {
    int exponent = (v >> 12) & 0x0F;
    if (exponent >= 8) {
        exponent -= 16;
    }
    return static_cast<double>(v & 0x0FFF) * std::pow(10.0, exponent);
}

std::string orientationOf(std::uint16_t code) {
    std::string out;
    for (int shift = 0; shift <= 8; shift += 4) {
        switch ((code >> shift) & 0x0F) {
        case 0: out += 'X'; break;
        case 1: out += 'x'; break;
        case 2: out += 'Y'; break;
        case 3: out += 'y'; break;
        case 4: out += 'Z'; break;
        case 5: out += 'z'; break;
        default: return {};
        }
    }
    return out;
}

char invertCase(char c) {
    return std::islower(static_cast<unsigned char>(c)) ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                                                       : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

/// Sony's raw order -> the camera frame, as the header comment says.
Vec3 toCameraFrame(const Vec3& v, std::string order) {
    if (order.size() != 3) {
        order = "XYZ";
    }
    std::swap(order[0], order[1]);
    order[2] = invertCase(order[2]);
    const double c[3] = {v.x, v.y, v.z};
    double out[3];
    for (std::size_t k = 0; k < 3; ++k) {
        const double sign = std::islower(static_cast<unsigned char>(order[k])) ? -1.0 : 1.0;
        out[k] = sign * c[std::toupper(static_cast<unsigned char>(order[k])) - 'X'];
    }
    return {-out[1], -out[0], -out[2]};
}

/// The readings of tag 0xE43B / 0xE44B: int32 count, int32 element size (6), count x (int16 x, y, z).
bool readImu(std::span<const std::uint8_t> data, std::size_t room, std::vector<Vec3>& out) {
    if (data.size() < 8) {
        return false;
    }
    const std::int32_t count = s32(data.data());
    const std::int32_t size = s32(data.data() + 4);
    if (size != 6 || count <= 0) {
        return false;
    }
    const std::size_t n = std::min({static_cast<std::size_t>(count), (data.size() - 8) / 6, room});
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* p = data.data() + 8 + i * 6;
        out.push_back({static_cast<double>(s16(p)), static_cast<double>(s16(p + 2)), static_cast<double>(s16(p + 4))});
    }
    return !out.empty();
}

bool detectRtmd(std::span<const std::uint8_t> bytes) { return bytes.size() > kHeader && bytes[0] == 0x00 && bytes[1] == kHeader; }

/// Walks one packet's items. `lens` collects the first value of each lens / imager item it meets.
void parsePacket(std::span<const std::uint8_t> bytes, std::size_t room, Frame& frame, Lens& lens) {
    std::size_t ends[kMaxNesting + 1];
    int depth = 0;
    ends[0] = bytes.size();
    std::size_t pos = kHeader;
    while (depth >= 0) {
        const std::size_t end = ends[depth];
        if (pos >= end) {
            pos = end;
            --depth;
            continue;
        }
        if (pos + 2 > end) {
            pos = end;
            continue;
        }
        const std::uint16_t tag = be16(bytes.data() + pos);
        if (tag == 0x060E) { // a 16-byte SMPTE label
            pos += 16;
            continue;
        }
        if (tag == 0x0000 || tag == 0xFFFF || pos + 4 > end) {
            pos = end; // padding to the end of the container
            continue;
        }
        const std::size_t length = be16(bytes.data() + pos + 2);
        pos += 4;
        if (length > end - pos) {
            pos = end; // damaged: the rest of this container is not trusted
            continue;
        }
        const auto data = bytes.subspan(pos, length);
        pos += length;
        if (tag == 0x8300) { // container
            if (depth < kMaxNesting) {
                ends[++depth] = pos;
                pos -= length;
            }
            continue;
        }
        switch (tag) {
        case 0xE438: if (length >= 1) { frame.gyro.radians = data[0] != 0; } break;
        case 0xE439: if (length >= 4) { frame.gyro.scale = static_cast<double>(std::bit_cast<float>(be32(data.data()))); } break;
        case 0xE43A: if (length >= 2) { frame.gyro.orientation = orientationOf(be16(data.data())); } break;
        case 0xE43B: readImu(data, room, frame.gyro.raw); break;
        case 0xE448: if (length >= 1) { frame.accel.radians = data[0] != 0; } break;
        case 0xE449: if (length >= 4) { frame.accel.scale = static_cast<double>(std::bit_cast<float>(be32(data.data()))); } break;
        case 0xE44A: if (length >= 2) { frame.accel.orientation = orientationOf(be16(data.data())); } break;
        case 0xE44B: readImu(data, room, frame.accel.raw); break;
        case 0xE40E: if (length >= 4 && !lens.readoutMicros) { lens.readoutMicros = s32(data.data()); } break;
        case 0xE40D: if (length >= 4 && !lens.exposureMicros) { lens.exposureMicros = s32(data.data()); } break;
        case 0xE405:
            if (length >= 4 && !lens.sensorPixels) {
                lens.sensorPixels = std::pair<unsigned, unsigned>{be16(data.data()), be16(data.data() + 2)};
            }
            break;
        case 0xE406: if (length >= 4 && !lens.pitchUnit) { lens.pitchUnit = s32(data.data()); } break;
        case 0xE407:
            if (length >= 4 && !lens.pitchUnits) {
                lens.pitchUnits = std::pair<double, double>{static_cast<double>(s16(data.data())), static_cast<double>(s16(data.data() + 2))};
            }
            break;
        case 0xE408: if (length >= 4 && !lens.cropUnit) { lens.cropUnit = s32(data.data()); } break;
        case 0xE40A:
            if (length >= 8 && !lens.cropSize) {
                lens.cropSize = std::pair<double, double>{static_cast<double>(be32(data.data())), static_cast<double>(be32(data.data() + 4))};
            }
            break;
        case 0xE421: // lens table: focal length in nm first
            if (length >= 4 && !lens.focalMetres && be32(data.data()) > 0) {
                lens.focalMetres = static_cast<double>(be32(data.data())) * 1e-9;
            }
            break;
        case 0x8005: // the lens's actual focal length
            if (length >= 2) {
                const double metres = rdd18Distance(be16(data.data()));
                if (metres > 0.0) {
                    lens.focalMetres = metres;
                }
            }
            break;
        default: break;
        }
    }
}

void fillTimes(const std::vector<Frame>& frames, bool gyro, std::vector<double>& times, std::vector<Vec3>& values) {
    double step = 0.0; // seconds per frame, for a packet that does not say its duration
    for (const Frame& f : frames) {
        if (f.duration > 0.0) {
            step = f.duration;
            break;
        }
    }
    double last = -1e300;
    for (const Frame& f : frames) {
        const Imu& imu = gyro ? f.gyro : f.accel;
        if (imu.raw.empty() || !(imu.scale > 0.0)) {
            continue;
        }
        const double duration = f.duration > 0.0 ? f.duration : step;
        const double dt = duration > 0.0 ? duration / static_cast<double>(imu.raw.size()) : 0.0;
        if (!(dt > 0.0)) {
            continue;
        }
        const double unit = gyro ? (imu.radians ? 1.0 : std::numbers::pi / 180.0) : (imu.radians ? 1.0 : 9.80665);
        for (std::size_t j = 0; j < imu.raw.size(); ++j) {
            double t = f.seconds + dt * static_cast<double>(j);
            if (!(t > last)) {
                t = last + 1e-6;
            }
            last = t;
            const Vec3 r = imu.raw[j];
            times.push_back(t);
            values.push_back(toCameraFrame({r.x / imu.scale * unit, r.y / imu.scale * unit, r.z / imu.scale * unit}, imu.orientation));
        }
    }
}

} // namespace

struct SonyRtmdReader::Impl {
    std::vector<Frame> frames;
    Lens lens;
    std::size_t readings = 0;
};

SonyRtmdReader::SonyRtmdReader() : m(std::make_unique<Impl>()) {}
SonyRtmdReader::~SonyRtmdReader() = default;

void SonyRtmdReader::add(const Packet& packet) {
    if (packet.bytes.size() > kMaxPacketBytes || m->readings >= kMaxGyroReadings || !detectRtmd(packet.bytes)) {
        return;
    }
    Frame frame;
    frame.seconds = packet.seconds;
    frame.duration = packet.duration;
    parsePacket(packet.bytes, kMaxGyroReadings - m->readings, frame, m->lens);
    // Settings are written in every packet, but keep the last known ones for a packet that omits them.
    if (!m->frames.empty()) {
        const Frame& before = m->frames.back();
        frame.gyro.scale = frame.gyro.scale > 0.0 ? frame.gyro.scale : before.gyro.scale;
        frame.gyro.orientation = frame.gyro.orientation.empty() ? before.gyro.orientation : frame.gyro.orientation;
        frame.accel.scale = frame.accel.scale > 0.0 ? frame.accel.scale : before.accel.scale;
        frame.accel.orientation = frame.accel.orientation.empty() ? before.accel.orientation : frame.accel.orientation;
    }
    m->readings += frame.gyro.raw.size();
    m->frames.push_back(std::move(frame));
}

std::expected<GyroTrack, Error> SonyRtmdReader::finish(std::string_view model) {
    const std::vector<Frame>& frames = m->frames;
    const Lens& lens = m->lens;
    GyroTrack track;
    track.camera = Camera::Sony;
    track.make = "Sony";
    track.model = std::string(model);
    track.format = "Sony rtmd";
    track.axesChecked = false;
    fillTimes(frames, true, track.times, track.omega);
    if (track.times.empty()) {
        return std::unexpected(Error{ErrorCode::ParseError, "the Sony metadata track holds no gyroscope readings (this camera does "
                                                           "not record a gyro, or it is switched off)"});
    }
    fillTimes(frames, false, track.accelTimes, track.accel);
    if (track.times.size() >= 2 && track.times.back() > track.times.front()) {
        track.rateHz = static_cast<double>(track.times.size() - 1) / (track.times.back() - track.times.front());
    }
    if (lens.readoutMicros && *lens.readoutMicros > 0.0 && *lens.readoutMicros < 200'000.0) {
        track.readoutSeconds = *lens.readoutMicros * 1e-6;
    }
    if (lens.exposureMicros && *lens.exposureMicros > 0.0 && *lens.exposureMicros < 10'000'000.0) {
        track.exposureSeconds = *lens.exposureMicros * 1e-6;
    }
    if (lens.focalMetres && *lens.focalMetres >= 1e-3 && *lens.focalMetres <= 2.0) {
        track.focalLengthMm = *lens.focalMetres * 1e3;
        // The sensor area the pictures use: the capture area (pixels) times the pixel pitch.
        const double pitchUnit = lens.pitchUnit && *lens.pitchUnit > 0.0 ? *lens.pitchUnit : 1e9;
        const double cropUnit = lens.cropUnit && *lens.cropUnit > 0.0 ? *lens.cropUnit : 1.0;
        double widthPixels = 0.0;
        if (lens.cropSize && lens.cropSize->first > 0.0) {
            widthPixels = lens.cropSize->first / cropUnit;
        } else if (lens.sensorPixels && lens.sensorPixels->first > 0) {
            widthPixels = static_cast<double>(lens.sensorPixels->first);
        }
        if (lens.pitchUnits && lens.pitchUnits->first > 0.0 && widthPixels > 0.0) {
            const double widthMm = widthPixels * (lens.pitchUnits->first / pitchUnit) * 1e3;
            if (widthMm >= 1.0 && widthMm <= 200.0) {
                track.imagedWidthMm = widthMm;
                const double hfov = 2.0 * std::atan(widthMm / (2.0 * *track.focalLengthMm)) * 180.0 / std::numbers::pi;
                if (hfov >= 5.0 && hfov <= 170.0) {
                    track.horizontalFovDegrees = hfov;
                }
            }
        }
    }
    return track;
}

std::expected<GyroTrack, Error> parseSonyRtmd(std::span<const Packet> packets, std::string_view model) {
    SonyRtmdReader reader;
    for (const Packet& p : packets) {
        reader.add(p);
    }
    return reader.finish(model);
}

} // namespace wabcut::gyro
