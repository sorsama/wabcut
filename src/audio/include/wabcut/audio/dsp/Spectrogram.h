#pragma once

// 0.7 (card 047): the analysis behind the Spectral Repair view. A spectrogram is the short-time Fourier transform of a
// clip's sound (sqrt-Hann windows, 75 % overlap, 1024 .. 4096 points) as 8-bit levels: 0 .. 255 stand for kSpectrogramFloorDb
// .. 0 dB under the level of a full-scale sine (0.47 dB per step). It is kept in TILES so a long recording stays inside a
// memory budget:
//   * level 0 has one column per STFT hop; level L pools 2^L neighbouring columns into one (the loudest of them, so a
//     click survives zooming out);
//   * a tile is kSpectrogramTileColumns columns of `bins` levels; the cache computes a tile on demand from the sound, keeps
//     the most recently used ones within a byte budget and gives the rest back to be computed again.
// Reading the sound is the host's (SampleSource: mono, any position, silence outside the media), so the same code serves a
// clip's decoded media and a synthetic signal in tests. Everything here is thread-safe where said and Qt-free.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace wabcut::audio::dsp {

inline constexpr double kSpectrogramFloorDb = -120.0;
inline constexpr int kSpectrogramTileColumns = 256;

/// dB (re a full-scale sine) <-> the 8-bit level.
std::uint8_t quantizeSpectrogramDb(double db) noexcept;
double spectrogramDb(std::uint8_t level) noexcept;

/// The sound a spectrogram is made of: mono samples at `sampleRate`, `length()` samples long.
class SampleSource {
public:
    virtual ~SampleSource() = default;
    virtual std::int64_t length() const = 0;
    /// Fills `out[0, count)` with the samples from `start` on; positions outside [0, length()) read as silence. Called from
    /// any one thread at a time. A failure to read is silence too.
    virtual void read(std::int64_t start, float* out, std::int64_t count) = 0;
};

/// STFT analysis of one frame at a time.
class SpectrogramAnalyzer {
public:
    explicit SpectrogramAnalyzer(int fftSize);
    ~SpectrogramAnalyzer();
    SpectrogramAnalyzer(const SpectrogramAnalyzer&) = delete;
    SpectrogramAnalyzer& operator=(const SpectrogramAnalyzer&) = delete;

    int fftSize() const noexcept;
    int hop() const noexcept;
    int bins() const noexcept;
    /// The levels of the `fftSize()` samples at `samples`: bins() bytes into `out`.
    void analyse(const float* samples, std::uint8_t* out);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// kSpectrogramTileColumns columns (fewer in the last tile of a level) of bins() levels, column after column.
struct SpectrogramTile {
    int level = 0;
    std::int64_t index = 0;
    int columns = 0;
    int bins = 0;
    std::vector<std::uint8_t> levels; ///< columns * bins
    std::uint8_t at(int column, int bin) const { return levels[static_cast<std::size_t>(column) * static_cast<std::size_t>(bins) + static_cast<std::size_t>(bin)]; }
    const std::uint8_t* column(int c) const { return levels.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(bins); }
};

class SpectrogramCache {
public:
    /// `budgetBytes`: how much computed tiles may take (at least a few tiles are always kept).
    SpectrogramCache(std::shared_ptr<SampleSource> source, int sampleRate, int fftSize, std::size_t budgetBytes);

    int sampleRate() const noexcept { return m_rate; }
    int fftSize() const noexcept { return m_fft; }
    int hop() const noexcept { return m_hop; }
    int bins() const noexcept { return m_bins; }
    double columnSeconds() const noexcept { return static_cast<double>(m_hop) / m_rate; }
    /// STFT frames (level-0 columns) of the whole sound: one per hop, the first centred on sample 0.
    std::int64_t baseColumns() const noexcept { return m_baseColumns; }
    /// Columns and tiles of a level.
    std::int64_t columns(int level) const noexcept;
    std::int64_t tiles(int level) const noexcept;
    /// The coarsest level worth having: the whole sound in one tile.
    int maxLevel() const noexcept;
    /// The level whose columns are at most `baseColumnsPerPixel` level-0 columns wide (0 when zoomed in).
    static int levelFor(double baseColumnsPerPixel) noexcept;

    /// A tile, computed from the sound when it is not cached (blocking; checks `stop` between blocks and returns null when
    /// it fired). Thread-safe; two threads asking for the same tile may both compute it.
    std::shared_ptr<const SpectrogramTile> tile(int level, std::int64_t index, std::stop_token stop = {});
    /// The cached tile, or null (never computes).
    std::shared_ptr<const SpectrogramTile> peek(int level, std::int64_t index) const;

    std::size_t bytes() const;
    std::size_t budget() const noexcept { return m_budget; }
    std::uint64_t tilesComputed() const noexcept { return m_computed.load(); }
    void clear();

private:
    using Key = std::uint64_t;
    static Key keyOf(int level, std::int64_t index) noexcept {
        return (static_cast<Key>(level) << 48) | static_cast<Key>(index & 0xFFFFFFFFFFFFLL);
    }
    std::shared_ptr<SpectrogramTile> compute(int level, std::int64_t index, std::stop_token stop);
    void insert(const std::shared_ptr<const SpectrogramTile>& tile);

    std::shared_ptr<SampleSource> m_source;
    int m_rate;
    int m_fft;
    int m_hop;
    int m_bins;
    std::int64_t m_baseColumns = 0;
    std::size_t m_budget;
    std::mutex m_sourceMutex; ///< one read at a time
    mutable std::mutex m_mutex;
    std::list<Key> m_lru; ///< most recently used first
    struct Entry {
        std::shared_ptr<const SpectrogramTile> tile;
        std::list<Key>::iterator position;
    };
    std::unordered_map<Key, Entry> m_tiles;
    std::size_t m_bytes = 0;
    std::atomic<std::uint64_t> m_computed{0};
};

} // namespace wabcut::audio::dsp
