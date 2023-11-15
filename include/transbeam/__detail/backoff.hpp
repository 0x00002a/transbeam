
#pragma once

#include <cstdint>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#define TRANSBEAM_X86_64 1
#elif defined(i386) || defined(__i386__) || defined(__i386) || defined(_M_IX86)
#define TRANSBEAM_X86 1
#endif

#if TRANSBEAM_X86_64
#include <emmintrin.h>
#elif TRANSBEAM_X86
#include <x86intrin.h>
#endif

namespace transbeam::__detail {

inline void cpu_spinhint_pause()
{
#if TRANSBEAM_X86_64 || TRANSBEAM_X86
    _mm_pause();
#endif
}
class backoff {
    constexpr static auto spin_limit = 6;
    constexpr static auto yield_limit = 10;

public:
    backoff() = default;

    void cpu_yield()
    {
        spin_to_lim();
        if (spins_ <= spin_limit) {
            spins_++;
        }
    }
    void thread_yield()
    {
        if (spins_ <= spin_limit) {
            spin_to_lim();
        }
        else {
            std::this_thread::yield();
        }
        if (spins_ <= yield_limit) {
            spins_++;
        }
    }

private:
    constexpr void spin_to_lim()
    {
        const int lim = 1 << spins_;
        for (int n = 0; n != lim; ++n) {
            cpu_spinhint_pause();
        }
    }

    uint8_t spins_{0};
};

} // namespace transbeam::__detail
