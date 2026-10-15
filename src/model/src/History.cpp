// Version history (History.h, C.9): content-addressed, compressed, crash-safe revisions in a sidecar folder.

#include <wabcut/model/History.h>
#include <wabcut/model/MediaLocator.h>

#include <wabcut/model/ProjectIO.h>

#include "HistoryFormat.h"
#include "ProjectChunks.h"

#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <unordered_set>

namespace wabcut {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// --- SHA-256 (FIPS 180-4) ---------------------------------------------------------------------------------

constexpr std::array<std::uint32_t, 64> kRound{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

constexpr std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha256Block(std::array<std::uint32_t, 8>& h, const unsigned char* p) {
    std::array<std::uint32_t, 64> w{};
    for (int i = 0; i < 16; ++i) {
        w[static_cast<std::size_t>(i)] = (std::uint32_t{p[4 * i]} << 24) | (std::uint32_t{p[4 * i + 1]} << 16) |
                                         (std::uint32_t{p[4 * i + 2]} << 8) | std::uint32_t{p[4 * i + 3]};
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = k + s1 + ch + kRound[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        k = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += k;
}

std::string sha256Hex(std::string_view data) {
    std::array<std::uint32_t, 8> h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    std::size_t n = data.size();
    while (n >= 64) {
        sha256Block(h, p);
        p += 64;
        n -= 64;
    }
    std::array<unsigned char, 128> tail{};
    std::memcpy(tail.data(), p, n);
    tail[n] = 0x80;
    const std::size_t blocks = n + 9 > 64 ? 2 : 1;
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    for (int i = 0; i < 8; ++i) {
        tail[blocks * 64 - 1 - static_cast<std::size_t>(i)] = static_cast<unsigned char>(bits >> (8 * i));
    }
    for (std::size_t b = 0; b < blocks; ++b) {
        sha256Block(h, tail.data() + 64 * b);
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const std::uint32_t v : h) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            out += kHex[(v >> shift) & 0xF];
        }
    }
    return out;
}

bool isKey(std::string_view key) {
    return key.size() == 64 && std::all_of(key.begin(), key.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

// --- Objects: "WCH1", the text's length (8 bytes, little-endian), zlib data -------------------------------------

constexpr char kMagic[4] = {'W', 'C', 'H', '1'};

std::expected<std::string, Error> compress(std::string_view text) {
    uLongf size = compressBound(static_cast<uLong>(text.size()));
    std::string out(12 + size, '\0');
    std::memcpy(out.data(), kMagic, 4);
    const auto length = static_cast<std::uint64_t>(text.size());
    for (int i = 0; i < 8; ++i) {
        out[4 + static_cast<std::size_t>(i)] = static_cast<char>((length >> (8 * i)) & 0xFF);
    }
    const int rc = compress2(reinterpret_cast<Bytef*>(out.data() + 12), &size,
                             reinterpret_cast<const Bytef*>(text.data()), static_cast<uLong>(text.size()), 6);
    if (rc != Z_OK) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "cannot compress a history object"});
    }
    out.resize(12 + size);
    return out;
}

std::expected<std::string, Error> decompress(std::string_view file) {
    if (file.size() < 12 || std::memcmp(file.data(), kMagic, 4) != 0) {
        return std::unexpected(Error{ErrorCode::ParseError, "a history object is damaged (no header)"});
    }
    std::uint64_t length = 0;
    for (int i = 0; i < 8; ++i) {
        length |= std::uint64_t{static_cast<unsigned char>(file[4 + static_cast<std::size_t>(i)])} << (8 * i);
    }
    if (length > kMaxHistoryObjectBytes) {
        return std::unexpected(Error{ErrorCode::ParseError, "a history object is larger than allowed"});
    }
    // Inflate in steps and grow the text as it arrives: the header's length bounds the text, but it is untrusted, so
    // it is never allocated up front (a 12-byte file must not cost 1 GiB).
    if (file.size() - 12 > std::numeric_limits<uInt>::max()) {
        return std::unexpected(Error{ErrorCode::ParseError, "a history object is larger than allowed"});
    }
    z_stream z{};
    if (inflateInit(&z) != Z_OK) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot start decompressing a history object"});
    }
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(file.data() + 12));
    z.avail_in = static_cast<uInt>(file.size() - 12);
    std::string out;
    std::array<char, 64 * 1024> buffer{};
    int rc = Z_OK;
    while (rc == Z_OK) {
        z.next_out = reinterpret_cast<Bytef*>(buffer.data());
        z.avail_out = static_cast<uInt>(buffer.size());
        rc = inflate(&z, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            break;
        }
        const std::size_t produced = buffer.size() - z.avail_out;
        if (out.size() + produced > length) {
            rc = Z_DATA_ERROR;
            break;
        }
        out.append(buffer.data(), produced);
    }
    inflateEnd(&z);
    if (rc != Z_STREAM_END || out.size() != length) {
        return std::unexpected(Error{ErrorCode::ParseError, "a history object is damaged (bad data)"});
    }
    return out;
}

