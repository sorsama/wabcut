#include <wabcut/audio/dsp/Spectrogram.h>

#include <wabcut/audio/dsp/Fft.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace wabcut::audio::dsp {

std::uint8_t quantizeSpectrogramDb(double db) noexcept {
    const double u = (db - kSpectrogramFloorDb) / (0.0 - kSpectrogramFloorDb);
    return static_cast<std::uint8_t>(std::lround(std::clamp(u, 0.0, 1.0) * 255.0));
}

double spectrogramDb(std::uint8_t level) noexcept {
    return kSpectrogramFloorDb + (0.0 - kSpectrogramFloorDb) * static_cast<double>(level) / 255.0;
}

struct SpectrogramAnalyzer::Impl {
    explicit Impl(int size) : fft(size) {
        const int n = fft.size();
        window.resize(static_cast<std::size_t>(n));
        double sum = 0.0;
        for (int i = 0; i < n; ++i) {
            const double w = std::sqrt(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / n));
            window[static_cast<std::size_t>(i)] = w;
            sum += w;
        }
        // A full-scale sine has the magnitude sum(w) / 2 in its bin.
        normalise = sum > 0.0 ? 2.0 / sum : 1.0;
        frame.assign(static_cast<std::size_t>(n), 0.0);
        bins.assign(static_cast<std::size_t>(fft.bins()), {});
    }
    Fft fft;
    std::vector<double> window;
    double normalise = 1.0;
    std::vector<double> frame;
    std::vector<std::complex<double>> bins;
};

SpectrogramAnalyzer::SpectrogramAnalyzer(int fftSize)
    : m_impl(std::make_unique<Impl>(std::clamp(fftSize, 64, 65536))) {}
SpectrogramAnalyzer::~SpectrogramAnalyzer() = default;

int SpectrogramAnalyzer::fftSize() const noexcept { return m_impl->fft.size(); }
int SpectrogramAnalyzer::hop() const noexcept { return m_impl->fft.size() / 4; }
int SpectrogramAnalyzer::bins() const noexcept { return m_impl->fft.bins(); }

void SpectrogramAnalyzer::analyse(const float* samples, std::uint8_t* out) {
    Impl& s = *m_impl;
    const int n = s.fft.size();
    for (int i = 0; i < n; ++i) {
        s.frame[static_cast<std::size_t>(i)] = static_cast<double>(samples[i]) * s.window[static_cast<std::size_t>(i)];
    }
    s.fft.forwardReal(s.frame.data(), s.bins.data());
    const int bins = s.fft.bins();
    for (int b = 0; b < bins; ++b) {
        const std::complex<double> v = s.bins[static_cast<std::size_t>(b)];
        const double power = (v.real() * v.real() + v.imag() * v.imag()) * s.normalise * s.normalise;
        const double db = power > 1.0e-18 ? 10.0 * std::log10(power) : kSpectrogramFloorDb;
        out[b] = quantizeSpectrogramDb(db);
    }
}

SpectrogramCache::SpectrogramCache(std::shared_ptr<SampleSource> source, int sampleRate, int fftSize, std::size_t budgetBytes)
    : m_source(std::move(source)), m_rate(std::max(sampleRate, 1)), m_fft(fftSize), m_hop(fftSize / 4), m_bins(fftSize / 2 + 1),
      m_budget(std::max<std::size_t>(budgetBytes, 4U * static_cast<std::size_t>(kSpectrogramTileColumns) * static_cast<std::size_t>(fftSize / 2 + 1))) {
    const std::int64_t length = m_source ? m_source->length() : 0;
    // One column per hop, the first centred on sample 0.
    m_baseColumns = length > 0 ? (length + m_hop - 1) / m_hop + 1 : 0;
}

std::int64_t SpectrogramCache::columns(int level) const noexcept {
    const std::int64_t span = std::int64_t{1} << std::clamp(level, 0, 40);
    return (m_baseColumns + span - 1) / span;
}

std::int64_t SpectrogramCache::tiles(int level) const noexcept {
    return (columns(level) + kSpectrogramTileColumns - 1) / kSpectrogramTileColumns;
}

int SpectrogramCache::maxLevel() const noexcept {
    int level = 0;
    while (level < 40 && columns(level) > kSpectrogramTileColumns) {
        ++level;
    }
    return level;
}

