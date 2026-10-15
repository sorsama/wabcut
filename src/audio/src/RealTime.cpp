#include <wabcut/audio/RealTime.h>

#if defined(_M_X64) || defined(__x86_64__)
#include <pmmintrin.h> // _MM_DENORMALS_ZERO_ON
#include <xmmintrin.h> // _mm_getcsr, _mm_setcsr, _MM_FLUSH_ZERO_ON
#define WABCUT_AUDIO_MXCSR 1
#endif

namespace wabcut::audio::rt {

namespace {
thread_local int t_realtimeDepth = 0;
std::atomic<std::uint64_t> g_lockViolations{0};

#ifdef WABCUT_AUDIO_MXCSR
constexpr unsigned int kDenormalBits = _MM_FLUSH_ZERO_ON | _MM_DENORMALS_ZERO_ON;
#endif
} // namespace

bool inRealtimeSection() noexcept { return t_realtimeDepth > 0; }

ScopedRealtimeSection::ScopedRealtimeSection() noexcept { ++t_realtimeDepth; }

ScopedRealtimeSection::~ScopedRealtimeSection() { --t_realtimeDepth; }

ScopedDenormalsOff::ScopedDenormalsOff() noexcept {
#ifdef WABCUT_AUDIO_MXCSR
    const unsigned int csr = _mm_getcsr();
    m_saved = csr & kDenormalBits;
    _mm_setcsr(csr | kDenormalBits);
#endif
}

ScopedDenormalsOff::~ScopedDenormalsOff() {
#ifdef WABCUT_AUDIO_MXCSR
    // Only the two mode bits go back: the rest of MXCSR (rounding, masks, sticky flags) stays as the scope left it.
    _mm_setcsr((_mm_getcsr() & ~kDenormalBits) | m_saved);
#else
    (void)m_saved;
#endif
}

std::uint64_t lockViolations() noexcept { return g_lockViolations.load(std::memory_order_relaxed); }

void recordLockViolation() noexcept { g_lockViolations.fetch_add(1, std::memory_order_relaxed); }

} // namespace wabcut::audio::rt
