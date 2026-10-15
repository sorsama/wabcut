#include <wabcut/ai/search/SearchIndex.h>

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/Sha256.h>
#include <wabcut/ai/search/Tokenizer.h>
#include <wabcut/model/MediaLocator.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4701 4703) // hnswlib: "potentially uninitialized" (false positives in addPoint)
#endif
// hnswlib 0.9.0's USE_SSE code prefetches the neighbour after the last one, *(datal + j + 1): one tableint past a full
// upper-level link list, which addPoint allocates with one spare byte (size_links_per_element_ * level + 1). A
// heap-buffer-overflow read under AddressSanitizer (C.10), undefined behaviour in any build. Without manual
// vectorisation those prefetches are gone; UnitDotSpace below brings back a fast distance. This is the only file that
// includes hnswlib, so every use of its templates sees the same definition.
#define NO_MANUAL_VECTORIZATION
#include <hnswlib/hnswlib.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace wabcut::ai::search {

namespace {

using Json = nlohmann::json;

std::unexpected<Error> ioError(std::string message) {
    return std::unexpected(Error(ErrorCode::IoError, "search index: " + std::move(message)));
}

std::int64_t asKey(std::uint64_t fingerprint) {
    std::int64_t key = 0;
    std::memcpy(&key, &fingerprint, sizeof key);
    return key;
}

std::uint64_t fromKey(std::int64_t key) {
    std::uint64_t fingerprint = 0;
    std::memcpy(&fingerprint, &key, sizeof fingerprint);
    return fingerprint;
}

/// One prepared statement (RAII).
class Statement {
public:
    Statement(sqlite3* db, const char* sql) : m_db(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &m_stmt, nullptr) != SQLITE_OK) {
            m_stmt = nullptr;
        }
    }
    ~Statement() { sqlite3_finalize(m_stmt); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    bool ok() const { return m_stmt != nullptr; }
    std::string error() const { return sqlite3_errmsg(m_db); }

    Statement& bind(int index, std::int64_t value) {
        sqlite3_bind_int64(m_stmt, index, value);
        return *this;
    }
    Statement& bind(int index, const std::string& value) {
        sqlite3_bind_text(m_stmt, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Statement& bindBlob(int index, const void* data, std::size_t size) {
        sqlite3_bind_blob(m_stmt, index, data, static_cast<int>(size), SQLITE_TRANSIENT);
        return *this;
    }
    /// SQLITE_ROW, SQLITE_DONE or an error code.
    int step() { return sqlite3_step(m_stmt); }
    void reset() {
        sqlite3_reset(m_stmt);
        sqlite3_clear_bindings(m_stmt);
    }

    std::int64_t integer(int column) const { return sqlite3_column_int64(m_stmt, column); }
    double number(int column) const { return sqlite3_column_double(m_stmt, column); }
    std::string text(int column) const {
        const auto* p = sqlite3_column_text(m_stmt, column);
        return p ? std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(sqlite3_column_bytes(m_stmt, column)))
                 : std::string();
    }
    std::span<const std::byte> blob(int column) const {
        const void* p = sqlite3_column_blob(m_stmt, column);
        return {static_cast<const std::byte*>(p), static_cast<std::size_t>(sqlite3_column_bytes(m_stmt, column))};
    }

private:
    sqlite3* m_db = nullptr;
    sqlite3_stmt* m_stmt = nullptr;
};

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS sources(
    fingerprint INTEGER PRIMARY KEY,
    name TEXT NOT NULL DEFAULT '',
    visual_version TEXT NOT NULL DEFAULT '',
    transcript_key TEXT NOT NULL DEFAULT '');
CREATE TABLE IF NOT EXISTS keyframes(
    id INTEGER PRIMARY KEY,
    fingerprint INTEGER NOT NULL,
    t0 INTEGER NOT NULL, t1 INTEGER NOT NULL, time INTEGER NOT NULL,
    vector BLOB NOT NULL);
CREATE INDEX IF NOT EXISTS keyframes_source ON keyframes(fingerprint);
CREATE TABLE IF NOT EXISTS phrases(
    id INTEGER PRIMARY KEY,
    fingerprint INTEGER NOT NULL,
    t0 INTEGER NOT NULL, t1 INTEGER NOT NULL,
    words TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS phrases_source ON phrases(fingerprint);
CREATE VIRTUAL TABLE IF NOT EXISTS phrases_fts USING fts5(spaced, compact, tokenize='trigram');
)sql";

/// The words of a phrase as stored: [["word", t0, t1], ...].
std::string wordsJson(const IndexedPhrase& phrase) {
    Json words = Json::array();
    for (std::size_t i = 0; i < phrase.words.size(); ++i) {
        const TimeRange t = i < phrase.wordTimes.size() ? phrase.wordTimes[i] : phrase.range;
        words.push_back(Json::array({phrase.words[i], t.start.value, t.end().value}));
    }
    return words.dump(-1, ' ', false, Json::error_handler_t::replace);
}

void readWords(const std::string& text, TextMatch& match) {
    const Json words = Json::parse(text, nullptr, false);
    if (!words.is_array()) {
        return;
    }
    for (const Json& w : words) {
        if (w.is_array() && w.size() == 3 && w[0].is_string() && w[1].is_number_integer() && w[2].is_number_integer()) {
            match.words.push_back(w[0].get<std::string>());
            match.wordTimes.push_back(TimeRange::fromStartEnd(Ticks{w[1].get<std::int64_t>()}, Ticks{w[2].get<std::int64_t>()}));
        }
    }
}

/// SHA-256 of a file (hex), "" when it cannot be read.
std::string fileSha256(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return {};
    }
    ai::Sha256 h;
    std::vector<char> buffer(1 << 20);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        h.update(std::string_view(buffer.data(), static_cast<std::size_t>(in.gcount())));
    }
    return in.bad() ? std::string() : h.finishHex();
}