int SpectrogramCache::levelFor(double baseColumnsPerPixel) noexcept {
    if (!(baseColumnsPerPixel > 1.0)) {
        return 0;
    }
    return std::clamp(static_cast<int>(std::floor(std::log2(baseColumnsPerPixel))), 0, 40);
}

std::shared_ptr<SpectrogramTile> SpectrogramCache::compute(int level, std::int64_t index, std::stop_token stop) {
    auto out = std::make_shared<SpectrogramTile>();
    out->level = level;
    out->index = index;
    out->bins = m_bins;
    const std::int64_t total = columns(level);
    const std::int64_t firstColumn = index * kSpectrogramTileColumns;
    if (index < 0 || firstColumn >= total) {
        out->columns = 0;
        return out;
    }
    out->columns = static_cast<int>(std::min<std::int64_t>(kSpectrogramTileColumns, total - firstColumn));
    out->levels.assign(static_cast<std::size_t>(out->columns) * static_cast<std::size_t>(m_bins), 0);

    SpectrogramAnalyzer analyzer(m_fft);
    const std::int64_t pool = std::int64_t{1} << level;
    const std::int64_t firstBase = firstColumn * pool;
    const std::int64_t lastBase = std::min(m_baseColumns, (firstColumn + out->columns) * pool); // exclusive
    constexpr std::int64_t kChunk = 64;                                                    // base columns per read
    std::vector<float> samples;
    std::vector<std::uint8_t> row(static_cast<std::size_t>(m_bins));
    for (std::int64_t base = firstBase; base < lastBase; base += kChunk) {
        if (stop.stop_requested()) {
            return nullptr;
        }
        const std::int64_t count = std::min(kChunk, lastBase - base);
        const std::int64_t start = base * m_hop - m_fft / 2;
        const std::int64_t needed = (count - 1) * m_hop + m_fft;
        samples.assign(static_cast<std::size_t>(needed), 0.0f);
        {
            std::lock_guard lock(m_sourceMutex);
            m_source->read(start, samples.data(), needed);
        }
        for (std::int64_t k = 0; k < count; ++k) {
            analyzer.analyse(samples.data() + k * m_hop, row.data());
            const auto column = static_cast<std::size_t>(((base + k) / pool) - firstColumn);
            std::uint8_t* dst = out->levels.data() + column * static_cast<std::size_t>(m_bins);
            for (int b = 0; b < m_bins; ++b) {
                dst[b] = std::max(dst[b], row[static_cast<std::size_t>(b)]);
            }
        }
    }
    return out;
}

void SpectrogramCache::insert(const std::shared_ptr<const SpectrogramTile>& tile) {
    std::lock_guard lock(m_mutex);
    const Key key = keyOf(tile->level, tile->index);
    if (const auto it = m_tiles.find(key); it != m_tiles.end()) {
        m_lru.splice(m_lru.begin(), m_lru, it->second.position);
        return;
    }
    m_lru.push_front(key);
    m_tiles.emplace(key, Entry{tile, m_lru.begin()});
    m_bytes += tile->levels.size();
    ++m_computed;
    while (m_bytes > m_budget && m_lru.size() > 1) {
        const Key oldest = m_lru.back();
        const auto it = m_tiles.find(oldest);
        if (it != m_tiles.end()) {
            m_bytes -= it->second.tile->levels.size();
            m_tiles.erase(it);
        }
        m_lru.pop_back();
    }
}

std::shared_ptr<const SpectrogramTile> SpectrogramCache::peek(int level, std::int64_t index) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_tiles.find(keyOf(level, index));
    return it != m_tiles.end() ? it->second.tile : nullptr;
}

std::shared_ptr<const SpectrogramTile> SpectrogramCache::tile(int level, std::int64_t index, std::stop_token stop) {
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_tiles.find(keyOf(level, index));
        if (it != m_tiles.end()) {
            m_lru.splice(m_lru.begin(), m_lru, it->second.position);
            return it->second.tile;
        }
    }
    std::shared_ptr<const SpectrogramTile> made = compute(level, index, stop);
    if (made && made->columns > 0) {
        insert(made);
    }
    return made;
}

std::size_t SpectrogramCache::bytes() const {
    std::lock_guard lock(m_mutex);
    return m_bytes;
}

void SpectrogramCache::clear() {
    std::lock_guard lock(m_mutex);
    m_tiles.clear();
    m_lru.clear();
    m_bytes = 0;
}

} // namespace wabcut::audio::dsp
