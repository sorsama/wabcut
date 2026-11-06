#include <wabcut/ai/video/ProcessedCache.h>

#include <wabcut/ai/ModelStore.h>

#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>

namespace wabcut::ai::video {

namespace {

using Json = nlohmann::json;

constexpr char kMagic[4] = {'W', 'P', 'F', '1'};
constexpr std::uint64_t kMaxRawBytes = std::uint64_t{1} << 30; ///< a decoded picture (8K 4:4:4 at 16 bits is 200 MB)
constexpr int kMaxSide = 16384;
constexpr int kMaxPlanes = 4;

std::string hex16(std::uint64_t v) {
    char buffer[17];
    std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(v));
    return buffer;
}

template <class T>
void put(std::vector<std::uint8_t>& out, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<std::uint8_t>((static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<T>>(value)) >> (8 * i)) & 0xFFu));
    }
}

class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t>& bytes) : m_bytes(bytes) {}
    template <class T>
    bool get(T& value) {
        if (m_at + sizeof(T) > m_bytes.size()) {
            return false;
        }
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            v |= static_cast<std::uint64_t>(m_bytes[m_at + i]) << (8 * i);
        }
        m_at += sizeof(T);
        value = static_cast<T>(static_cast<std::make_unsigned_t<T>>(v));
        return true;
    }
    std::size_t at() const { return m_at; }
    std::size_t left() const { return m_bytes.size() - m_at; }
    const std::uint8_t* data() const { return m_bytes.data() + m_at; }

private:
    const std::vector<std::uint8_t>& m_bytes;
    std::size_t m_at = 0;
};

/// Samples of 2 bytes compress better with their high bytes together (they change slowly) and their low bytes (noise) apart.
void shuffle(const std::vector<std::uint8_t>& in, int bytesPerTexel, std::vector<std::uint8_t>& out) {
    if (bytesPerTexel != 2) {
        out.insert(out.end(), in.begin(), in.end());
        return;
    }
    const std::size_t n = in.size() / 2;
    const std::size_t base = out.size();
    out.resize(base + in.size());
    for (std::size_t i = 0; i < n; ++i) {
        out[base + i] = in[2 * i + 1];
        out[base + n + i] = in[2 * i];
    }
}

void unshuffle(const std::uint8_t* in, std::size_t bytes, int bytesPerTexel, std::vector<std::uint8_t>& out) {
    out.resize(bytes);
    if (bytesPerTexel != 2) {
        std::memcpy(out.data(), in, bytes);
        return;
    }
    const std::size_t n = bytes / 2;
    for (std::size_t i = 0; i < n; ++i) {
        out[2 * i + 1] = in[i];
        out[2 * i] = in[n + i];
    }
}

std::optional<std::int64_t> parsePts(const std::string& stem) {
    std::int64_t value = 0;
    const auto [end, ec] = std::from_chars(stem.data(), stem.data() + stem.size(), value);
    if (ec != std::errc{} || end != stem.data() + stem.size()) {
        return std::nullopt;
    }
    return value;
}

} // namespace

std::string CacheIdentity::directoryName() const {
    // The model version is folded into the chain's hash: a new weight file or pipeline reads no old picture.
    std::uint64_t h = chain ^ 0x9E3779B97F4A7C15ull;
    for (const char c : modelVersion) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return hex16(media) + "-s" + std::to_string(stream) + "-" + hex16(h);
}

std::size_t StoredFrame::bytes() const {
    std::size_t n = sizeof(StoredFrame);
    for (const StoredPlane& p : planes) {
        n += p.data.size();
    }
    return n;
}

bool StoredFrame::valid() const {
    if (width <= 0 || height <= 0 || width > kMaxSide || height > kMaxSide || planes.empty() || planes.size() > kMaxPlanes) {
        return false;
    }
    for (const StoredPlane& p : planes) {
        if (p.width <= 0 || p.height <= 0 || p.width > kMaxSide || p.height > kMaxSide || (p.bytesPerTexel != 1 && p.bytesPerTexel != 2 && p.bytesPerTexel != 4) ||
            p.data.size() != static_cast<std::size_t>(p.width) * static_cast<std::size_t>(p.height) * static_cast<std::size_t>(p.bytesPerTexel)) {
            return false;
        }
    }
    return true;
}