/// Cosine distance of unit vectors, 1 - a . b (hnswlib's InnerProductSpace without its intrinsics): eight independent
/// sums, which compilers keep in vector registers.
float unitDotDistance(const void* a, const void* b, const void* param) {
    const std::size_t n = *static_cast<const std::size_t*>(param);
    const float* x = static_cast<const float*>(a);
    const float* y = static_cast<const float*>(b);
    float s[8] = {};
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        for (std::size_t k = 0; k < 8; ++k) {
            s[k] += x[i + k] * y[i + k];
        }
    }
    for (; i < n; ++i) {
        s[0] += x[i] * y[i];
    }
    return 1.0f - (((s[0] + s[1]) + (s[2] + s[3])) + ((s[4] + s[5]) + (s[6] + s[7])));
}

class UnitDotSpace final : public hnswlib::SpaceInterface<float> {
public:
    explicit UnitDotSpace(std::size_t dimension) : m_dimension(dimension) {}
    std::size_t get_data_size() override { return m_dimension * sizeof(float); }
    hnswlib::DISTFUNC<float> get_dist_func() override { return &unitDotDistance; }
    void* get_dist_func_param() override { return &m_dimension; }

private:
    std::size_t m_dimension;
};

/// A whole file (at most `limit` bytes), nullopt when it cannot be read or is larger.
std::optional<std::vector<std::uint8_t>> readFile(const std::filesystem::path& file, std::uintmax_t limit) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec || size > limit) {
        return std::nullopt;
    }
    std::ifstream in(file, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!in || !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return std::nullopt;
    }
    return bytes;
}

std::string bytesSha256(std::span<const std::uint8_t> bytes) {
    ai::Sha256 h;
    h.update(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    return h.finishHex();
}

/// Reads a POD from the front of `in`; false when it is too short.
template <class T>
bool take(std::span<const std::uint8_t>& in, T& value) {
    if (in.size() < sizeof(T)) {
        return false;
    }
    std::memcpy(&value, in.data(), sizeof(T));
    in = in.subspan(sizeof(T));
    return true;
}

template <class T>
T at(const std::uint8_t* p) {
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}

constexpr std::size_t kMaxGraphElements = std::size_t{1} << 24;
constexpr int kMaxGraphLevel = 64;

std::string joined(std::span<const std::string> words, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i > 0) {
            out += separator;
        }
        out += words[i];
    }
    return out;
}

} // namespace

// --- Impl -----------------------------------------------------------------------------------------------------------

struct SearchIndex::Impl {
    struct Row {
        std::int64_t id = 0;
        std::uint64_t fingerprint = 0;
        TimeRange range;
        Ticks time;
    };

    std::filesystem::path dir;
    std::string modelVersion;
    int dimension = 0;
    SearchIndexOptions options;
    sqlite3* db = nullptr;
    mutable std::mutex mutex;

    // Every keyframe vector in memory: row i's vector is matrix[i * dimension, (i + 1) * dimension).
    std::vector<Row> rows;
    std::vector<float> matrix;
    std::unordered_map<std::int64_t, std::size_t> rowOfId;

    // The HNSW graph over all rows (label = keyframe id), built on first need.
    std::unique_ptr<UnitDotSpace> space;
    mutable std::unique_ptr<hnswlib::HierarchicalNSW<float>> graph;
    mutable bool graphDirty = false;
    mutable bool lastUsedGraph = false;
    mutable std::size_t refusedGraphs = 0; ///< saved graphs that did not check out (rebuilt)

    ~Impl() {
        if (db) {
            sqlite3_close_v2(db);
        }
    }

    std::filesystem::path graphFile() const { return dir / "visual.hnsw"; }

