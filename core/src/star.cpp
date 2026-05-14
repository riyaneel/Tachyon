#include <time.h>
#include <utility>

#include <tachyon/arena.hpp>
#include <tachyon/star.hpp>

namespace tachyon::core {

	namespace {
		double calibrate_tsc_per_us() noexcept {
			struct timespec ts_start, ts_end;
			::clock_gettime(CLOCK_MONOTONIC, &ts_start);
			const uint64_t tsc_start = rdtsc();

			constexpr struct timespec req = {0, 10'000'000};
			::nanosleep(&req, nullptr);

			const uint64_t tsc_end = tachyon::rdtsc();
			::clock_gettime(CLOCK_MONOTONIC, &ts_end);
			const uint64_t elapsed_ns = static_cast<uint64_t>(ts_end.tv_sec - ts_start.tv_sec) * 1'000'000'000ULL +
										static_cast<uint64_t>(ts_end.tv_nsec) - static_cast<uint64_t>(ts_start.tv_nsec);

			if (elapsed_ns == 0) [[unlikely]] {
				return 1000.0;
			}

			return static_cast<double>(tsc_end - tsc_start) / (static_cast<double>(elapsed_ns) / 1000.0);
		}
	} // namespace

	StarBus::~StarBus() noexcept {
		delete[] pending_;

		if (buses_) {
			for (size_t i = 0; i < n_; ++i) {
				tachyon_bus_destroy(buses_[i]);
			}

			delete[] buses_;
		}
	}

	StarBus::StarBus(StarBus &&other) noexcept
		: buses_(std::exchange(other.buses_, nullptr)), n_(std::exchange(other.n_, 0)),
		  tsc_per_us_(std::exchange(other.tsc_per_us_, 0.0)), pending_(std::exchange(other.pending_, nullptr)) {}

	StarBus &StarBus::operator=(StarBus &&other) noexcept {
		if (this != &other) [[likely]] {
			delete[] pending_;
			if (buses_) {
				for (size_t i = 0; i < n_; ++i) {
					tachyon_bus_destroy(buses_[i]);
				}

				delete[] buses_;
			}

			buses_		= std::exchange(other.buses_, nullptr);
			n_			= std::exchange(other.n_, 0);
			tsc_per_us_ = std::exchange(other.tsc_per_us_, 0.0);
			pending_	= std::exchange(other.pending_, nullptr);
		}

		return *this;
	}

	std::expected<StarBus, tachyon_error_t>
	StarBus::create(tachyon_bus_t **buses, const size_t n, const int *node_ids) noexcept {
		if (!buses || n == 0) [[unlikely]] {
			return std::unexpected(TACHYON_ERR_NULL_PTR);
		}

		auto **owned_buses = new (std::nothrow) tachyon_bus_t *[n];
		if (!owned_buses) [[unlikely]] {
			return std::unexpected(TACHYON_ERR_MEM);
		}

		auto *pending = new (std::nothrow) PendingSpoke[n]();
		if (!pending) [[unlikely]] {
			delete[] owned_buses;
			return std::unexpected(TACHYON_ERR_MEM);
		}

		for (size_t i = 0; i < n; ++i) {
			tachyon_bus_ref(buses[i]);
			owned_buses[i] = buses[i];
		}

		StarBus star_bus{};
		star_bus.buses_		 = owned_buses;
		star_bus.n_			 = n;
		star_bus.tsc_per_us_ = calibrate_tsc_per_us();
		star_bus.pending_	 = pending;

		if (node_ids) {
			for (size_t i = 0; i < n; ++i) {
				if (node_ids[i] >= 0) {
					tachyon_bus_set_numa_node(owned_buses[i], node_ids[i]);
				}
			}
		}

		return star_bus;
	}

	std::size_t StarBus::poll(
		tachyon_msg_view_t *views, const std::size_t max_total, const uint64_t budget_us, std::size_t *out_spoke_indices
	) {
		if (!views || max_total == 0) [[unlikely]] {
			return 0;
		}

		auto		  *cxx_views = reinterpret_cast<RxView *>(views);
		const uint64_t deadline	 = rdtsc() + static_cast<uint64_t>(static_cast<double>(budget_us) * tsc_per_us_);

		std::size_t total = 0;
		while (total < max_total && rdtsc() < deadline) {
			bool any_data = false;

			for (std::size_t i = 0; i < n_; ++i) {
				if (const auto k = buses_[i]->arena.acquire_rx_batch(cxx_views + total, max_total - total); k > 0) {
					if (out_spoke_indices) [[likely]] {
						for (std::size_t j = 0; j < k; ++j) {
							out_spoke_indices[total + j] = i;
						}
					}

					auto &[count, total_reserved_bytes] = pending_[i];
					for (std::size_t j = 0; j < k; ++j) {
						total_reserved_bytes += views[total + j].reserved_;
					}

					count += k;
					total += k;
					any_data = true;

					if (total >= max_total) {
						return total;
					}
				}
			}

			if (!any_data) {
				cpu_relax();
			}
		}

		return total;
	}

	bool StarBus::commit() noexcept {
		for (std::size_t i = 0; i < n_; ++i) {
			auto &[count, total_reserved_bytes] = pending_[i];
			if (count == 0) {
				continue;
			}

			if (!buses_[i]->arena.advance_tail_batch(total_reserved_bytes, count)) [[unlikely]] {
				return false;
			}

			count				 = 0;
			total_reserved_bytes = 0;
		}

		return true;
	}

	bool StarBus::commit_tx(
		const std::size_t spoke_idx, const std::size_t actual_size, const uint32_t type_id
	) const noexcept {
		if (spoke_idx >= n_) [[unlikely]] {
			return false;
		}

		if (!buses_[spoke_idx]->arena.commit_tx(actual_size, type_id)) [[unlikely]] {
			return false;
		}

		buses_[spoke_idx]->arena.flush_tx();
		return true;
	}

	void *StarBus::acquire_tx(const std::size_t spoke_idx, const std::size_t max_size) const noexcept {
		if (spoke_idx >= n_) [[unlikely]] {
			return nullptr;
		}

		return buses_[spoke_idx]->arena.acquire_tx(max_size);
	}

	bool StarBus::rollback_tx(const std::size_t spoke_idx) const noexcept {
		if (spoke_idx >= n_) [[unlikely]] {
			return false;
		}

		return buses_[spoke_idx]->arena.rollback_tx();
	}

	void StarBus::flush(const std::size_t spoke_idx) const noexcept {
		if (spoke_idx >= n_) [[unlikely]] {
			return;
		}

		buses_[spoke_idx]->arena.flush_tx();
	}
} // namespace tachyon::core