std::expected<std::string, Error> readFile(const fs::path& path, std::uint64_t limit) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot read " + pathToUtf8(path)});
    }
    if (size > limit) {
        return std::unexpected(Error{ErrorCode::ParseError, pathToUtf8(path) + " is larger than allowed"});
    }
    std::ifstream in(path, std::ios::binary);
    std::string data(static_cast<std::size_t>(size), '\0');
    if (!in.read(data.data(), static_cast<std::streamsize>(size))) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot read " + pathToUtf8(path)});
    }
    return data;
}

std::string nowUtc() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return text;
}

constexpr std::string_view kIndexFormat = "wabcut-history";
constexpr std::array<std::pair<HistoryKind, std::string_view>, 5> kKinds{{{HistoryKind::Save, "save"},
                                                                          {HistoryKind::Auto, "auto"},
                                                                          {HistoryKind::Agent, "agent"},
                                                                          {HistoryKind::Manual, "manual"},
                                                                          {HistoryKind::Restore, "restore"}}};

} // namespace

std::string_view toString(HistoryKind kind) {
    for (const auto& [k, name] : kKinds) {
        if (k == kind) {
            return name;
        }
    }
    return "auto";
}

std::optional<HistoryKind> historyKindFromString(std::string_view name) {
    for (const auto& [k, text] : kKinds) {
        if (text == name) {
            return k;
        }
    }
    return std::nullopt;
}

/// Stores the chunks of one checkpoint: new texts become objects; known nodes are answered from the previous one.
class HistorySink final : public io::detail::ChunkSink {
public:
    explicit HistorySink(ProjectHistory& history) : m_history(history) {}

    std::optional<std::string> known(const std::shared_ptr<const void>& node) override {
        const auto it = m_history.m_known.find(node.get());
        if (it == m_history.m_known.end() || !m_history.m_objects.contains(it->second.second)) {
            return std::nullopt;
        }
        m_seen.emplace(node.get(), std::pair{node, it->second.second});
        return it->second.second;
    }

    std::string put(const std::shared_ptr<const void>& node, std::string text) override {
        std::string key = sha256Hex(text);
        if (node) {
            m_seen.emplace(node.get(), std::pair{node, key});
        }
        if (!m_history.m_objects.contains(key) && !m_failed) {
            auto data = compress(text);
            if (!data) {
                m_failed = data.error();
                return key;
            }
            const fs::path path = m_history.objectPath(key);
            // Each fan-out folder once per checkpoint (P.1): create_directories walks the whole path every time, a
            // third of a 10,000-clip checkpoint's time when it ran for every object.
            if (m_folders.insert(key.substr(0, 2)).second) {
                std::error_code ec;
                fs::create_directories(path.parent_path(), ec);
            }
            if (auto written = io::writeFileAtomically(path, *data); !written) {
                m_failed = written.error();
                return key;
            }
            m_history.m_objects.emplace(key, data->size());
            m_added += data->size();
        }
        return key;
    }

    std::optional<Error> m_failed;
    std::uint64_t m_added = 0;
    std::unordered_set<std::string> m_folders; ///< object folders made (or found) in this checkpoint
    std::unordered_map<const void*, std::pair<std::shared_ptr<const void>, std::string>> m_seen;

private:
    ProjectHistory& m_history;
};

fs::path ProjectHistory::folderFor(const fs::path& projectFile) {
    fs::path folder = projectFile;
    folder += ".history";
    return folder;
}

fs::path ProjectHistory::objectPath(const std::string& key) const {
    return m_folder / "objects" / key.substr(0, 2) / key;
}