    std::expected<void, Error> exec(const char* sql) {
        char* message = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK) {
            std::string text = message ? message : sqlite3_errmsg(db);
            sqlite3_free(message);
            return ioError(text);
        }
        return {};
    }

    std::string meta(const std::string& key) {
        Statement s(db, "SELECT value FROM meta WHERE key = ?1");
        s.bind(1, key);
        return s.ok() && s.step() == SQLITE_ROW ? s.text(0) : std::string();
    }
    std::expected<void, Error> setMeta(const std::string& key, const std::string& value) {
        Statement s(db, "INSERT INTO meta(key, value) VALUES(?1, ?2) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
        if (!s.ok()) {
            return ioError(s.error());
        }
        s.bind(1, key).bind(2, value);
        if (s.step() != SQLITE_DONE) {
            return ioError(s.error());
        }
        return {};
    }

    std::expected<void, Error> openDatabase(const std::filesystem::path& file) {
        const std::string path = pathToUtf8(file);
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
            std::string message = db ? sqlite3_errmsg(db) : "out of memory";
            sqlite3_close_v2(db);
            db = nullptr;
            return ioError("cannot open " + path + ": " + message);
        }
        sqlite3_busy_timeout(db, 5000);
        if (auto ok = exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"); !ok) {
            return ok;
        }
        if (auto ok = exec(kSchema); !ok) {
            const std::string message = ok.error().message;
            if (message.find("fts5") != std::string::npos || message.find("trigram") != std::string::npos) {
                return std::unexpected(Error(ErrorCode::Unsupported, "search index: this SQLite has no FTS5 trigram tokenizer (" +
                                                                         message + ")"));
            }
            return ok;
        }
        return {};
    }

    /// Schema and model checks after opening: an index of another schema is rebuilt, one of another model loses
    /// its vectors.
    std::expected<void, Error> prepare() {
        const std::string schema = meta("schema");
        if (!schema.empty() && schema != std::to_string(kSchemaVersion)) {
            if (auto ok = exec("DELETE FROM keyframes; DELETE FROM phrases; DELETE FROM phrases_fts; DELETE FROM sources;"); !ok) {
                return ok;
            }
        }
        if (auto ok = setMeta("schema", std::to_string(kSchemaVersion)); !ok) {
            return ok;
        }
        const std::string model = meta("model");
        const std::string dim = meta("dimension");
        if (model != modelVersion || dim != std::to_string(dimension)) {
            if (auto ok = exec("DELETE FROM keyframes; UPDATE sources SET visual_version = '';"); !ok) {
                return ok;
            }
            std::error_code ec;
            std::filesystem::remove(graphFile(), ec);
            if (auto ok = setMeta("model", modelVersion); !ok) {
                return ok;
            }
            if (auto ok = setMeta("dimension", std::to_string(dimension)); !ok) {
                return ok;
            }
        }
        return loadRows();
    }

    std::expected<void, Error> loadRows() {
        rows.clear();
        matrix.clear();
        rowOfId.clear();
        Statement s(db, "SELECT id, fingerprint, t0, t1, time, vector FROM keyframes ORDER BY id");
        if (!s.ok()) {
            return ioError(s.error());
        }
        int rc = SQLITE_ROW;
        while ((rc = s.step()) == SQLITE_ROW) {
            const auto bytes = s.blob(5);
            if (bytes.size() != static_cast<std::size_t>(dimension) * sizeof(float)) {
                continue; // a vector of another size: skipped (and gone at the next rebuild of its source)
            }
            Row row{s.integer(0), fromKey(s.integer(1)), TimeRange::fromStartEnd(Ticks{s.integer(2)}, Ticks{s.integer(3)}),
                    Ticks{s.integer(4)}};
            rowOfId[row.id] = rows.size();
            rows.push_back(row);
            const std::size_t at = matrix.size();
            matrix.resize(at + static_cast<std::size_t>(dimension));
            std::memcpy(&matrix[at], bytes.data(), bytes.size());
        }
        if (rc != SQLITE_DONE) {
            return ioError(sqlite3_errmsg(db));
        }
        return {};
    }

    std::expected<void, Error> upsertSource(std::uint64_t fingerprint, const std::string& name) {
        Statement s(db, "INSERT INTO sources(fingerprint, name) VALUES(?1, ?2) "
                        "ON CONFLICT(fingerprint) DO UPDATE SET name = excluded.name");
        if (!s.ok()) {
            return ioError(s.error());
        }
        s.bind(1, asKey(fingerprint)).bind(2, name);
        if (s.step() != SQLITE_DONE) {
            return ioError(s.error());
        }
        return {};
    }

    /// Drops the source's rows from memory (and marks them deleted in the graph).
    void forgetRows(std::uint64_t fingerprint) {
        std::vector<Row> keptRows;
        std::vector<float> keptMatrix;
        keptRows.reserve(rows.size());
        keptMatrix.reserve(matrix.size());
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].fingerprint == fingerprint) {
                if (graph) {
                    try {
                        graph->markDelete(static_cast<hnswlib::labeltype>(rows[i].id));
                        graphDirty = true;
                    } catch (const std::exception&) {
                        graph.reset(); // out of step: rebuilt on next need
                    }
                }
                continue;
            }
            keptRows.push_back(rows[i]);
            keptMatrix.insert(keptMatrix.end(), matrix.begin() + static_cast<std::ptrdiff_t>(i * static_cast<std::size_t>(dimension)),
                              matrix.begin() + static_cast<std::ptrdiff_t>((i + 1) * static_cast<std::size_t>(dimension)));
        }
        rows = std::move(keptRows);
        matrix = std::move(keptMatrix);
        rowOfId.clear();
        for (std::size_t i = 0; i < rows.size(); ++i) {
            rowOfId[rows[i].id] = i;
        }
    }

    const float* vectorOf(std::size_t row) const { return &matrix[row * static_cast<std::size_t>(dimension)]; }

    /// The graph, built (or loaded from visual.hnsw) on first use. Null when it cannot be built.
    hnswlib::HierarchicalNSW<float>* ensureGraph() const {
        if (graph) {
            return graph.get();
        }
        auto* self = const_cast<Impl*>(this);
        if (!self->space) {
            self->space = std::make_unique<UnitDotSpace>(static_cast<std::size_t>(dimension));
        }
        // A saved graph is used when it is the file flush() wrote (its SHA-256 is in the database) and checks out
        // against the current rows, element by element (savedGraph). Anything else is rebuilt from the rows, and the
        // file is dropped.
        std::error_code ec;
        const std::string savedHash = self->meta("graph_sha256");
        if (!savedHash.empty() && std::filesystem::exists(graphFile(), ec)) {
            try {
                if (auto loaded = self->savedGraph(savedHash)) {
                    loaded->setEf(options.hnswEfSearch);
                    graph = std::move(loaded);
                    return graph.get();
                }
            } catch (const std::exception&) {
                // out of memory: rebuilt below
            }
            refusedGraphs += 1;
            std::filesystem::remove(graphFile(), ec);
            [[maybe_unused]] const auto cleared = self->setMeta("graph_sha256", "");
        }
        try {
            auto built = std::make_unique<hnswlib::HierarchicalNSW<float>>(self->space.get(), std::max<std::size_t>(rows.size(), 1024),
                                                                           options.hnswM, options.hnswEfConstruction, 100, true);
            for (std::size_t i = 0; i < rows.size(); ++i) {
                built->addPoint(vectorOf(i), static_cast<hnswlib::labeltype>(rows[i].id));
            }
            built->setEf(options.hnswEfSearch);
            graph = std::move(built);
            graphDirty = true;
        } catch (const std::exception&) {
            graph.reset();
        }
        return graph.get();
    }

    /// visual.hnsw as a graph, or null when it is not one flush() wrote for exactly the current rows. hnswlib's own
    /// loader trusts its input (header sizes and offsets, neighbour ids, levels: a crafted file reads and writes out of
    /// bounds), and whoever can write the cache can write both the file and its hash. So the file is read once, and
    /// every field is checked against what this build writes for this index before the graph is filled from it:
    ///   - the header: hnswlib's layout for this dimension and M, a sane element count, level and entry point;
    ///   - every level-0 list: at most 2 M neighbours, each an element; the label unique; a live element's label one
    ///     of the rows and its vector exactly that row's; the rows all there;
    ///   - every upper level: whole lists, at most M neighbours, each an element that has that level; the entry point
    ///     on the top level; nothing after the last list.
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> savedGraph(const std::string& savedHash) {
        using hnswlib::labeltype;
        using hnswlib::linklistsizeint;
        using hnswlib::tableint;
        const std::size_t m = options.hnswM;
        const std::size_t dataSize = static_cast<std::size_t>(dimension) * sizeof(float);
        const std::size_t links0 = 2 * m * sizeof(tableint) + sizeof(linklistsizeint);
        const std::size_t linksUpper = m * sizeof(tableint) + sizeof(linklistsizeint);
        const std::size_t perElement = links0 + dataSize + sizeof(labeltype);
        if (m < 2 || m > 1024 || rows.empty() || rows.size() > kMaxGraphElements) {
            return nullptr;
        }
        // Deleted elements stay in the graph until reused: room for a few times the rows, and their upper levels.
        const std::uintmax_t limit = 128 + static_cast<std::uintmax_t>(rows.size() * 4 + 1024) * (perElement + 4 + 4 * linksUpper);
        const auto bytes = readFile(graphFile(), limit);
        if (!bytes || bytesSha256(*bytes) != savedHash) {
            return nullptr;
        }
        std::span<const std::uint8_t> in(*bytes);
        std::size_t offsetLevel0 = 0, maxElements = 0, count = 0, sizeDataPerElement = 0, labelOffset = 0, offsetData = 0;
        int maxLevel = 0;
        tableint enterpoint = 0;
        std::size_t maxM = 0, maxM0 = 0, mFile = 0, efConstruction = 0;
        double mult = 0.0;
        if (!take(in, offsetLevel0) || !take(in, maxElements) || !take(in, count) || !take(in, sizeDataPerElement) || !take(in, labelOffset) ||
            !take(in, offsetData) || !take(in, maxLevel) || !take(in, enterpoint) || !take(in, maxM) || !take(in, maxM0) || !take(in, mFile) ||
            !take(in, mult) || !take(in, efConstruction)) {
            return nullptr;
        }
        if (offsetLevel0 != 0 || mFile != m || maxM != m || maxM0 != 2 * m || offsetData != links0 || labelOffset != links0 + dataSize ||
            sizeDataPerElement != perElement || !std::isfinite(mult) || std::abs(mult - 1.0 / std::log(static_cast<double>(m))) > 1e-9 ||
            count < rows.size() || count > kMaxGraphElements || maxElements < count || maxLevel < 0 || maxLevel > kMaxGraphLevel ||
            enterpoint >= count || in.size() < count * perElement) {
            return nullptr;
        }
        const std::span<const std::uint8_t> level0 = in.first(count * perElement);
        in = in.subspan(count * perElement);
        std::vector<int> levels(count);
        std::vector<std::span<const std::uint8_t>> upper(count);
        for (std::size_t i = 0; i < count; ++i) {
            std::uint32_t size = 0;
            if (!take(in, size) || size % linksUpper != 0 || size / linksUpper > static_cast<std::size_t>(maxLevel) || in.size() < size) {
                return nullptr;
            }
            levels[i] = static_cast<int>(size / linksUpper);
            upper[i] = in.first(size);
            in = in.subspan(size);
        }
        if (!in.empty() || levels[enterpoint] != maxLevel) {
            return nullptr;
        }
        // A list: its count (16 bits), a flag byte (bit 0: deleted; level 0 only), a zero byte, then the neighbours.
        const auto listOk = [&](const std::uint8_t* list, std::size_t capacity, int level, bool flags) {
            const auto n = at<std::uint16_t>(list);
            if (n > capacity || list[3] != 0 || (list[2] & (flags ? 0xFE : 0xFF)) != 0) {
                return false;
            }
            for (std::size_t j = 0; j < n; ++j) {
                const auto neighbour = at<tableint>(list + sizeof(linklistsizeint) + j * sizeof(tableint));
                if (neighbour >= count || levels[neighbour] < level) {
                    return false;
                }
            }
            return true;
        };
        std::unordered_set<labeltype> labels;
        std::size_t live = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t* element = level0.data() + i * perElement;
            if (!listOk(element, 2 * m, 0, true)) {
                return nullptr;
            }
            const auto label = at<labeltype>(element + labelOffset);
            if (!labels.insert(label).second) {
                return nullptr;
            }
            for (std::size_t k = 0; k < static_cast<std::size_t>(dimension); ++k) {
                if (!std::isfinite(at<float>(element + offsetData + k * sizeof(float)))) {
                    return nullptr;
                }
            }
            if ((element[2] & 1) == 0) {
                const auto row = rowOfId.find(static_cast<std::int64_t>(label));
                if (row == rowOfId.end() || std::memcmp(element + offsetData, vectorOf(row->second), dataSize) != 0) {
                    return nullptr;
                }
                ++live;
            }
            for (int level = 1; level <= levels[i]; ++level) {
                if (!listOk(upper[i].data() + static_cast<std::size_t>(level - 1) * linksUpper, m, level, false)) {
                    return nullptr;
                }
            }
        }
        if (live != rows.size()) {
            return nullptr;
        }
        // Checked: the graph, filled from the checked bytes (never hnswlib's loader).
        auto g = std::make_unique<hnswlib::HierarchicalNSW<float>>(space.get(), std::max<std::size_t>(count, 1024), m, options.hnswEfConstruction,
                                                                   100, true);
        if (g->size_data_per_element_ != perElement || g->offsetData_ != offsetData || g->label_offset_ != labelOffset ||
            g->size_links_per_element_ != linksUpper || g->size_links_level0_ != links0) {
            return nullptr; // another hnswlib layout
        }
        std::memcpy(g->data_level0_memory_, level0.data(), level0.size());
        for (std::size_t i = 0; i < count; ++i) {
            g->element_levels_[i] = levels[i];
            if (levels[i] > 0) {
                g->linkLists_[i] = static_cast<char*>(std::malloc(upper[i].size() + 1)); // + 1 as addPoint allocates
                if (g->linkLists_[i] == nullptr) {
                    g->element_levels_[i] = 0; // the destructor frees the lists of elements 0 .. i - 1
                    throw std::bad_alloc();
                }
                std::memcpy(g->linkLists_[i], upper[i].data(), upper[i].size());
            }
            const std::uint8_t* element = level0.data() + i * perElement;
            g->label_lookup_[at<labeltype>(element + labelOffset)] = static_cast<tableint>(i);
            if ((element[2] & 1) != 0) {
                g->num_deleted_ += 1;
                g->deleted_elements.insert(static_cast<tableint>(i));
            }
            g->cur_element_count = i + 1; // the destructor frees exactly the lists set so far
        }
        g->maxlevel_ = maxLevel;
        g->enterpoint_node_ = enterpoint;
        return g;
    }

    void addToGraph(std::size_t firstRow) {
        if (!graph) {
            return; // built with every row on next need
        }
        try {
            for (std::size_t i = firstRow; i < rows.size(); ++i) {
                if (graph->getCurrentElementCount() >= graph->getMaxElements() && graph->getDeletedCount() == 0) {
                    graph->resizeIndex(graph->getMaxElements() * 2);
                }
                graph->addPoint(vectorOf(i), static_cast<hnswlib::labeltype>(rows[i].id), graph->getDeletedCount() > 0);
            }
            graphDirty = true;
        } catch (const std::exception&) {
            graph.reset();
        }
    }
};

