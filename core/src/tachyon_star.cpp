#include <new>
#include <utility>

#include <tachyon.h>
#include <tachyon/arena.hpp>
#include <tachyon/star.hpp>

using namespace tachyon::core;

struct tachyon_star {
	StarBus bus;

	explicit tachyon_star(StarBus &&sb) noexcept : bus(std::move(sb)) {}
};

extern "C" {
tachyon_error_t
tachyon_star_create(tachyon_bus_t **buses, const size_t n, const int *node_ids, tachyon_star_t **out) TACHYON_NOEXCEPT {
	if (!out) [[unlikely]] {
		return TACHYON_ERR_NULL_PTR;
	}

	auto res = StarBus::create(buses, n, node_ids);
	if (!res.has_value()) [[unlikely]] {
		return res.error();
	}

	auto *star = new (std::nothrow) tachyon_star(std::move(*res));
	if (!star) [[unlikely]] {
		return TACHYON_ERR_MEM;
	}

	*out = star;
	return TACHYON_SUCCESS;
}

void tachyon_star_destroy(tachyon_star_t *star) TACHYON_NOEXCEPT {
	if (star) {
		delete star;
	}
}

size_t tachyon_star_poll(
	tachyon_star_t	   *star,
	tachyon_msg_view_t *out_views,
	const size_t		max_total,
	const uint64_t		budget_us,
	size_t			   *out_spoke_indices
) TACHYON_NOEXCEPT {
	if (!star || !out_views || max_total == 0) [[unlikely]] {
		return 0;
	}

	return star->bus.poll(out_views, max_total, budget_us, out_spoke_indices);
}

tachyon_error_t tachyon_star_commit(tachyon_star_t *star) TACHYON_NOEXCEPT {
	if (!star) [[unlikely]] {
		return TACHYON_ERR_NULL_PTR;
	}

	return star->bus.commit() ? TACHYON_SUCCESS : TACHYON_ERR_SYSTEM;
}

void *tachyon_star_acquire_tx(tachyon_star_t *star, const size_t spoke_idx, const size_t max_size) TACHYON_NOEXCEPT {
	if (!star) [[unlikely]] {
		return nullptr;
	}

	return star->bus.acquire_tx(spoke_idx, max_size);
}

tachyon_error_t tachyon_star_commit_tx(
	tachyon_star_t *star, const size_t spoke_idx, const size_t actual_size, const uint32_t type_id
) TACHYON_NOEXCEPT {
	if (!star) [[unlikely]] {
		return TACHYON_ERR_NULL_PTR;
	}

	if (spoke_idx >= star->bus.n_spokes()) [[unlikely]] {
		return TACHYON_ERR_INVALID_SZ;
	}

	return star->bus.commit_tx(spoke_idx, actual_size, type_id) ? TACHYON_SUCCESS : TACHYON_ERR_SYSTEM;
}

tachyon_error_t tachyon_star_rollback_tx(tachyon_star_t *star, const size_t spoke_idx) TACHYON_NOEXCEPT {
	if (!star) [[unlikely]] {
		return TACHYON_ERR_NULL_PTR;
	}

	if (spoke_idx >= star->bus.n_spokes()) [[unlikely]] {
		return TACHYON_ERR_INVALID_SZ;
	}

	return star->bus.rollback_tx(spoke_idx) ? TACHYON_SUCCESS : TACHYON_ERR_SYSTEM;
}

void tachyon_star_flush(tachyon_star_t *star, const size_t spoke_idx) TACHYON_NOEXCEPT {
	if (!star) [[unlikely]] {
		return;
	}

	star->bus.flush(spoke_idx);
}

tachyon_state_t tachyon_star_get_state(const tachyon_star_t *star, const size_t spoke_idx) TACHYON_NOEXCEPT {
	if (!star) [[unlikely]] {
		return TACHYON_STATE_UNKNOWN;
	}

	return star->bus.get_state(spoke_idx);
}

size_t tachyon_star_n_spokes(const tachyon_star_t *star) TACHYON_NOEXCEPT {
	if (!star) [[unlikely]] {
		return 0;
	}

	return star->bus.n_spokes();
}
}
