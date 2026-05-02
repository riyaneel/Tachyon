#pragma once

#include <cstring>
#include <new>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif // #if defined(__x86_64__) || defined(_M_X64)

#if defined(_WIN32) || defined(__CYGWIN__)
#define TACHYON_API __declspec(dllexport)
#define TACHYON_INLINE __forceinline
#else
#define TACHYON_API __attribute__((visibility("default")))
#define TACHYON_INLINE __attribute__((always_inline)) inline
#endif

namespace tachyon {
	[[gnu::always_inline]] inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
		asm volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(_M_ARM64) // #if defined(__x86_64__) || defined(_M_X64)
		asm volatile("yield" ::: "memory");
#else // #elif defined(__aarch64__) || defined(_M_ARM64)
		asm volatile("" ::: "memory");
#endif // #elif defined(__aarch64__) || defined(_M_ARM64) #else
	}

	[[gnu::always_inline]] inline uint64_t rdtsc() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
		return __rdtsc();
#elif defined(__aarch64__) || defined(_M_ARM64) // #if defined(__x86_64__) || defined(_M_X64)
		uint64_t val;
		asm volatile("mrs %0, cntvct_el0" : "=r"(val));
		return val;
#else // #elif defined(__aarch64__) || defined(_M_ARM64)
		return 0;
#endif // #elif defined(__aarch64__) || defined(_M_ARM64) #else
	}

	template <typename T> [[nodiscard]] inline T *tachyon_start_lifetime_as(void *p) noexcept {
#if defined(__cpp_lib_start_lifetime_as) && __cpp_lib_start_lifetime_as >= 202207L
		return std::start_lifetime_as<T>(p);
#else  // #if defined(__cpp_lib_start_lifetime_as) && __cpp_lib_start_lifetime_as >= 202207L
		return std::launder(static_cast<T *>(std::memmove(p, p, sizeof(T))));
#endif // #if defined(__cpp_lib_start_lifetime_as) && __cpp_lib_start_lifetime_as >= 202207L #else
	}
} // namespace tachyon