std::expected<std::vector<std::uint8_t>, Error> encodeFrame(const StoredFrame& frame) {
    if (!frame.valid()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "not a valid processed picture"));
    }
    std::vector<std::uint8_t> out;
    out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
    put<std::uint32_t>(out, static_cast<std::uint32_t>(frame.width));
    put<std::uint32_t>(out, static_cast<std::uint32_t>(frame.height));
    put<std::int64_t>(out, frame.pts);
    put<std::int64_t>(out, frame.duration);
    put<std::int64_t>(out, frame.streamPts);
    put<std::int32_t>(out, frame.timeBaseNum);
    put<std::int32_t>(out, frame.timeBaseDen);
    put<std::int32_t>(out, frame.sampleNum);
    put<std::int32_t>(out, frame.sampleDen);
    for (const std::uint8_t v : {frame.format, frame.matrix, frame.range, frame.primaries, frame.transfer, frame.chroma, frame.alpha,
                                 static_cast<std::uint8_t>(frame.planes.size())}) {
        out.push_back(v);
    }
    std::vector<std::uint8_t> raw;
    for (const StoredPlane& p : frame.planes) {
        put<std::uint32_t>(out, static_cast<std::uint32_t>(p.width));
        put<std::uint32_t>(out, static_cast<std::uint32_t>(p.height));
        put<std::uint32_t>(out, static_cast<std::uint32_t>(p.bytesPerTexel));
        shuffle(p.data, p.bytesPerTexel, raw);
    }
    uLongf packedSize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::uint8_t> packed(packedSize);
    if (compress2(packed.data(), &packedSize, raw.data(), static_cast<uLong>(raw.size()), 1) != Z_OK) {
        return std::unexpected(Error(ErrorCode::IoError, "compressing the processed picture failed"));
    }
    put<std::uint64_t>(out, raw.size());
    put<std::uint64_t>(out, packedSize);
    out.insert(out.end(), packed.begin(), packed.begin() + static_cast<std::ptrdiff_t>(packedSize));
    return out;
}

std::expected<StoredFrame, Error> decodeFrame(const std::vector<std::uint8_t>& bytes) {
    const auto damaged = [] { return std::unexpected(Error(ErrorCode::ParseError, "a processed picture is damaged")); };
    if (bytes.size() < sizeof kMagic || std::memcmp(bytes.data(), kMagic, sizeof kMagic) != 0) {
        return damaged();
    }
    Reader in(bytes);
    char magic[4];
    for (char& c : magic) {
        std::uint8_t b = 0;
        in.get(b);
        c = static_cast<char>(b);
    }
    StoredFrame f;
    std::uint32_t w = 0, h = 0;
    std::uint8_t planeCount = 0;
    bool ok = in.get(w) && in.get(h) && in.get(f.pts) && in.get(f.duration) && in.get(f.streamPts) && in.get(f.timeBaseNum) &&
              in.get(f.timeBaseDen) && in.get(f.sampleNum) && in.get(f.sampleDen) && in.get(f.format) && in.get(f.matrix) &&
              in.get(f.range) && in.get(f.primaries) && in.get(f.transfer) && in.get(f.chroma) && in.get(f.alpha) && in.get(planeCount);
    if (!ok || w == 0 || h == 0 || w > kMaxSide || h > kMaxSide || planeCount == 0 || planeCount > kMaxPlanes) {
        return damaged();
    }
    f.width = static_cast<int>(w);
    f.height = static_cast<int>(h);
    std::uint64_t expected = 0;
    for (int i = 0; i < planeCount; ++i) {
        StoredPlane p;
        std::uint32_t pw = 0, ph = 0, bpt = 0;
        if (!in.get(pw) || !in.get(ph) || !in.get(bpt) || pw == 0 || ph == 0 || pw > kMaxSide || ph > kMaxSide ||
            (bpt != 1 && bpt != 2 && bpt != 4)) {
            return damaged();
        }
        p.width = static_cast<int>(pw);
        p.height = static_cast<int>(ph);
        p.bytesPerTexel = static_cast<int>(bpt);
        expected += static_cast<std::uint64_t>(pw) * ph * bpt;
        f.planes.push_back(std::move(p));
    }
    std::uint64_t rawBytes = 0, packedBytes = 0;
    if (!in.get(rawBytes) || !in.get(packedBytes) || rawBytes != expected || rawBytes > kMaxRawBytes || packedBytes != in.left()) {
        return damaged();
    }
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(rawBytes));
    uLongf rawSize = static_cast<uLongf>(raw.size());
    if (uncompress(raw.data(), &rawSize, in.data(), static_cast<uLong>(packedBytes)) != Z_OK || rawSize != raw.size()) {
        return damaged();
    }
    std::size_t at = 0;
    for (StoredPlane& p : f.planes) {
        const std::size_t n = static_cast<std::size_t>(p.width) * static_cast<std::size_t>(p.height) * static_cast<std::size_t>(p.bytesPerTexel);
        unshuffle(raw.data() + at, n, p.bytesPerTexel, p.data);
        at += n;
    }
    return f;
}