// --- SearchIndex ----------------------------------------------------------------------------------------------------

SearchIndex::SearchIndex(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

SearchIndex::~SearchIndex() {
    if (m_impl) {
        (void)flush();
    }
}

std::filesystem::path SearchIndex::defaultDir() { return userCacheDir() / "search" / "v1"; }

std::expected<std::unique_ptr<SearchIndex>, Error> SearchIndex::open(const std::filesystem::path& dir, std::string modelVersion,
                                                                     int dimension, SearchIndexOptions options) {
    if (dimension <= 0 || dimension > 65536) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "search index: invalid vector dimension"));
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return ioError("cannot create " + pathToUtf8(dir) + ": " + ec.message());
    }
    const auto attempt = [&]() -> std::expected<std::unique_ptr<Impl>, Error> {
        auto impl = std::make_unique<Impl>();
        impl->dir = dir;
        impl->modelVersion = modelVersion;
        impl->dimension = dimension;
        impl->options = options;
        if (auto ok = impl->openDatabase(dir / "index.sqlite"); !ok) {
            return std::unexpected(ok.error());
        }
        if (auto ok = impl->prepare(); !ok) {
            return std::unexpected(ok.error());
        }
        return impl;
    };
    auto impl = attempt();
    if (!impl && impl.error().code == ErrorCode::IoError) {
        // A damaged database (or one of a newer, incompatible layout) is a cache: start over once.
        for (const char* name : {"index.sqlite", "index.sqlite-wal", "index.sqlite-shm", "visual.hnsw"}) {
            std::filesystem::remove(dir / name, ec);
        }
        impl = attempt();
    }
    if (!impl) {
        return std::unexpected(impl.error());
    }
    return std::unique_ptr<SearchIndex>(new SearchIndex(std::move(*impl)));
}