std::expected<ProjectHistory, Error> ProjectHistory::open(fs::path folder, HistoryOptions options) {
    ProjectHistory h;
    h.m_folder = std::move(folder);
    h.m_options = options;
    std::error_code ec;
    fs::create_directories(h.m_folder / "objects", ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot create the history folder " + pathToUtf8(h.m_folder)});
    }
    // Objects on disk (leftover temporary files of an interrupted write go).
    for (fs::recursive_directory_iterator it(h.m_folder / "objects", ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const std::string name = pathToUtf8(it->path().filename());
        if (isKey(name)) {
            h.m_objects.emplace(name, it->file_size(ec));
        } else {
            fs::remove(it->path(), ec);
        }
    }
    const fs::path index = h.m_folder / "index.json";
    fs::remove(h.m_folder / "index.json.tmp", ec);
    if (!fs::exists(index, ec)) {
        return h;
    }
    auto text = readFile(index, kMaxHistoryIndexBytes);
    if (!text) {
        return std::unexpected(text.error());
    }
    auto parsed = detail::parseHistoryIndex(*text);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    h.m_revisions = std::move(parsed->revisions);
    h.m_nextRevision = parsed->nextRevision;
    return h;
}

namespace detail {

std::expected<HistoryIndex, Error> parseHistoryIndex(std::string_view text) {
    if (text.size() > kMaxHistoryIndexBytes || !io::jsonNestingWithin(text)) {
        return std::unexpected(Error{ErrorCode::ParseError, "the history index is larger or deeper than allowed"});
    }
    const json root = json::parse(text, nullptr, false);
    const auto damaged = [&](const std::string& why) {
        return std::unexpected(Error{ErrorCode::ParseError, "the history index is damaged: " + why});
    };
    if (!root.is_object() || root.value("format", std::string()) != kIndexFormat || !root.contains("revisions") ||
        !root["revisions"].is_array()) {
        return damaged("not a Wabcut history index");
    }
    if (root.value("version", 0) != 1) {
        return std::unexpected(Error{ErrorCode::VersionMismatch, "the history was written by a newer version of Wabcut"});
    }
    if (root["revisions"].size() > kMaxHistoryRevisions) {
        return damaged("too many revisions");
    }
    HistoryIndex out;
    try {
        for (const json& r : root["revisions"]) {
            HistoryRevision rev;
            rev.id = r.at("id").get<std::uint64_t>();
            rev.createdAt = r.value("createdAt", std::string());
            const auto kind = historyKindFromString(r.value("kind", std::string("auto")));
            if (!kind) {
                return damaged("unknown revision kind");
            }
            rev.kind = *kind;
            rev.label = r.value("label", std::string());
            rev.root = r.at("root").get<std::string>();
            rev.addedBytes = r.value("addedBytes", std::uint64_t{0});
            rev.sequences = r.value("sequences", std::uint64_t{0});
            rev.clips = r.value("clips", std::uint64_t{0});
            if (!isKey(rev.root) || rev.label.size() > kMaxHistoryLabelBytes || rev.createdAt.size() > 64 ||
                rev.id == 0 || rev.id >= kMaxHistoryRevisionId ||
                (!out.revisions.empty() && rev.id <= out.revisions.back().id)) {
                return damaged("revision " + std::to_string(rev.id) + " is malformed");
            }
            out.revisions.push_back(std::move(rev));
        }
        const std::uint64_t next = root.value("nextRevision", std::uint64_t{1});
        if (next >= kMaxHistoryRevisionId) {
            return damaged("the next revision id is out of range");
        }
        out.nextRevision = std::max<std::uint64_t>(next, out.revisions.empty() ? 1 : out.revisions.back().id + 1);
    } catch (const json::exception& e) {
        return damaged(e.what());
    }
    return out;
}

std::expected<std::string, Error> encodeHistoryObject(std::string_view text) { return compress(text); }

std::expected<std::string, Error> decodeHistoryObject(std::string_view file) { return decompress(file); }

} // namespace detail

const HistoryRevision* ProjectHistory::find(std::uint64_t id) const {
    const auto it = std::lower_bound(m_revisions.begin(), m_revisions.end(), id,
                                     [](const HistoryRevision& r, std::uint64_t v) { return r.id < v; });
    return it != m_revisions.end() && it->id == id ? &*it : nullptr;
}

std::expected<void, Error> ProjectHistory::writeIndex() const {
    json revisions = json::array();
    for (const HistoryRevision& r : m_revisions) {
        revisions.push_back(json{{"id", r.id},
                                 {"createdAt", r.createdAt},
                                 {"kind", std::string(toString(r.kind))},
                                 {"label", r.label},
                                 {"root", r.root},
                                 {"addedBytes", r.addedBytes},
                                 {"sequences", r.sequences},
                                 {"clips", r.clips}});
    }
    const json root{{"format", std::string(kIndexFormat)},
                    {"version", 1},
                    {"nextRevision", m_nextRevision},
                    {"revisions", std::move(revisions)}};
    return io::writeFileAtomically(m_folder / "index.json", root.dump(1));
}

std::expected<std::string, Error> ProjectHistory::readObject(const std::string& key) const {
    if (!isKey(key)) {
        return std::unexpected(Error{ErrorCode::ParseError, "a history reference is malformed"});
    }
    auto file = readFile(objectPath(key), kMaxHistoryObjectBytes);
    if (!file) {
        return std::unexpected(Error{ErrorCode::NotFound, "history object " + key + " is missing"});
    }
    auto text = decompress(*file);
    if (!text) {
        return std::unexpected(text.error());
    }
    if (sha256Hex(*text) != key) {
        return std::unexpected(Error{ErrorCode::ParseError, "history object " + key + " is damaged (checksum)"});
    }
    return text;
}

std::expected<HistoryRevision, Error> ProjectHistory::checkpoint(const ProjectState& state, HistoryKind kind,
                                                                 std::string label, std::string createdAt) {
    if (label.size() > kMaxHistoryLabelBytes) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "a version name has at most " +
                                                                     std::to_string(kMaxHistoryLabelBytes) + " bytes"});
    }
    HistorySink sink(*this);
    auto root = io::detail::writeChunks(state, sink);
    if (!root) {
        return std::unexpected(root.error());
    }
    if (sink.m_failed) {
        return std::unexpected(*sink.m_failed);
    }
    m_known = std::move(sink.m_seen); // only the newest checkpoint's nodes: bounded memory
    if (!m_revisions.empty() && m_revisions.back().root == *root && label.empty()) {
        return m_revisions.back(); // nothing changed
    }
    if (m_options.failBeforeIndexForTesting) {
        return std::unexpected(Error{ErrorCode::IoError, "simulated crash before the history index was written"});
    }
    HistoryRevision rev;
    rev.id = m_nextRevision++;
    rev.createdAt = createdAt.empty() ? nowUtc() : std::move(createdAt);
    rev.kind = kind;
    rev.label = std::move(label);
    rev.root = std::move(*root);
    rev.addedBytes = sink.m_added;
    rev.sequences = state.sequences.size();
    for (const SequencePtr& s : state.sequences) {
        s->forEachTrack([&](const Track& t) { rev.clips += t.clips.size(); });
    }
    m_revisions.push_back(rev);
    if (auto written = writeIndex(); !written) {
        m_revisions.pop_back();
        --m_nextRevision;
        return std::unexpected(written.error());
    }
    if (m_options.pruneOnCheckpoint) {
        if (auto pruned = prune(); !pruned) {
            return std::unexpected(pruned.error());
        }
    }
    return rev;
}