ProcessedCache::ProcessedCache(std::filesystem::path root, std::size_t memoryBudget)
    : m_root(std::move(root)), m_memoryBudget(memoryBudget) {}

std::filesystem::path ProcessedCache::defaultRoot() { return userCacheDir() / "ai-video"; }

std::filesystem::path ProcessedCache::directoryOf(const CacheIdentity& id) const { return m_root / id.directoryName(); }

std::filesystem::path ProcessedCache::fileFor(const CacheIdentity& id, std::int64_t pts) const {
    return directoryOf(id) / (std::to_string(pts) + ".wpf");
}

ProcessedCache::Directory& ProcessedCache::directory(const CacheIdentity& id) {
    Directory& d = m_directories[id.directoryName()];
    if (!d.scanned) {
        d.scanned = true;
        d.files.clear();
        d.bytes = 0;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(directoryOf(id), ec), end; !ec && it != end; it.increment(ec)) {
            const std::filesystem::path& p = it->path();
            if (p.extension() != ".wpf") {
                continue;
            }
            if (const auto pts = parsePts(p.stem().string())) {
                std::error_code sizeEc;
                const auto size = it->file_size(sizeEc);
                if (!sizeEc) {
                    d.files[*pts] = size;
                    d.bytes += size;
                }
            }
        }
    }
    return d;
}

std::expected<void, Error> ProcessedCache::put(const CacheIdentity& id, const StoredFrame& frame) {
    if (!id.valid()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "no cache identity"));
    }
    auto encoded = encodeFrame(frame);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    const std::filesystem::path dir = directoryOf(id);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot create the cache folder: " + ec.message()));
    }
    const std::filesystem::path target = fileFor(id, frame.pts);
    std::filesystem::path temporary = target;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(encoded->data()), static_cast<std::streamsize>(encoded->size()));
        out.close();
        if (!out) {
            std::filesystem::remove(temporary, ec);
            return std::unexpected(Error(ErrorCode::IoError, "cannot write the processed picture (disk full?)"));
        }
    }
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        std::error_code ignore;
        std::filesystem::remove(temporary, ignore);
        return std::unexpected(Error(ErrorCode::IoError, "cannot store the processed picture: " + ec.message()));
    }
    std::lock_guard lock(m_mutex);
    Directory& d = directory(id);
    const auto old = d.files.find(frame.pts);
    if (old != d.files.end()) {
        d.bytes -= std::min(d.bytes, old->second);
    }
    d.files[frame.pts] = encoded->size();
    d.bytes += encoded->size();
    // The decoded copy of an older picture at this time is stale.
    const std::string key = id.directoryName() + "/" + std::to_string(frame.pts);
    std::erase_if(m_recent, [&](const Decoded& r) {
        if (r.key == key) {
            m_recentBytes -= std::min(m_recentBytes, r.frame->bytes());
            return true;
        }
        return false;
    });
    ++m_revision;
    return {};
}

std::shared_ptr<const StoredFrame> ProcessedCache::load(const CacheIdentity& id, std::int64_t pts) {
    const std::filesystem::path file = fileFor(id, pts);
    std::vector<std::uint8_t> bytes;
    {
        std::ifstream in(file, std::ios::binary | std::ios::ate);
        if (!in) {
            return nullptr;
        }
        const auto size = in.tellg();
        if (size <= 0 || static_cast<std::uint64_t>(size) > kMaxRawBytes) {
            return nullptr;
        }
        bytes.resize(static_cast<std::size_t>(size));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(bytes.data()), size);
        if (!in) {
            return nullptr;
        }
    }
    auto decoded = decodeFrame(bytes);
    if (!decoded) {
        // Damaged (a copy interrupted elsewhere, a bad sector): forget it, the run makes it again.
        std::error_code ec;
        std::filesystem::remove(file, ec);
        std::lock_guard lock(m_mutex);
        Directory& d = directory(id);
        if (const auto it = d.files.find(pts); it != d.files.end()) {
            d.bytes -= std::min(d.bytes, it->second);
            d.files.erase(it);
            ++m_revision;
        }
        return nullptr;
    }
    return std::make_shared<const StoredFrame>(std::move(*decoded));
}

