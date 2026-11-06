// GoPro GPMF telemetry: the 'gpmd' track of GoPro MP4 files (Gyro.h).
//
// Format: https://github.com/gopro/gpmf-parser (README, "GPMF Deeper Dive"; Apache-2.0 or MIT). All values are big
// endian. A KLV item is a four character key, a type character, a one byte sample size, a two byte repeat count and
// size * repeat bytes padded to four; type 0 nests more items. A packet is one DEVC (a device) holding STRM (stream)
// items; a gyroscope stream holds SCAL (the divisor to the unit in SIUN, rad/s), ORIN / ORIO / MTRX (the sensor's
// axis order), STMP (the camera clock in microseconds at the stream's first reading, HERO8 and later), TSMP and the
// GYRO readings (three values per sample, 'GYRO'); the accelerometer is the same with 'ACCL'.
//
// Axes: ORIN names what each column of a reading is ("zxY": column 0 is the camera's -Z, column 1 -X, column 2 +Y;
// lower case is negative); streams without it use the order the README lists per camera (HERO5 Z,X,Y; HERO6 and later
// Y,-X,Z; Fusion -Y,X,Z). The camera frame this library hands out follows from the footage: GoPro samples
// (HERO5, 6, 7 and 8 from the gpmf-parser repository) tracked in the picture give the picture's content moving by
// +f * Z, -f * X (pixels per radian and second) and rolling by +Y, i.e. omega = (-X, -Z, -Y) in x right, y down, z out.

#include <wabcut/gyro/Gyro.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <numbers>

namespace wabcut::gyro {

namespace {

constexpr std::uint32_t fourcc(const char (&s)[5]) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(s[0])) << 24) | (static_cast<std::uint32_t>(static_cast<unsigned char>(s[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[2])) << 8) | static_cast<std::uint32_t>(static_cast<unsigned char>(s[3]));
}

struct Klv {
    std::uint32_t key = 0;
    char type = 0;
    std::size_t size = 0;   ///< bytes per sample
    std::size_t repeat = 0; ///< samples
    std::span<const std::uint8_t> data; ///< size * repeat bytes (without the padding)
    std::size_t next = 0;               ///< offset of the item after this one
};

std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
std::uint64_t be64(const std::uint8_t* p) { return (static_cast<std::uint64_t>(be32(p)) << 32) | be32(p + 4); }

/// The item at `pos` of `buf`; false when it is cut off by the end of `buf`.
bool readKlv(std::span<const std::uint8_t> buf, std::size_t pos, Klv& out) {
    if (pos + 8 > buf.size()) {
        return false;
    }
    out.key = be32(buf.data() + pos);
    out.type = static_cast<char>(buf[pos + 4]);
    out.size = buf[pos + 5];
    out.repeat = (static_cast<std::size_t>(buf[pos + 6]) << 8) | buf[pos + 7];
    const std::size_t bytes = out.size * out.repeat;
    const std::size_t start = pos + 8;
    if (bytes > buf.size() - start) {
        return false;
    }
    out.data = buf.subspan(start, bytes);
    out.next = start + ((bytes + 3) & ~static_cast<std::size_t>(3));
    return true;
}

std::size_t elementSize(char type) {
    switch (type) {
    case 'b': case 'B': case 'c': case 'u': return 1;
    case 's': case 'S': return 2;
    case 'l': case 'L': case 'f': case 'q': case 'F': return 4;
    case 'j': case 'J': case 'd': case 'Q': return 8;
    default: return 0;
    }
}