std::expected<void, Error> ProjectHistory::name(std::uint64_t id, std::string label) {
    if (label.size() > kMaxHistoryLabelBytes) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "a version name has at most " +
                                                                     std::to_string(kMaxHistoryLabelBytes) + " bytes"});
    }
    const auto it = std::find_if(m_revisions.begin(), m_revisions.end(), [&](const HistoryRevision& r) { return r.id == id; });
    if (it == m_revisions.end()) {
        return std::unexpected(Error{ErrorCode::NotFound, "revision " + std::to_string(id) + " is not in the history"});
    }
    const std::string previous = std::exchange(it->label, std::move(label));
    if (auto written = writeIndex(); !written) {
        it->label = previous;
        return std::unexpected(written.error());
    }
    return {};
}

std::expected<ProjectState, Error> ProjectHistory::load(std::uint64_t id) const {
    const HistoryRevision* rev = find(id);
    if (!rev) {
        return std::unexpected(Error{ErrorCode::NotFound, "revision " + std::to_string(id) + " is not in the history"});
    }
    return io::detail::readChunks(rev->root, [&](const std::string& key) { return readObject(key); });
}

std::uint64_t ProjectHistory::diskBytes() const {
    std::uint64_t total = 0;
    for (const auto& [key, size] : m_objects) {
        total += size;
    }
    std::error_code ec;
    const auto index = fs::file_size(m_folder / "index.json", ec);
    return total + (ec ? 0 : index);
}

