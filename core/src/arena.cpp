#include <cstring>
#include <utility>

#if defined(__linux__)
#include <linux/futex.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <time.h>
#endif

#include <tachyon/arena.hpp>

namespace tachyon::core {
	namespace {
		struct alignas(16) MessageHeader {
			uint32_t				 size;
			uint32_t				 type_id;
			[[maybe_unused]] uint8_t padding_[8];
		};
		constexpr uint32_t SKIP_MARKER = 0xFFFFFFFF;

		inline auto platform_wait(std::atomic<uint32_t> *addr) noexcept -> void {
#if defined(__linux__)
			syscall(SYS_futex, addr, FUTEX_WAIT, 1, nullptr, nullptr, 0);
#elif defined(__APPLE__)
			struct timespec ts = {0, 50000};
			nanosleep(&ts, nullptr);
#else
			std::this_thread::yield();
#endif
		}
		inline void platform_wake(std::atomic<uint32_t> *addr) noexcept {
#if defined(__linux__)
			syscall(SYS_futex, addr, FUTEX_WAKE, 1, nullptr, nullptr, 0);
#endif
		}
	} // namespace

	[[nodiscard]] static constexpr bool is_power_of_two(const size_t v) noexcept {
		return v != 0 && (v & (v - 1)) == 0;
	}

	Arena::Arena(MemoryLayout *layout, const size_t capacity) noexcept
		: layout_(layout), capacity_mask_(capacity - 1),
		  local_head_(layout->indices.head.load(std::memory_order_relaxed)),
		  cached_tail_(layout->indices.tail.load(std::memory_order_relaxed)),
		  local_tail_(layout->indices.tail.load(std::memory_order_relaxed)),
		  cached_head_(layout->indices.head.load(std::memory_order_relaxed)) {}

	Arena::Arena(Arena &&other) noexcept
		: layout_(std::exchange(other.layout_, nullptr)), capacity_mask_(std::exchange(other.capacity_mask_, 0)),
		  local_head_(std::exchange(other.local_head_, 0)), cached_tail_(std::exchange(other.cached_tail_, 0)),
		  pending_tx_(std::exchange(other.pending_tx_, 0)), local_tail_(std::exchange(other.local_tail_, 0)),
		  cached_head_(std::exchange(other.cached_head_, 0)), pending_rx_(std::exchange(other.pending_rx_, 0)) {}

	Arena &Arena::operator=(Arena &&other) noexcept {
		if (this != &other) [[likely]] {
			layout_		   = std::exchange(other.layout_, nullptr);
			capacity_mask_ = std::exchange(other.capacity_mask_, 0);
			local_head_	   = std::exchange(other.local_head_, 0);
			cached_tail_   = std::exchange(other.cached_tail_, 0);
			pending_tx_	   = std::exchange(other.pending_tx_, 0);
			local_tail_	   = std::exchange(other.local_tail_, 0);
			cached_head_   = std::exchange(other.cached_head_, 0);
			pending_rx_	   = std::exchange(other.pending_rx_, 0);
		}
		return *this;
	}

	auto Arena::format(const std::span<std::byte> shm_span, const size_t capacity) -> std::expected<Arena, ShmError> {
		if (!is_power_of_two(capacity) || shm_span.size() < sizeof(MemoryLayout) + capacity) [[unlikely]]
			return std::unexpected(ShmError::InvalidSize);

		auto *layout			= tachyon_start_lifetime_as<MemoryLayout>(shm_span.data());
		layout->header.magic	= TACHYON_MAGIC;
		layout->header.version	= TACHYON_VERSION;
		layout->header.capacity = static_cast<uint32_t>(capacity);
		layout->indices.head.store(0, std::memory_order_relaxed);
		layout->indices.tail.store(0, std::memory_order_relaxed);
		layout->indices.consumer_sleeping.store(0, std::memory_order_relaxed);
		layout->header.state.store(BusState::Ready, std::memory_order_release);

		return Arena(layout, capacity);
	}

	auto Arena::attach(const std::span<std::byte> shm_span) -> std::expected<Arena, ShmError> {
		if (shm_span.size() < sizeof(MemoryLayout)) [[unlikely]]
			return std::unexpected(ShmError::InvalidSize);

		auto *layout = tachyon_start_lifetime_as<MemoryLayout>(shm_span.data());
		if (layout->header.magic != TACHYON_MAGIC) [[unlikely]]
			return std::unexpected(ShmError::MapFailed);

		const size_t capacity = layout->header.capacity;
		if (!is_power_of_two(capacity) || shm_span.size() < sizeof(MemoryLayout) + capacity) [[unlikely]]
			return std::unexpected(ShmError::InvalidSize);

		return Arena(layout, capacity);
	}

