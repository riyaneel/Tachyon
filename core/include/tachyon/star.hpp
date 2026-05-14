#pragma once

#include <cstddef>
#include <expected>

#include <tachyon.h>
#include <tachyon.hpp>

#include "bus_impl.h"

namespace tachyon::core {
	struct alignas(16) PendingSpoke {
		std::size_t count;
		std::size_t total_reserved_bytes;
	};

	class alignas(TACHYON_MSG_ALIGNMENT) StarBus {
		tachyon_bus_t **buses_{nullptr};
		std::size_t		n_{0};
		double			tsc_per_us_{0.0};
		PendingSpoke   *pending_;

		StarBus() noexcept = default;

	public:
		~StarBus() noexcept;

		StarBus(StarBus &&other) noexcept;

		StarBus &operator=(StarBus &&other) noexcept;

		StarBus(const StarBus &&) = delete;

		StarBus &operator=(const StarBus &&) = delete;

		[[nodiscard]] static std::expected<StarBus, tachyon_error_t>
		create(tachyon_bus_t **buses, std::size_t n, const int *node_ids) noexcept;

		[[nodiscard]] std::size_t
		poll(tachyon_msg_view_t *views, std::size_t max_total, uint64_t budget_us, std::size_t *out_spoke_indices);

		[[nodiscard]] bool commit() noexcept;

		[[nodiscard]] bool commit_tx(std::size_t spoke_idx, std::size_t actual_size, uint32_t type_id) noexcept;

		[[nodiscard]] void *acquire_tx(std::size_t spoke_idx, std::size_t max_size) noexcept;

		[[nodiscard]] bool rollback_tx(std::size_t spoke_idx) noexcept;

		void flush(std::size_t spoke_idx) noexcept;

		[[nodiscard]] TACHYON_INLINE tachyon_state_t get_state(const std::size_t spoke_idx) const noexcept {
			if (spoke_idx >= n_) [[unlikely]] {
				return TACHYON_STATE_UNKNOWN;
			}

			return static_cast<tachyon_state_t>(buses_[spoke_idx]->arena.get_state());
		}

		[[nodiscard]] TACHYON_INLINE std::size_t n_spokes() const noexcept {
			return n_;
		}
	};
} // namespace tachyon::core