const std::string& SearchIndex::modelVersion() const { return m_impl->modelVersion; }

int SearchIndex::dimension() const { return m_impl->dimension; }

std::optional<IndexedSource> SearchIndex::source(std::uint64_t fingerprint) const {
    std::lock_guard lock(m_impl->mutex);
    Statement s(m_impl->db,
                "SELECT name, visual_version, transcript_key, "
                "(SELECT count(*) FROM keyframes k WHERE k.fingerprint = sources.fingerprint), "
                "(SELECT count(*) FROM phrases p WHERE p.fingerprint = sources.fingerprint) "
                "FROM sources WHERE fingerprint = ?1");
    if (!s.ok()) {
        return std::nullopt;
    }
    s.bind(1, asKey(fingerprint));
    if (s.step() != SQLITE_ROW) {
        return std::nullopt;
    }
    return IndexedSource{fingerprint, s.text(0), s.text(1), s.text(2), static_cast<std::size_t>(s.integer(3)),
                         static_cast<std::size_t>(s.integer(4))};
}

std::vector<IndexedSource> SearchIndex::sources() const {
    std::lock_guard lock(m_impl->mutex);
    std::vector<IndexedSource> out;
    Statement s(m_impl->db,
                "SELECT fingerprint, name, visual_version, transcript_key, "
                "(SELECT count(*) FROM keyframes k WHERE k.fingerprint = sources.fingerprint), "
                "(SELECT count(*) FROM phrases p WHERE p.fingerprint = sources.fingerprint) FROM sources");
    while (s.ok() && s.step() == SQLITE_ROW) {
        out.push_back(IndexedSource{fromKey(s.integer(0)), s.text(1), s.text(2), s.text(3), static_cast<std::size_t>(s.integer(4)),
                                    static_cast<std::size_t>(s.integer(5))});
    }
    return out;
}