	bool Arena::try_push(const uint32_t type_id, const std::span<const std::byte> data) noexcept {
		const size_t msg_size		  = data.size();
		const size_t total_msg_size	  = sizeof(MessageHeader) + msg_size;
		const size_t aligned_msg_size = (total_msg_size + 15) & ~15ULL;
		const size_t capacity		  = capacity_mask_ + 1;
		if (aligned_msg_size > capacity) [[unlikely]]
			return false;

		size_t		 physical_idx	 = local_head_ & capacity_mask_;
		const size_t space_until_end = capacity - physical_idx;
		const bool	 need_skip		 = space_until_end < aligned_msg_size;
		size_t		 required_space	 = aligned_msg_size;
		if (need_skip) {
			required_space += space_until_end;
		}

		if (local_head_ - cached_tail_ + required_space > capacity) {
			cached_tail_ = layout_->indices.tail.load(std::memory_order_acquire);
			if (local_head_ - cached_tail_ + required_space > capacity) [[unlikely]]
				return false;
		}

		if (need_skip) {
			constexpr MessageHeader skip_hdr{SKIP_MARKER, 0, {}};
			std::memcpy(&layout_->data_arena()[physical_idx], &skip_hdr, sizeof(MessageHeader));
			local_head_ += space_until_end;
			physical_idx = 0;
		}

		const MessageHeader hdr{static_cast<uint32_t>(msg_size), type_id, {}};
		std::memcpy(&layout_->data_arena()[physical_idx], &hdr, sizeof(MessageHeader));
		std::memcpy(&layout_->data_arena()[physical_idx + sizeof(MessageHeader)], data.data(), msg_size);

		local_head_ += aligned_msg_size;
		pending_tx_++;

		if ((pending_tx_ & (BATCH_SIZE - 1)) == 0) [[unlikely]] {
			layout_->indices.head.store(local_head_, std::memory_order_release);
			std::atomic_thread_fence(std::memory_order_seq_cst);
			if (layout_->indices.consumer_sleeping.load(std::memory_order_acquire) == 1) [[unlikely]] {
				platform_wake(&layout_->indices.consumer_sleeping);
			}
		}

		return true;
	}

	bool Arena::try_pop(uint32_t &out_type_id, std::span<std::byte> out_buffer, size_t &out_size) noexcept {
		if (cached_head_ <= local_tail_) {
			cached_head_ = layout_->indices.head.load(std::memory_order_acquire);
			if (cached_head_ <= local_tail_) [[likely]]
				return false;
		}

		size_t		  physical_idx = local_tail_ & capacity_mask_;
		MessageHeader hdr;
		std::memcpy(&hdr, &layout_->data_arena()[physical_idx], sizeof(MessageHeader));

		if (hdr.size == SKIP_MARKER) [[unlikely]] {
			const size_t space_until_end = (capacity_mask_ + 1) - physical_idx;
			local_tail_ += space_until_end;
			physical_idx = 0;
			std::memcpy(&hdr, &layout_->data_arena()[0], sizeof(MessageHeader));
		}

		if (out_buffer.size() < hdr.size) [[unlikely]]
			return false;

		std::memcpy(out_buffer.data(), &layout_->data_arena()[physical_idx + sizeof(MessageHeader)], hdr.size);

		const size_t total_msg_size	  = sizeof(MessageHeader) + hdr.size;
		const size_t aligned_msg_size = (total_msg_size + 15) & ~15ULL;

		out_size	= hdr.size;
		out_type_id = hdr.type_id;
		local_tail_ += aligned_msg_size;
		pending_rx_++;

		if ((pending_rx_ & (BATCH_SIZE - 1)) == 0) [[unlikely]] {
			layout_->indices.tail.store(local_tail_, std::memory_order_release);
		}

		return true;
	}

	bool Arena::pop_spin(
		uint32_t &out_type_id, const std::span<std::byte> out_buffer, size_t &out_size, const uint32_t max_spins
	) noexcept {
		uint32_t spins = 0;
		while (!try_pop(out_type_id, out_buffer, out_size)) {
			if (max_spins != 0 && spins >= max_spins)
				return false;
			cpu_relax();
			spins++;
		}
		return true;
	}

	bool Arena::pop_blocking(
		uint32_t &out_type_id, const std::span<std::byte> out_buffer, size_t &out_size, const uint32_t spin_threshold
	) noexcept {
		uint32_t spins = 0;
		while (!try_pop(out_type_id, out_buffer, out_size)) {
			if (spins < spin_threshold) {
				cpu_relax();
				spins++;
			} else {
				layout_->indices.consumer_sleeping.store(1, std::memory_order_release);
				std::atomic_thread_fence(std::memory_order_seq_cst);
				if (try_pop(out_type_id, out_buffer, out_size)) {
					layout_->indices.consumer_sleeping.store(0, std::memory_order_relaxed);
					return true;
				}
				platform_wait(&layout_->indices.consumer_sleeping);
				layout_->indices.consumer_sleeping.store(0, std::memory_order_relaxed);
				spins = 0;
			}
		}
		return true;
	}

	void Arena::flush() noexcept {
		bool published_tx = false;
		if ((pending_tx_ & (BATCH_SIZE - 1)) != 0) {
			layout_->indices.head.store(local_head_, std::memory_order_release);
			pending_tx_	 = 0;
			published_tx = true;
		}

		if (published_tx) {
			std::atomic_thread_fence(std::memory_order_seq_cst);
			if (layout_->indices.consumer_sleeping.load(std::memory_order_acquire) == 1) [[unlikely]] {
				platform_wake(&layout_->indices.consumer_sleeping);
			}
		}

		if ((pending_rx_ & (BATCH_SIZE - 1)) != 0) {
			layout_->indices.tail.store(local_tail_, std::memory_order_release);
			pending_rx_ = 0;
		}
	}
} // namespace tachyon::core
