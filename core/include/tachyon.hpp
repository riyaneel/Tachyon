#pragma once

#include <cstring>

#if defined(_WIN32) || defined(__CYGWIN__)
#define TACHYON_API __declspec(dllexport)
#else
#define TACHYON_API __attribute__((visibility("default")))
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

	template <typename T> [[nodiscard]] inline T *tachyon_start_lifetime_as(void *p) noexcept {
#if defined(__cpp_lib_start_lifetime_as) && __cpp_lib_start_lifetime_as >= 202207L
		return std::start_lifetime_as<T>(p);
#else  // #if defined(__cpp_lib_start_lifetime_as) && __cpp_lib_start_lifetime_as >= 202207L
		return std::launder(static_cast<T *>(std::memmove(p, p, sizeof(T))));
#endif // #if defined(__cpp_lib_start_lifetime_as) && __cpp_lib_start_lifetime_as >= 202207L #else
	}
} // namespace tachyon