std::expected<void, Error> SearchIndex::putKeyframes(std::uint64_t fingerprint, const std::string& name,
                                                     std::span<const IndexedKeyframe> keyframes, std::string_view variant) {
    std::lock_guard lock(m_impl->mutex);
    Impl& d = *m_impl;
    for (const IndexedKeyframe& k : keyframes) {
        if (k.vector.size() != static_cast<std::size_t>(d.dimension)) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, "search index: a keyframe vector has the wrong dimension"));
        }
    }
    if (auto ok = d.exec("BEGIN IMMEDIATE"); !ok) {
        return ok;
    }
    const auto rollback = [&](Error error) -> std::expected<void, Error> {
        (void)d.exec("ROLLBACK");
        return std::unexpected(std::move(error));
    };
    if (auto ok = d.upsertSource(fingerprint, name); !ok) {
        return rollback(ok.error());
    }
    {
        Statement del(d.db, "DELETE FROM keyframes WHERE fingerprint = ?1");
        Statement version(d.db, "UPDATE sources SET visual_version = ?2 WHERE fingerprint = ?1");
        if (!del.ok() || !version.ok()) {
            return rollback(Error(ErrorCode::IoError, "search index: " + del.error()));
        }
        del.bind(1, asKey(fingerprint));
        version.bind(1, asKey(fingerprint)).bind(2, d.modelVersion + std::string(variant));
        if (del.step() != SQLITE_DONE || version.step() != SQLITE_DONE) {
            return rollback(Error(ErrorCode::IoError, std::string("search index: ") + sqlite3_errmsg(d.db)));
        }
    }
    std::vector<std::int64_t> ids;
    {
        Statement insert(d.db, "INSERT INTO keyframes(fingerprint, t0, t1, time, vector) VALUES(?1, ?2, ?3, ?4, ?5)");
        if (!insert.ok()) {
            return rollback(Error(ErrorCode::IoError, "search index: " + insert.error()));
        }
        for (const IndexedKeyframe& k : keyframes) {
            insert.bind(1, asKey(fingerprint)).bind(2, k.range.start.value).bind(3, k.range.end().value).bind(4, k.time.value);
            insert.bindBlob(5, k.vector.data(), k.vector.size() * sizeof(float));
            if (insert.step() != SQLITE_DONE) {
                return rollback(Error(ErrorCode::IoError, "search index: " + insert.error()));
            }
            ids.push_back(sqlite3_last_insert_rowid(d.db));
            insert.reset();
        }
    }
    if (auto ok = d.exec("COMMIT"); !ok) {
        return rollback(ok.error());
    }
    d.forgetRows(fingerprint);
    const std::size_t first = d.rows.size();
    for (std::size_t i = 0; i < keyframes.size(); ++i) {
        d.rowOfId[ids[i]] = d.rows.size();
        d.rows.push_back(Impl::Row{ids[i], fingerprint, keyframes[i].range, keyframes[i].time});
        d.matrix.insert(d.matrix.end(), keyframes[i].vector.begin(), keyframes[i].vector.end());
    }
    d.addToGraph(first);
    return {};
}

