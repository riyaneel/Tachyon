#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <utility>
#include <vector>

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <tachyon/arena.hpp>

#include "proc_scanner.hpp"

namespace tachyon::top {
	struct BusUIData {
		pid_t					pid;
		std::string				comm;
		size_t					capacity;
		size_t					used_bytes;
		size_t					head;
		size_t					tail;
		double					fill_pct;
		double					msg_per_sec;
		double					mb_per_sec;
		bool					consumer_sleeping;
		uint64_t				producer_hb_age_us;
		uint64_t				consumer_hb_age_us;
		tachyon::core::BusState state;
		std::array<double, 60>	sparkline;
	};

	class UIDataState {
		std::array<std::vector<BusUIData>, 2> buffers_;
		alignas(64) std::atomic<uint8_t> active_idx_{0};

	public:
		__always_inline void commit_render_state(std::vector<BusUIData> &&new_state) noexcept {
			const uint8_t inactive = 1 - active_idx_.load(std::memory_order_relaxed);
			buffers_[inactive]	   = std::move(new_state);
			active_idx_.store(inactive, std::memory_order_release);
		}

		[[nodiscard]] __always_inline const std::vector<BusUIData> &get_render_state() const noexcept {
			return buffers_[active_idx_.load(std::memory_order_acquire)];
		}
	};

	struct BusSnapshot {
		size_t head = 0;
		size_t tail = 0;
	};

	class BusView {
		BusHandle							  handle_;
		const tachyon::core::MemoryLayout	 *layout_{nullptr};
		size_t								  capacity_{0};
		BusSnapshot							  prev_{};
		std::chrono::steady_clock::time_point prev_ts_;
		std::array<double, 60>				  sparkline_{};
		size_t								  sparkline_idx_{0};
		double								  tsc_ticks_per_us_{1.0};

	public:
		explicit BusView(BusHandle &&handle, const double tsc_ticks_per_us = 1.0) noexcept
			: handle_(std::move(handle)), tsc_ticks_per_us_(tsc_ticks_per_us) {
			void *ptr = mmap(nullptr, handle_.shm_size, PROT_READ, MAP_SHARED, handle_.fd, 0);
			if (ptr != MAP_FAILED) [[likely]] {
				layout_	  = static_cast<const tachyon::core::MemoryLayout *>(ptr);
				capacity_ = layout_->header.capacity;
			}
			prev_ts_ = std::chrono::steady_clock::now();
		}

		~BusView() noexcept {
			if (layout_ != nullptr) {
				munmap(const_cast<tachyon::core::MemoryLayout *>(layout_), handle_.shm_size);
			}

			if (handle_.fd >= 0) {
				close(handle_.fd);
			}
		}

		BusView(const BusView &) = delete;

		BusView &operator=(const BusView &) = delete;

		BusView(BusView &&other) noexcept
			: handle_(std::move(other.handle_)), layout_(std::exchange(other.layout_, nullptr)),
			  capacity_(other.capacity_), prev_(other.prev_), prev_ts_(other.prev_ts_), sparkline_(other.sparkline_),
			  sparkline_idx_(other.sparkline_idx_), tsc_ticks_per_us_(other.tsc_ticks_per_us_) {}

		BusView &operator=(BusView &&other) noexcept {
			using std::swap;
			swap(handle_, other.handle_);
			swap(layout_, other.layout_);
			swap(capacity_, other.capacity_);
			swap(prev_, other.prev_);
			swap(prev_ts_, other.prev_ts_);
			swap(sparkline_, other.sparkline_);
			swap(sparkline_idx_, other.sparkline_idx_);
			swap(tsc_ticks_per_us_, other.tsc_ticks_per_us_);
			return *this;
		}

		[[nodiscard]] BusUIData sample() noexcept {
			BusUIData data{
				.pid				= handle_.pid,
				.comm				= handle_.comm,
				.capacity			= capacity_,
				.used_bytes			= 0,
				.head				= 0,
				.tail				= 0,
				.fill_pct			= 0.0,
				.msg_per_sec		= 0.0,
				.mb_per_sec			= 0.0,
				.consumer_sleeping	= false,
				.producer_hb_age_us = 0,
				.consumer_hb_age_us = 0,
				.state				= tachyon::core::BusState::Unknown,
				.sparkline			= sparkline_
			};

			if (!layout_) [[unlikely]] {
				return data;
			}

			const size_t cur_head = layout_->indices.head.load(std::memory_order_relaxed);
			const size_t cur_tail = layout_->indices.tail.load(std::memory_order_relaxed);
			const auto	 c_sleep  = layout_->indices.consumer_sleeping.load(std::memory_order_relaxed);
			const auto	 p_hb	  = layout_->indices.producer_heartbeat.load(std::memory_order_relaxed);
			const auto	 c_hb	  = layout_->indices.consumer_heartbeat.load(std::memory_order_relaxed);
			const auto	 b_state  = layout_->header.state.load(std::memory_order_relaxed);

			const auto	 now	   = std::chrono::steady_clock::now();
			const double delta_t_s = std::chrono::duration<double>(now - prev_ts_).count();

			const size_t delta_head = cur_head - prev_.head;

			prev_ts_   = now;
			prev_.head = cur_head;
			prev_.tail = cur_tail;

			data.head = cur_head;
			data.tail = cur_tail;

			data.used_bytes = std::min(cur_head - cur_tail, capacity_);

			if (capacity_ > 0) [[likely]] {
				data.fill_pct = (static_cast<double>(data.used_bytes) / static_cast<double>(capacity_)) * 100.0;
			}

			if (delta_t_s > 0.0) [[likely]] {
				data.mb_per_sec	 = (static_cast<double>(delta_head) / delta_t_s) / 1e6;
				data.msg_per_sec = static_cast<double>(delta_head) / (2.0 * TACHYON_MSG_ALIGNMENT) / delta_t_s;
			}

			data.consumer_sleeping = (c_sleep == 1);
			data.state			   = b_state;

			const uint64_t now_tsc = tachyon::rdtsc();
			data.producer_hb_age_us =
				(now_tsc > p_hb) ? static_cast<uint64_t>(static_cast<double>(now_tsc - p_hb) / tsc_ticks_per_us_) : 0;
			data.consumer_hb_age_us =
				(now_tsc > c_hb) ? static_cast<uint64_t>(static_cast<double>(now_tsc - c_hb) / tsc_ticks_per_us_) : 0;

			sparkline_[sparkline_idx_ % 60] = data.mb_per_sec;
			data.sparkline					= sparkline_;
			sparkline_idx_++;

			return data;
		}

		[[nodiscard]] __always_inline bool is_alive() const noexcept {
			char path_buf[64];
			std::memcpy(path_buf, "/proc/", 6);
			auto [ptr, ec] = std::to_chars(path_buf + 6, path_buf + sizeof(path_buf) - 6, handle_.pid);
			*ptr		   = '\0';

			struct stat st;
			return stat(path_buf, &st) == 0;
		}

		[[nodiscard]] const BusHandle &handle() const noexcept {
			return handle_;
		}
	};
} // namespace tachyon::top
