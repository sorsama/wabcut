#include "MasterBus.h"

#include "Fader.h"
#include "LiveFader.h"

#include <algorithm>

namespace wabcut::playback::detail {

MasterBus::MasterBus(int channels, int sampleRate)
    : m_channels(channels), m_sampleRate(sampleRate), m_rampFrames(std::max(sampleRate / 100, 1)) {
    m_owned = std::make_unique<Program>(channels, sampleRate);
    m_current.store(m_owned.get(), std::memory_order_seq_cst);
}

MasterBus::~MasterBus() = default;

void MasterBus::update(const EffectStack& stack, const ParamValue& volume) {
    collect();
    if (stack == m_owned->stack && volume == m_owned->volume) {
        return;
    }
    auto next = std::make_unique<Program>(m_channels, m_sampleRate);
    next->stack = stack;
    next->volume = volume;
    next->chain.bind(next->stack, &m_owned->chain); // shares the processors of unchanged instances
    m_current.store(next.get(), std::memory_order_seq_cst);
    // If the audio thread was outside a render now, every later render sees the new program; otherwise it is done
    // with the old one once the counter moves past this value.
    const std::uint64_t mark = m_renderSeq.load(std::memory_order_seq_cst);
    m_retired.emplace_back(std::move(m_owned), mark);
    m_owned = std::move(next);
}

void MasterBus::collect() {
    const std::uint64_t now = m_renderSeq.load(std::memory_order_seq_cst);
    std::erase_if(m_retired, [now](const auto& retired) {
        const std::uint64_t mark = retired.second;
        return mark % 2 == 0 || now > mark;
    });
}

void MasterBus::beginRender() noexcept {
    m_renderSeq.fetch_add(1, std::memory_order_seq_cst); // odd: rendering
    m_active = m_current.load(std::memory_order_seq_cst);
}

void MasterBus::endRender() noexcept {
    m_lastProgram = m_active;
    m_active = nullptr;
    m_renderSeq.fetch_add(1, std::memory_order_seq_cst); // even: done
}

int MasterBus::latencyFrames() const noexcept { return m_active != nullptr ? m_active->chain.latencyFrames() : 0; }

bool MasterBus::empty() const noexcept {
    return m_active == nullptr || (m_active->chain.empty() && m_active->volume == ParamValue());
}

void MasterBus::restart(Ticks origin, std::int64_t ticksPerFrame) noexcept {
    if (m_active != nullptr) {
        m_active->chain.reset();
    }
    m_clock = audio::CurveClock{origin, ticksPerFrame, nullptr};
    m_frame = 0;
}

void MasterBus::process(std::span<float> block, int channels) noexcept {
    if (m_active == nullptr || channels != m_channels) {
        return;
    }
    const auto frames = static_cast<int>(block.size() / static_cast<std::size_t>(channels));
    m_active->chain.process(block.data(), frames, m_frame, m_clock);
    // An automation pass's live master fader replaces the evaluated one while it is set (and ramps back after).
    const bool live = m_live != nullptr && m_live->engaged() &&
                      m_live->apply(block.data(), frames, channels, faderGain(m_active->volume.valueAt(m_clock.timelineAt(m_frame))),
                                    audio::StereoGains{}, m_rampFrames, channels == 2);
    if (!live) {
        detail::applyFader(m_active->volume, nullptr, m_frame, m_frame + frames, block.data(), channels,
                           [this](std::int64_t f) { return m_clock.timelineAt(f); });
    }
    m_frame += frames;
}

} // namespace wabcut::playback::detail
