#include <wabcut/audio/RealTime.h>

#if defined(_M_X64) || defined(__x86_64__)
#include <pmmintrin.h> // _MM_DENORMALS_ZERO_ON
#include <xmmintrin.h> // _mm_getcsr, _mm_setcsr, _MM_FLUSH_ZERO_ON
#define WABCUT_AUDIO_MXCSR 1
#elif defined(__aarch64__) || defined(_M_ARM64)
// AArch64 (Apple silicon, Windows on ARM64): FPCR bit 24 (FZ) flushes denormal results and inputs to zero, the one
// control that does what MXCSR's FTZ and DAZ do together.
// https://developer.arm.com/documentation/ddi0601/latest/AArch64-Registers/FPCR--Floating-point-Control-Register
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h> // _ReadStatusReg, _WriteStatusReg
#endif
#define WABCUT_AUDIO_FPCR 1
#endif

namespace wabcut::audio::rt {

namespace {
thread_local int t_realtimeDepth = 0;
std::atomic<std::uint64_t> g_lockViolations{0};

#ifdef WABCUT_AUDIO_MXCSR
constexpr unsigned int kDenormalBits = _MM_FLUSH_ZERO_ON | _MM_DENORMALS_ZERO_ON;
#endif

#ifdef WABCUT_AUDIO_FPCR
constexpr unsigned int kDenormalBits = 1u << 24; // FPCR.FZ

std::uint64_t readFpcr() noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
    // FPCR is S3_3_C4_C4_0; MSVC names a system register by ARM64_SYSREG(op0, op1, CRn, CRm, op2) = 0x5A20.
    return static_cast<std::uint64_t>(_ReadStatusReg(0x5A20));
#else
    std::uint64_t value = 0;
    __asm__ volatile("mrs %0, fpcr" : "=r"(value));
    return value;
#endif
}

void writeFpcr(std::uint64_t value) noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
    _WriteStatusReg(0x5A20, static_cast<__int64>(value));
#else
    __asm__ volatile("msr fpcr, %0" : : "r"(value));
#endif
}
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
#elif defined(WABCUT_AUDIO_FPCR)
    const std::uint64_t fpcr = readFpcr();
    m_saved = static_cast<unsigned int>(fpcr & kDenormalBits);
    writeFpcr(fpcr | kDenormalBits);
#endif
}

ScopedDenormalsOff::~ScopedDenormalsOff() {
#ifdef WABCUT_AUDIO_MXCSR
    // Only the two mode bits go back: the rest of MXCSR (rounding, masks, sticky flags) stays as the scope left it.
    _mm_setcsr((_mm_getcsr() & ~kDenormalBits) | m_saved);
#elif defined(WABCUT_AUDIO_FPCR)
    // Only FZ goes back: the rounding mode and the trap bits stay as the scope left them.
    writeFpcr((readFpcr() & ~std::uint64_t{kDenormalBits}) | m_saved);
#else
    (void)m_saved;
#endif
}

std::uint64_t lockViolations() noexcept { return g_lockViolations.load(std::memory_order_relaxed); }

void recordLockViolation() noexcept { g_lockViolations.fetch_add(1, std::memory_order_relaxed); }

} // namespace wabcut::audio::rt