std::expected<void, Error> SearchIndex::putPhrases(std::uint64_t fingerprint, const std::string& name,
                                                   const std::string& transcriptKey, const std::string& language,
                                                   std::span<const IndexedPhrase> phrases) {
    std::lock_guard lock(m_impl->mutex);
    Impl& d = *m_impl;
    if (auto ok = d.exec("BEGIN IMMEDIATE"); !ok) {
        return ok;
    }
    const auto rollback = [&](std::string message) -> std::expected<void, Error> {
        (void)d.exec("ROLLBACK");
        return ioError(std::move(message));
    };
    if (auto ok = d.upsertSource(fingerprint, name); !ok) {
        return rollback(ok.error().message);
    }
    Statement delFts(d.db, "DELETE FROM phrases_fts WHERE rowid IN (SELECT id FROM phrases WHERE fingerprint = ?1)");
    Statement del(d.db, "DELETE FROM phrases WHERE fingerprint = ?1");
    Statement key(d.db, "UPDATE sources SET transcript_key = ?2 WHERE fingerprint = ?1");
    Statement insert(d.db, "INSERT INTO phrases(fingerprint, t0, t1, words) VALUES(?1, ?2, ?3, ?4)");
    Statement insertFts(d.db, "INSERT INTO phrases_fts(rowid, spaced, compact) VALUES(?1, ?2, ?3)");
    if (!delFts.ok() || !del.ok() || !key.ok() || !insert.ok() || !insertFts.ok()) {
        return rollback(sqlite3_errmsg(d.db));
    }
    delFts.bind(1, asKey(fingerprint));
    del.bind(1, asKey(fingerprint));
    key.bind(1, asKey(fingerprint)).bind(2, transcriptKey);
    if (delFts.step() != SQLITE_DONE || del.step() != SQLITE_DONE || key.step() != SQLITE_DONE) {
        return rollback(sqlite3_errmsg(d.db));
    }
    const bool compact = writtenWithoutSpaces(language);
    for (const IndexedPhrase& phrase : phrases) {
        insert.bind(1, asKey(fingerprint)).bind(2, phrase.range.start.value).bind(3, phrase.range.end().value).bind(4, wordsJson(phrase));
        if (insert.step() != SQLITE_DONE) {
            return rollback(insert.error());
        }
        insert.reset();
        insertFts.bind(1, sqlite3_last_insert_rowid(d.db))
            .bind(2, lowercaseUtf8(joined(phrase.words, " ")))
            .bind(3, compact ? lowercaseUtf8(joined(phrase.words, "")) : std::string());
        if (insertFts.step() != SQLITE_DONE) {
            return rollback(insertFts.error());
        }
        insertFts.reset();
    }
    if (auto ok = d.exec("COMMIT"); !ok) {
        return rollback(ok.error().message);
    }
    return {};
}

std::expected<void, Error> SearchIndex::removeSource(std::uint64_t fingerprint) {
    std::lock_guard lock(m_impl->mutex);
    Impl& d = *m_impl;
    if (auto ok = d.exec("BEGIN IMMEDIATE"); !ok) {
        return ok;
    }
    for (const char* sql : {"DELETE FROM phrases_fts WHERE rowid IN (SELECT id FROM phrases WHERE fingerprint = ?1)",
                            "DELETE FROM phrases WHERE fingerprint = ?1", "DELETE FROM keyframes WHERE fingerprint = ?1",
                            "DELETE FROM sources WHERE fingerprint = ?1"}) {
        Statement s(d.db, sql);
        if (!s.ok()) {
            (void)d.exec("ROLLBACK");
            return ioError(s.error());
        }
        s.bind(1, asKey(fingerprint));
        if (s.step() != SQLITE_DONE) {
            const std::string message = s.error();
            (void)d.exec("ROLLBACK");
            return ioError(message);
        }
    }
    if (auto ok = d.exec("COMMIT"); !ok) {
        (void)d.exec("ROLLBACK");
        return ok;
    }
    d.forgetRows(fingerprint);
    return {};
}

std::expected<std::size_t, Error> SearchIndex::prune(const std::unordered_set<std::uint64_t>& keep) {
    std::vector<std::uint64_t> gone;
    for (const IndexedSource& s : sources()) {
        if (!keep.contains(s.fingerprint)) {
            gone.push_back(s.fingerprint);
        }
    }
    for (const std::uint64_t fingerprint : gone) {
        if (auto ok = removeSource(fingerprint); !ok) {
            return std::unexpected(ok.error());
        }
    }
    return gone.size();
}

std::expected<std::vector<VisualMatch>, Error> SearchIndex::nearest(std::span<const float> query, std::size_t k,
                                                                    const std::unordered_set<std::uint64_t>& allowed) const {
    std::lock_guard lock(m_impl->mutex);
    const Impl& d = *m_impl;
    std::vector<VisualMatch> out;
    if (query.size() != static_cast<std::size_t>(d.dimension)) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "search index: the query vector has the wrong dimension"));
    }
    if (k == 0 || allowed.empty() || d.rows.empty()) {
        return out;
    }
    std::size_t candidates = 0;
    for (const Impl::Row& row : d.rows) {
        candidates += allowed.contains(row.fingerprint) ? 1 : 0;
    }
    d.lastUsedGraph = false;
    if (candidates > d.options.exactLimit) {
        if (auto* graph = d.ensureGraph()) {
            struct Allowed : hnswlib::BaseFilterFunctor {
                const Impl* index;
                const std::unordered_set<std::uint64_t>* allowed;
                bool operator()(hnswlib::labeltype label) override {
                    const auto it = index->rowOfId.find(static_cast<std::int64_t>(label));
                    return it != index->rowOfId.end() && allowed->contains(index->rows[it->second].fingerprint);
                }
            } filter;
            filter.index = &d;
            filter.allowed = &allowed;
            try {
                graph->setEf(std::max(d.options.hnswEfSearch, k));
                auto found = graph->searchKnn(query.data(), k, &filter);
                while (!found.empty()) {
                    const auto [distance, label] = found.top();
                    found.pop();
                    const Impl::Row& row = d.rows[d.rowOfId.at(static_cast<std::int64_t>(label))];
                    out.push_back(VisualMatch{row.fingerprint, row.range, row.time, 1.0f - distance});
                }
                std::ranges::reverse(out); // the queue pops the farthest first
                d.lastUsedGraph = true;
                return out;
            } catch (const std::exception&) {
                d.graph.reset(); // fall back to the scan
            }
        }
    }
    std::vector<std::pair<float, std::size_t>> scored;
    scored.reserve(candidates);
    for (std::size_t i = 0; i < d.rows.size(); ++i) {
        if (allowed.contains(d.rows[i].fingerprint)) {
            scored.emplace_back(dot(query, std::span<const float>(d.vectorOf(i), static_cast<std::size_t>(d.dimension))), i);
        }
    }
    const std::size_t n = std::min(k, scored.size());
    std::partial_sort(scored.begin(), scored.begin() + static_cast<std::ptrdiff_t>(n), scored.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
    for (std::size_t i = 0; i < n; ++i) {
        const Impl::Row& row = d.rows[scored[i].second];
        out.push_back(VisualMatch{row.fingerprint, row.range, row.time, scored[i].first});
    }
    return out;
}