void ProcessedCache::remember(const std::string& key, std::shared_ptr<const StoredFrame> frame) {
    const std::size_t bytes = frame->bytes();
    if (bytes > m_memoryBudget) {
        return;
    }
    std::erase_if(m_recent, [&](const Decoded& r) {
        if (r.key == key) {
            m_recentBytes -= std::min(m_recentBytes, r.frame->bytes());
            return true;
        }
        return false;
    });
    m_recent.insert(m_recent.begin(), Decoded{key, std::move(frame)});
    m_recentBytes += bytes;
    while (m_recentBytes > m_memoryBudget && m_recent.size() > 1) {
        m_recentBytes -= std::min(m_recentBytes, m_recent.back().frame->bytes());
        m_recent.pop_back();
    }
}

std::shared_ptr<const StoredFrame> ProcessedCache::get(const CacheIdentity& id, Ticks pts, Ticks tolerance) {
    if (!id.valid()) {
        return nullptr;
    }
    std::int64_t found = 0;
    std::string key;
    {
        std::lock_guard lock(m_mutex);
        Directory& d = directory(id);
        if (d.files.empty()) {
            return nullptr;
        }
        auto it = d.files.lower_bound(pts.value);
        std::optional<std::int64_t> best;
        const auto consider = [&](std::map<std::int64_t, std::uint64_t>::iterator candidate) {
            const std::int64_t distance = std::abs(candidate->first - pts.value);
            if (distance <= tolerance.value && (!best || distance < std::abs(*best - pts.value))) {
                best = candidate->first;
            }
        };
        if (it != d.files.end()) {
            consider(it);
        }
        if (it != d.files.begin()) {
            consider(std::prev(it));
        }
        if (!best) {
            return nullptr;
        }
        found = *best;
        key = id.directoryName() + "/" + std::to_string(found);
        for (const Decoded& r : m_recent) {
            if (r.key == key) {
                return r.frame;
            }
        }
    }
    std::shared_ptr<const StoredFrame> frame = load(id, found);
    if (frame) {
        std::lock_guard lock(m_mutex);
        remember(key, frame);
    }
    return frame;
}

bool ProcessedCache::contains(const CacheIdentity& id, Ticks pts, Ticks tolerance) {
    if (!id.valid()) {
        return false;
    }
    std::lock_guard lock(m_mutex);
    Directory& d = directory(id);
    auto it = d.files.lower_bound(pts.value - tolerance.value);
    return it != d.files.end() && it->first <= pts.value + tolerance.value;
}

std::vector<Ticks> ProcessedCache::frames(const CacheIdentity& id) {
    std::vector<Ticks> out;
    if (!id.valid()) {
        return out;
    }
    std::lock_guard lock(m_mutex);
    for (const auto& [pts, size] : directory(id).files) {
        out.push_back(Ticks{pts});
    }
    return out;
}

std::size_t ProcessedCache::countIn(const CacheIdentity& id, Ticks from, Ticks to) {
    if (!id.valid() || to <= from) {
        return 0;
    }
    std::lock_guard lock(m_mutex);
    const Directory& d = directory(id);
    return static_cast<std::size_t>(std::distance(d.files.lower_bound(from.value), d.files.lower_bound(to.value)));
}

std::size_t ProcessedCache::countCovered(const CacheIdentity& id, const std::vector<Ticks>& wanted, Ticks tolerance) {
    if (!id.valid()) {
        return 0;
    }
    std::lock_guard lock(m_mutex);
    const Directory& d = directory(id);
    std::size_t n = 0;
    for (const Ticks t : wanted) {
        const auto it = d.files.lower_bound(t.value - tolerance.value);
        n += it != d.files.end() && it->first <= t.value + tolerance.value ? 1 : 0;
    }
    return n;
}

std::uint64_t ProcessedCache::diskBytes(const CacheIdentity& id) {
    if (!id.valid()) {
        return 0;
    }
    std::lock_guard lock(m_mutex);
    return directory(id).bytes;
}