/// Element `i` of `data` as a number (types b B s S l L j J f d q Q).
double numberAt(char type, const std::uint8_t* p) {
    switch (type) {
    case 'b': return static_cast<std::int8_t>(p[0]);
    case 'B': return p[0];
    case 's': return static_cast<std::int16_t>((p[0] << 8) | p[1]);
    case 'S': return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
    case 'l': return static_cast<std::int32_t>(be32(p));
    case 'L': return be32(p);
    case 'j': return static_cast<double>(static_cast<std::int64_t>(be64(p)));
    case 'J': return static_cast<double>(be64(p));
    case 'q': return static_cast<std::int32_t>(be32(p)) / 65536.0;
    case 'Q': return static_cast<double>(static_cast<std::int64_t>(be64(p))) / 4294967296.0;
    case 'f': {
        const std::uint32_t bits = be32(p);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    case 'd': {
        const std::uint64_t bits = be64(p);
        double d;
        std::memcpy(&d, &bits, 8);
        return d;
    }
    default: return 0.0;
    }
}

std::vector<double> numbers(const Klv& k) {
    std::vector<double> out;
    const std::size_t e = elementSize(k.type);
    if (e == 0 || k.type == 'c' || k.type == 'u' || k.type == 'F') {
        return out;
    }
    out.reserve(k.data.size() / e);
    for (std::size_t i = 0; i + e <= k.data.size(); i += e) {
        out.push_back(numberAt(k.type, k.data.data() + i));
    }
    return out;
}

/// A string of a KLV item (a unit, an axis order, a camera model): printable ASCII up to the first NUL, at most 64 bytes.
/// The text travels on into a project (the effect's data), a JSON file that holds valid UTF-8 only, so the bytes of a
/// damaged or foreign file never get there.
std::string text(const Klv& k) {
    std::string s;
    for (const std::uint8_t byte : k.data) {
        if (byte == 0 || s.size() >= 64) {
            break;
        }
        if (byte >= 0x20 && byte < 0x7F) {
            s.push_back(static_cast<char>(byte));
        }
    }
    while (!s.empty() && s.back() == ' ') {
        s.pop_back();
    }
    return s;
}

/// The readings of an IMU stream: three values per sample, scaled by SCAL.
struct Stream {
    std::vector<Vec3> samples; ///< raw columns, divided by SCAL
    std::string unit;          ///< SIUN / UNIT ("rad/s", "m/s²" as bytes), lower-cased ASCII part
    std::string orin;          ///< ORIN: what each column is (ORIO and MTRX restate it as a matrix and are not needed)
    std::optional<double> stampSeconds; ///< STMP, the camera clock at the first reading
};

bool validOrientation(const std::string& s) {
    if (s.size() != 3) {
        return false;
    }
    bool seen[3] = {false, false, false};
    for (const char c : s) {
        const char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (u < 'X' || u > 'Z' || seen[u - 'X']) {
            return false;
        }
        seen[u - 'X'] = true;
    }
    return true;
}

/// Reads the GYRO or ACCL item `item` of a STRM whose other items were collected in `scal`, `stream`.
void readImu(const Klv& item, const std::vector<double>& scal, Stream& stream, std::size_t limit) {
    const std::size_t e = elementSize(item.type);
    if (e == 0 || item.type == 'c' || item.size != 3 * e) {
        return; // not three numbers per sample
    }
    double divisor[3] = {1.0, 1.0, 1.0};
    if (scal.size() == 1 && scal[0] != 0.0) {
        divisor[0] = divisor[1] = divisor[2] = scal[0];
    } else if (scal.size() == 3) {
        for (int i = 0; i < 3; ++i) {
            divisor[i] = scal[static_cast<std::size_t>(i)] != 0.0 ? scal[static_cast<std::size_t>(i)] : 1.0;
        }
    }
    const std::size_t room = limit > stream.samples.size() ? limit - stream.samples.size() : 0;
    const std::size_t count = std::min(item.repeat, room);
    stream.samples.reserve(stream.samples.size() + count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* p = item.data.data() + i * item.size;
        stream.samples.push_back({numberAt(item.type, p) / divisor[0], numberAt(item.type, p + e) / divisor[1], numberAt(item.type, p + 2 * e) / divisor[2]});
    }
}

/// What one packet gave: the streams of one device.
struct DeviceBlock {
    std::string deviceName;
    std::optional<Stream> gyro;
    std::optional<Stream> accel;
    std::optional<double> readout; ///< SROT, seconds
};

void parseStream(std::span<const std::uint8_t> buf, std::size_t limit, DeviceBlock& device) {
    std::vector<double> scal;
    Stream stream;
    bool isGyro = false;
    bool isAccel = false;
    std::optional<Klv> data;
    std::size_t pos = 0;
    Klv k;
    while (readKlv(buf, pos, k)) {
        pos = k.next;
        switch (k.key) {
        case fourcc("SCAL"): scal = numbers(k); break;
        case fourcc("SIUN"):
        case fourcc("UNIT"): stream.unit = text(k); break;
        case fourcc("ORIN"): stream.orin = text(k); break;
        case fourcc("STMP"): {
            const auto v = numbers(k);
            if (!v.empty() && v[0] >= 0.0) {
                stream.stampSeconds = v[0] * 1e-6;
            }
            break;
        }
        case fourcc("SROT"): {
            const auto v = numbers(k);
            // Sensor read-out time, milliseconds (README, "Header metadata"); a reading outside 0.1 ms .. 200 ms is not one.
            if (!v.empty() && v[0] >= 0.1 && v[0] <= 200.0) {
                device.readout = v[0] * 1e-3;
            }
            break;
        }
        case fourcc("GYRO"): isGyro = true; data = k; break;
        case fourcc("ACCL"): isAccel = true; data = k; break;
        default: break;
        }
    }
    if (!data || (!isGyro && !isAccel)) {
        return;
    }
    readImu(*data, scal, stream, limit);
    if (stream.samples.empty()) {
        return;
    }
    (isGyro ? device.gyro : device.accel) = std::move(stream);
}

void parseDevice(std::span<const std::uint8_t> buf, std::size_t limit, int depth, DeviceBlock& device) {
    if (depth > kMaxNesting) {
        return;
    }
    std::size_t pos = 0;
    Klv k;
    while (readKlv(buf, pos, k)) {
        pos = k.next;
        if (k.key == fourcc("DVNM")) {
            device.deviceName = text(k);
        } else if (k.key == fourcc("STRM") && k.type == 0) {
            parseStream(k.data, limit, device);
        }
    }
}

/// The packet's devices (a GoPro with a Karma or a Bluetooth sensor has several; the first with a gyro wins).
std::vector<DeviceBlock> parsePacket(std::span<const std::uint8_t> buf, std::size_t limit) {
    std::vector<DeviceBlock> out;
    std::size_t pos = 0;
    Klv k;
    while (out.size() < 8 && readKlv(buf, pos, k)) {
        pos = k.next;
        if (k.key == fourcc("DEVC") && k.type == 0) {
            DeviceBlock device;
            parseDevice(k.data, limit, 1, device);
            out.push_back(std::move(device));
        }
    }
    return out;
}

std::string upper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

/// What the columns of a reading are, for a stream: ORIN when it has one, else the order the README gives for the camera.
std::string columnOrder(const Stream& s, const std::string& deviceName) {
    if (validOrientation(s.orin)) {
        return s.orin;
    }
    const std::string name = upper(deviceName);
    if (name.find("FUSION") != std::string::npos) {
        return "yXZ"; // README: Fusion, data order -Y,X,Z
    }
    if (name.find("HERO6") != std::string::npos || name.find("HERO7") != std::string::npos || name.find("HERO 6") != std::string::npos ||
        name.find("HERO 7") != std::string::npos) {
        return "YxZ"; // README: HERO6 and later, data order Y,-X,Z
    }
    return "ZXY"; // README: HERO5 Black and Session, data order Z,X,Y (also the order of a camera the file does not name)
}

/// A reading's columns -> the camera frame (x right, y down, z out of the lens).
Vec3 toCameraFrame(const Vec3& column, const std::string& order) {
    double axis[3] = {0.0, 0.0, 0.0}; // the camera's X, Y, Z as GoPro labels them
    const double c[3] = {column.x, column.y, column.z};
    for (std::size_t k = 0; k < 3; ++k) {
        const char letter = order[k];
        const double sign = std::islower(static_cast<unsigned char>(letter)) ? -1.0 : 1.0;
        axis[std::toupper(static_cast<unsigned char>(letter)) - 'X'] = sign * c[k];
    }
    return {-axis[0], -axis[2], -axis[1]};
}

struct Timed {
    double pts = 0.0;
    double duration = 0.0;
    Stream stream;
};

/// Gives every reading of `blocks` a time on the file's clock: by the camera clock (STMP) when every block has it,
/// else by the container's: a block's readings are spread evenly over the packet.
void assignTimes(const std::vector<Timed>& blocks, const std::string& deviceName, bool gyro, std::vector<double>& times,
                 std::vector<Vec3>& values) {
    if (blocks.empty()) {
        return;
    }
    const bool stamped = std::all_of(blocks.begin(), blocks.end(), [](const Timed& b) { return b.stream.stampSeconds.has_value(); });
    double meanStep = 0.0; // seconds per reading over the whole track, for blocks with no better spacing
    {
        std::size_t readings = 0;
        double span = 0.0;
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            const double s = stamped && i + 1 < blocks.size() ? *blocks[i + 1].stream.stampSeconds - *blocks[i].stream.stampSeconds : blocks[i].duration;
            if ((stamped && i + 1 < blocks.size()) || (!stamped && blocks[i].duration > 0.0)) {
                span += std::max(s, 0.0);
                readings += blocks[i].stream.samples.size();
            }
        }
        meanStep = readings > 0 && span > 0.0 ? span / static_cast<double>(readings) : 0.0;
    }
    double last = -1e300;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const Timed& b = blocks[i];
        const std::size_t n = b.stream.samples.size();
        if (n == 0) {
            continue;
        }
        double start = b.pts;
        double step = meanStep;
        if (stamped) {
            start = blocks[0].pts + (*b.stream.stampSeconds - *blocks[0].stream.stampSeconds);
            if (i + 1 < blocks.size()) {
                step = (*blocks[i + 1].stream.stampSeconds - *b.stream.stampSeconds) / static_cast<double>(n);
            }
        } else if (b.duration > 0.0) {
            step = b.duration / static_cast<double>(n);
        }
        if (!(step > 0.0) || !std::isfinite(step)) {
            step = meanStep > 0.0 ? meanStep : 1.0 / 200.0;
        }
        const std::string order = columnOrder(b.stream, deviceName);
        const double unit = b.stream.unit.rfind("deg", 0) == 0 ? std::numbers::pi / 180.0 : 1.0;
        for (std::size_t j = 0; j < n; ++j) {
            double t = start + step * static_cast<double>(j);
            if (!(t > last)) {
                t = last + 1e-6; // times strictly increase (overlapping packets, a repeated stamp)
            }
            last = t;
            times.push_back(t);
            Vec3 v = toCameraFrame(b.stream.samples[j], order);
            if (gyro) {
                v = {v.x * unit, v.y * unit, v.z * unit};
            }
            values.push_back(v);
        }
    }
}

} // namespace