std::expected<HistoryPruneReport, Error> ProjectHistory::prune() {
    HistoryPruneReport report;
    // Which revisions stay: named ones, the newest keepRecent unnamed ones, then the oldest go while over budget.
    std::vector<bool> keep(m_revisions.size(), false);
    std::size_t recent = 0;
    for (std::size_t i = m_revisions.size(); i-- > 0;) {
        if (m_revisions[i].named()) {
            keep[i] = true;
        } else if (recent < m_options.keepRecent) {
            keep[i] = true;
            ++recent;
        }
    }
    if (!m_revisions.empty()) {
        keep.back() = true;
    }
    // The objects each remaining revision reaches: its root, the media list and sequences the root names, the tracks
    // each sequence names (tracks and the media list name nothing: they are not read).
    const auto reach = [&](const HistoryRevision& r, std::unordered_set<std::string>& into) -> std::expected<void, Error> {
        std::vector<std::pair<std::string, int>> stack{{r.root, 0}};
        while (!stack.empty()) {
            auto [key, depth] = std::move(stack.back());
            stack.pop_back();
            if (!into.insert(key).second || depth >= 2) {
                continue;
            }
            auto it = m_refs.find(key);
            if (it == m_refs.end()) {
                auto text = readObject(key);
                if (!text) {
                    continue; // a missing object: nothing beneath it to keep
                }
                auto named = io::detail::chunkReferences(*text);
                it = m_refs.emplace(key, named ? std::move(*named) : std::vector<std::string>{}).first;
            }
            for (const std::string& k : it->second) {
                stack.emplace_back(k, depth + 1);
            }
        }
        return {};
    };
    const auto sizeOf = [&](const std::unordered_set<std::string>& keys) {
        std::uint64_t total = 0;
        for (const std::string& k : keys) {
            if (const auto it = m_objects.find(k); it != m_objects.end()) {
                total += it->second;
            }
        }
        return total;
    };
    std::unordered_set<std::string> live;
    for (std::size_t i = 0; i < m_revisions.size(); ++i) {
        if (keep[i]) {
            if (auto ok = reach(m_revisions[i], live); !ok) {
                return std::unexpected(ok.error());
            }
        }
    }
    // Over budget: drop the oldest kept unnamed revisions (never the newest) until the live objects fit.
    for (std::size_t i = 0; i + 1 < m_revisions.size() && sizeOf(live) > m_options.maxBytes; ++i) {
        if (keep[i] && !m_revisions[i].named()) {
            keep[i] = false;
            live.clear();
            for (std::size_t k = 0; k < m_revisions.size(); ++k) {
                if (keep[k]) {
                    if (auto ok = reach(m_revisions[k], live); !ok) {
                        return std::unexpected(ok.error());
                    }
                }
            }
        }
    }
    std::vector<HistoryRevision> kept;
    for (std::size_t i = 0; i < m_revisions.size(); ++i) {
        if (keep[i]) {
            kept.push_back(m_revisions[i]);
        } else {
            ++report.revisionsRemoved;
        }
    }
    if (report.revisionsRemoved > 0) {
        std::swap(m_revisions, kept);
        if (auto written = writeIndex(); !written) {
            std::swap(m_revisions, kept);
            return std::unexpected(written.error());
        }
    }
    // Objects no revision reaches go (after the index no longer names them).
    std::vector<std::string> dead;
    for (const auto& [key, size] : m_objects) {
        if (!live.contains(key)) {
            dead.push_back(key);
        }
    }
    for (const std::string& key : dead) {
        std::error_code ec;
        fs::remove(objectPath(key), ec);
        if (!ec) {
            report.bytesFreed += m_objects[key];
            ++report.objectsRemoved;
            m_objects.erase(key);
            m_refs.erase(key);
        }
    }
    std::erase_if(m_known, [&](const auto& entry) { return !m_objects.contains(entry.second.second); });
    return report;
}

} // namespace wabcut