std::uint64_t ProcessedCache::totalDiskBytes() {
    std::uint64_t total = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(m_root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code sizeEc;
        if (it->is_regular_file(sizeEc)) {
            const auto size = it->file_size(sizeEc);
            total += sizeEc ? 0 : size;
        }
    }
    return total;
}

void ProcessedCache::remove(const CacheIdentity& id) {
    const std::string name = id.directoryName();
    std::error_code ec;
    std::filesystem::remove_all(directoryOf(id), ec);
    std::lock_guard lock(m_mutex);
    m_directories.erase(name);
    std::erase_if(m_recent, [&](const Decoded& r) {
        if (r.key.starts_with(name + "/")) {
            m_recentBytes -= std::min(m_recentBytes, r.frame->bytes());
            return true;
        }
        return false;
    });
    ++m_revision;
}

void ProcessedCache::clear() {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(m_root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code removeEc;
        if (it->is_directory(removeEc)) {
            std::filesystem::remove_all(it->path(), removeEc);
        }
    }
    std::lock_guard lock(m_mutex);
    m_directories.clear();
    m_recent.clear();
    m_recentBytes = 0;
    ++m_revision;
}

void ProcessedCache::rescan(const CacheIdentity& id) {
    std::lock_guard lock(m_mutex);
    m_directories.erase(id.directoryName());
}

std::uint64_t ProcessedCache::revision() const {
    std::lock_guard lock(m_mutex);
    return m_revision;
}

std::expected<void, Error> ProcessedCache::writeProvenance(const CacheIdentity& id, const Provenance& p) {
    std::error_code ec;
    std::filesystem::create_directories(directoryOf(id), ec);
    const Json j{{"chain", p.chain}, {"settings", p.settings}, {"modelVersion", p.modelVersion},
                 {"models", p.models}, {"created", p.created}, {"app", p.app}};
    const std::filesystem::path target = directoryOf(id) / "provenance.json";
    std::filesystem::path temporary = target;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << j.dump(2, ' ', false, Json::error_handler_t::replace);
        out.close();
        if (!out) {
            return std::unexpected(Error(ErrorCode::IoError, "cannot write the provenance"));
        }
    }
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot write the provenance: " + ec.message()));
    }
    return {};
}

std::optional<Provenance> ProcessedCache::provenance(const CacheIdentity& id) const {
    std::ifstream in(directoryOf(id) / "provenance.json", std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    const Json j = Json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return std::nullopt;
    }
    const auto text = [&](const char* key) { return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string{}; };
    Provenance p;
    p.chain = text("chain");
    p.settings = text("settings");
    p.modelVersion = text("modelVersion");
    p.models = text("models");
    p.created = text("created");
    p.app = text("app");
    return p;
}

void ProcessedCache::recordSpeed(const std::string& model, const std::string& device, double msPerMegapixel) {
    if (!(msPerMegapixel > 0.0) || model.empty() || device.empty()) {
        return;
    }
    std::lock_guard lock(m_mutex);
    const std::filesystem::path file = m_root / "speed.json";
    Json j = Json::object();
    {
        std::ifstream in(file, std::ios::binary);
        if (in) {
            Json read = Json::parse(in, nullptr, false);
            if (read.is_object()) {
                j = std::move(read);
            }
        }
    }
    const std::string key = model + "|" + device;
    // A running mean: a slow first frame (the model compiling) does not stick.
    const double old = j.contains(key) && j[key].is_number() ? j[key].get<double>() : 0.0;
    j[key] = old > 0.0 ? 0.7 * old + 0.3 * msPerMegapixel : msPerMegapixel;
    std::error_code ec;
    std::filesystem::create_directories(m_root, ec);
    std::filesystem::path temporary = file;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << j.dump(2);
    }
    std::filesystem::rename(temporary, file, ec);
}

double ProcessedCache::speed(const std::string& model, const std::string& device) const {
    std::lock_guard lock(m_mutex);
    std::ifstream in(m_root / "speed.json", std::ios::binary);
    if (!in) {
        return 0.0;
    }
    const Json j = Json::parse(in, nullptr, false);
    const std::string key = model + "|" + device;
    return j.is_object() && j.contains(key) && j[key].is_number() ? j[key].get<double>() : 0.0;
}

} // namespace wabcut::ai::video