std::expected<std::vector<TextMatch>, Error> SearchIndex::matchText(const TextQuery& query, std::size_t k,
                                                                    const std::unordered_set<std::uint64_t>& allowed) const {
    std::lock_guard lock(m_impl->mutex);
    Impl& d = *m_impl;
    std::vector<TextMatch> out;
    if (query.empty() || k == 0 || allowed.empty()) {
        return out;
    }
    // The allowed sources as a temporary table, so the filter runs inside the query.
    if (auto ok = d.exec("CREATE TEMP TABLE IF NOT EXISTS allowed_sources(f INTEGER PRIMARY KEY); DELETE FROM temp.allowed_sources;");
        !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = d.exec("BEGIN"); !ok) {
        return std::unexpected(ok.error());
    }
    {
        Statement insert(d.db, "INSERT OR IGNORE INTO temp.allowed_sources(f) VALUES(?1)");
        for (const std::uint64_t fingerprint : allowed) {
            insert.bind(1, asKey(fingerprint));
            if (insert.step() != SQLITE_DONE) {
                const std::string message = insert.error();
                (void)d.exec("ROLLBACK");
                return ioError(message);
            }
            insert.reset();
        }
    }
    if (auto ok = d.exec("COMMIT"); !ok) {
        (void)d.exec("ROLLBACK");
        return std::unexpected(ok.error());
    }
    std::string sql;
    if (!query.match.empty()) {
        sql = "SELECT p.fingerprint, p.t0, p.t1, p.words, bm25(phrases_fts) AS score, phrases_fts.compact = '' FROM phrases_fts "
              "JOIN phrases p ON p.id = phrases_fts.rowid "
              "WHERE phrases_fts MATCH ?1 AND p.fingerprint IN (SELECT f FROM temp.allowed_sources) "
              "ORDER BY score, p.fingerprint, p.t0 LIMIT ?2";
    } else {
        // Terms under three characters: LIKE over the indexed text (the documented fallback; a scan).
        std::string where;
        for (std::size_t i = 0; i < query.likeTerms.size(); ++i) {
            const std::string n = std::to_string(i + 3);
            where += (i == 0 ? "" : " OR ");
            where += "f.spaced LIKE ?" + n + " ESCAPE '\\' OR f.compact LIKE ?" + n + " ESCAPE '\\'";
        }
        sql = "SELECT p.fingerprint, p.t0, p.t1, p.words, 0.0, f.compact = '' FROM phrases p JOIN phrases_fts f ON f.rowid = p.id "
              "WHERE ?1 IS NOT NULL AND (" + where + ") AND p.fingerprint IN (SELECT f FROM temp.allowed_sources) "
              "ORDER BY p.fingerprint, p.t0 LIMIT ?2";
    }
    Statement s(d.db, sql.c_str());
    if (!s.ok()) {
        return ioError(s.error());
    }
    s.bind(1, query.match.empty() ? std::string("like") : query.match).bind(2, static_cast<std::int64_t>(k));
    for (std::size_t i = 0; i < query.likeTerms.size() && query.match.empty(); ++i) {
        s.bind(static_cast<int>(i + 3), query.likeTerms[i]);
    }
    int rc = SQLITE_ROW;
    while ((rc = s.step()) == SQLITE_ROW) {
        TextMatch match;
        match.fingerprint = fromKey(s.integer(0));
        match.range = TimeRange::fromStartEnd(Ticks{s.integer(1)}, Ticks{s.integer(2)});
        readWords(s.text(3), match);
        match.relevance = -s.number(4);
        match.spaced = s.integer(5) != 0;
        out.push_back(std::move(match));
    }
    if (rc != SQLITE_DONE) {
        return ioError(sqlite3_errmsg(d.db));
    }
    return out;
}

std::size_t SearchIndex::keyframeCount() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->rows.size();
}

std::size_t SearchIndex::phraseCount() const {
    std::lock_guard lock(m_impl->mutex);
    Statement s(m_impl->db, "SELECT count(*) FROM phrases");
    return s.ok() && s.step() == SQLITE_ROW ? static_cast<std::size_t>(s.integer(0)) : 0;
}

bool SearchIndex::usedGraph() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->lastUsedGraph;
}

std::expected<void, Error> SearchIndex::flush() {
    std::lock_guard lock(m_impl->mutex);
    Impl& d = *m_impl;
    if (!d.graph || !d.graphDirty) {
        return {};
    }
    std::error_code ec;
    const std::filesystem::path temp = d.dir / "visual.hnsw.tmp";
    try {
        d.graph->saveIndex(temp.string());
    } catch (const std::exception& e) {
        std::filesystem::remove(temp, ec);
        return ioError(std::string("cannot save the vector graph: ") + e.what());
    }
    const std::string hash = fileSha256(temp);
    if (hash.empty()) {
        std::filesystem::remove(temp, ec);
        return ioError("cannot read back the vector graph");
    }
    std::filesystem::rename(temp, d.graphFile(), ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return ioError("cannot save the vector graph: " + ec.message());
    }
    if (auto stored = d.setMeta("graph_sha256", hash); !stored) {
        return stored;
    }
    d.graphDirty = false;
    return {};
}

} // namespace wabcut::ai::search
