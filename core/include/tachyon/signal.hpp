#pragma once

#include <atomic>

#include <tachyon.hpp>

namespace tachyon::core {
	class TACHYON_API SignalHandler {
		static std::atomic<bool> terminate_flag_;

	public:
		SignalHandler() = delete;

		static void setup() noexcept;

		[[nodiscard]] static inline bool is_running() noexcept {
			return !terminate_flag_.load(std::memory_order_relaxed);
		}

		static inline void trigger() noexcept {
			terminate_flag_.store(true, std::memory_order_relaxed);
		}
	};
} // namespace tachyon::core