struct GpmfReader::Impl {
    std::vector<Timed> gyro;
    std::vector<Timed> accel;
    std::string deviceName;
    std::optional<double> readout;
    std::size_t readings = 0;
};

GpmfReader::GpmfReader() : m(std::make_unique<Impl>()) {}
GpmfReader::~GpmfReader() = default;

void GpmfReader::add(const Packet& packet) {
    if (packet.bytes.size() > kMaxPacketBytes || m->readings >= kMaxGyroReadings) {
        return;
    }
    for (DeviceBlock& device : parsePacket(packet.bytes, kMaxGyroReadings - m->readings)) {
        if (!device.gyro) {
            continue; // a connected sensor without a gyroscope
        }
        if (m->deviceName.empty()) {
            m->deviceName = device.deviceName;
        }
        if (device.deviceName != m->deviceName) {
            continue; // one device's readings only
        }
        m->readout = m->readout ? m->readout : device.readout;
        m->readings += device.gyro->samples.size();
        m->gyro.push_back({packet.seconds, packet.duration, std::move(*device.gyro)});
        if (device.accel) {
            m->accel.push_back({packet.seconds, packet.duration, std::move(*device.accel)});
        }
        break;
    }
}

std::expected<GyroTrack, Error> GpmfReader::finish() {
    const std::vector<Timed>& gyro = m->gyro;
    const std::vector<Timed>& accel = m->accel;
    const std::string& deviceName = m->deviceName;
    const std::optional<double>& readout = m->readout;
    if (gyro.empty()) {
        return std::unexpected(Error{ErrorCode::ParseError, "the GoPro metadata track holds no gyroscope readings (a camera older "
                                                           "than the HERO5, or the camera's gyro setting was off)"});
    }
    GyroTrack track;
    track.camera = Camera::GoPro;
    track.make = "GoPro";
    track.model = deviceName == "Camera" ? std::string() : deviceName; // the HERO5's DVNM says only "Camera"
    track.format = "GoPro GPMF";
    track.axesChecked = true;
    track.readoutSeconds = readout;
    assignTimes(gyro, deviceName, true, track.times, track.omega);
    assignTimes(accel, deviceName, false, track.accelTimes, track.accel);
    if (track.times.size() >= 2 && track.times.back() > track.times.front()) {
        track.rateHz = static_cast<double>(track.times.size() - 1) / (track.times.back() - track.times.front());
    }
    return track;
}

std::expected<GyroTrack, Error> parseGpmf(std::span<const Packet> packets) {
    GpmfReader reader;
    for (const Packet& p : packets) {
        reader.add(p);
    }
    return reader.finish();
}

} // namespace wabcut::gyro
