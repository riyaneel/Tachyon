#include <cerrno>
#include <cstring>
#include <utility>

#if defined(__linux__)
#include <linux/futex.h>
#include <unistd.h>
#endif // #if defined(__linux__)

#include <tachyon/arena.hpp>

namespace tachyon::core {
	namespace {
		constexpr uint32_t SKIP_MARKER		   = 0xFFFFFFFF;
		constexpr uint32_t WATCHDOG_TIMEOUT_US = 1'000'000;

		enum class WaitResult : uint8_t { Woken, Timeout, Interrupted };

#if defined(__APPLE__)
#define UL_COMPARE_AND_WAIT 1
#define ULF_WAKE_ALL 0x00000100

		extern "C" int __ulock_wait(uint32_t operation, void *addr, uint64_t value, uint32_t timeout);
		extern "C" int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);
#endif // #if defined(__APPLE__)

		inline WaitResult platform_wait(std::atomic<uint32_t> *addr) noexcept {
#if defined(__linux__)
			struct timespec ts = {
				.tv_sec	 = static_cast<time_t>(WATCHDOG_TIMEOUT_US / 1'000'000),
				.tv_nsec = static_cast<long>((WATCHDOG_TIMEOUT_US % 1'000'000) * 1000)
			};
			if (syscall(SYS_futex, addr, FUTEX_WAIT, 1, &ts, nullptr, 0) == -1) {
				if (errno == EINTR)
					return WaitResult::Interrupted;
				if (errno == ETIMEDOUT)
					return WaitResult::Timeout;
			}
			return WaitResult::Woken;
#elif defined(__APPLE__)
			if (__ulock_wait(UL_COMPARE_AND_WAIT, addr, 1, WATCHDOG_TIMEOUT_US) == -1) {
				if (errno == EINTR)
					return WaitResult::Interrupted;
				if (errno == ETIMEDOUT)
					return WaitResult::Timeout;
			}
			return WaitResult::Woken;
#else
			std::this_thread::yield();
			return WaitResult::Woken;
#endif
		}

		inline void platform_wake(std::atomic<uint32_t> *addr) noexcept {
#if defined(__linux__)
			syscall(SYS_futex, addr, FUTEX_WAKE, 1, nullptr, nullptr, 0);
#elif defined(__APPLE__)
			__ulock_wake(UL_COMPARE_AND_WAIT | ULF_WAKE_ALL, addr, 0);
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
		  cached_head_(std::exchange(other.cached_head_, 0)), pending_rx_(std::exchange(other.pending_rx_, 0)),
		  tx_reserved_size_(std::exchange(other.tx_reserved_size_, 0)),
		  rx_reserved_size_(std::exchange(other.rx_reserved_size_, 0)) {}

	Arena &Arena::operator=(Arena &&other) noexcept {
		if (this != &other) [[likely]] {
			layout_			  = std::exchange(other.layout_, nullptr);
			capacity_mask_	  = std::exchange(other.capacity_mask_, 0);
			local_head_		  = std::exchange(other.local_head_, 0);
			cached_tail_	  = std::exchange(other.cached_tail_, 0);
			pending_tx_		  = std::exchange(other.pending_tx_, 0);
			local_tail_		  = std::exchange(other.local_tail_, 0);
			cached_head_	  = std::exchange(other.cached_head_, 0);
			pending_rx_		  = std::exchange(other.pending_rx_, 0);
			tx_reserved_size_ = std::exchange(other.tx_reserved_size_, 0);
			rx_reserved_size_ = std::exchange(other.rx_reserved_size_, 0);
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
		layout->indices.producer_heartbeat.store(0, std::memory_order_relaxed);
		layout->indices.consumer_heartbeat.store(0, std::memory_order_relaxed);
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

	std::byte *Arena::acquire_tx(const size_t max_size) noexcept {
		const size_t total_msg_size = sizeof(MessageHeader) + max_size;
		const size_t aligned_msg_size =
			(total_msg_size + (TACHYON_MSG_ALIGNMENT - 1)) & ~(TACHYON_MSG_ALIGNMENT - 1ULL);
		const size_t capacity = capacity_mask_ + 1;
		if (aligned_msg_size > capacity || max_size > SKIP_MARKER - sizeof(MessageHeader)) [[unlikely]]
			return nullptr;

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
				return nullptr;
		}

		if (need_skip) {
			constexpr MessageHeader skip_hdr{SKIP_MARKER, 0, 0, {}};
			std::memcpy(&layout_->data_arena()[physical_idx], &skip_hdr, sizeof(MessageHeader));
			local_head_ += space_until_end;
			physical_idx = 0;
		}

		tx_reserved_size_ = aligned_msg_size;

		return layout_->data_arena() + physical_idx + sizeof(MessageHeader);
	}

	bool Arena::commit_tx(const size_t actual_size, const uint32_t type_id) noexcept {
		if (tx_reserved_size_ == 0 || actual_size > tx_reserved_size_ - sizeof(MessageHeader)) [[unlikely]] {
			tx_reserved_size_ = 0;
			return false;
		}

		const size_t		physical_idx = local_head_ & capacity_mask_;
		const MessageHeader hdr{
			static_cast<uint32_t>(actual_size), type_id, static_cast<uint32_t>(tx_reserved_size_), {}
		};
		std::memcpy(&layout_->data_arena()[physical_idx], &hdr, sizeof(MessageHeader));

		local_head_ += tx_reserved_size_;
		tx_reserved_size_ = 0;
		pending_tx_++;
		layout_->indices.producer_heartbeat.store(tachyon::rdtsc(), std::memory_order_relaxed);

		if ((pending_tx_ & (BATCH_SIZE - 1)) == 0) [[unlikely]] {
			layout_->indices.head.store(local_head_, std::memory_order_release);
			std::atomic_thread_fence(std::memory_order_seq_cst);
			if (layout_->indices.consumer_sleeping.load(std::memory_order_acquire) == 1) [[unlikely]] {
				platform_wake(&layout_->indices.consumer_sleeping);
			}
		}

		return true;
	}

	const std::byte *Arena::acquire_rx(uint32_t &out_type_id, size_t &out_actual_size) noexcept {
		if (cached_head_ <= local_tail_) {
			cached_head_ = layout_->indices.head.load(std::memory_order_acquire);
			if (cached_head_ <= local_tail_) [[likely]]
				return nullptr;
		}

		const size_t  capacity	   = capacity_mask_ + 1;
		size_t		  physical_idx = local_tail_ & capacity_mask_;
		MessageHeader hdr{};
		std::memcpy(&hdr, &layout_->data_arena()[physical_idx], sizeof(MessageHeader));

		if (hdr.size == SKIP_MARKER) [[unlikely]] {
			const size_t space_until_end = capacity - physical_idx;
			local_tail_ += space_until_end;
			physical_idx = 0;
			std::memcpy(&hdr, &layout_->data_arena()[0], sizeof(MessageHeader));
		}

		if (hdr.reserved_size > capacity || hdr.size > hdr.reserved_size - sizeof(MessageHeader)) [[unlikely]] {
			layout_->header.state.store(BusState::FatalError, std::memory_order_release);
			return nullptr;
		}

		out_type_id		  = hdr.type_id;
		out_actual_size	  = hdr.size;
		rx_reserved_size_ = hdr.reserved_size;

		return layout_->data_arena() + physical_idx + sizeof(MessageHeader);
	}

	bool Arena::commit_rx() noexcept {
		local_tail_ += rx_reserved_size_;
		rx_reserved_size_ = 0;
		pending_rx_++;
		layout_->indices.consumer_heartbeat.store(tachyon::rdtsc(), std::memory_order_relaxed);

		if ((pending_rx_ & (BATCH_SIZE - 1)) == 0) [[unlikely]] {
			layout_->indices.tail.store(local_tail_, std::memory_order_release);
		}

		return true;
	}

	const std::byte *
	Arena::acquire_rx_spin(uint32_t &out_type_id, size_t &out_actual_size, const uint32_t max_spins) noexcept {
		uint32_t		 spins = 0;
		const std::byte *ptr   = nullptr;

		while ((ptr = acquire_rx(out_type_id, out_actual_size)) == nullptr) {
			if (get_state() == BusState::FatalError) [[unlikely]]
				return nullptr;

			if (max_spins != 0 && spins >= max_spins)
				return nullptr;
			cpu_relax();
			spins++;
		}
		return ptr;
	}

	const std::byte *
	Arena::acquire_rx_blocking(uint32_t &out_type_id, size_t &out_actual_size, const uint32_t spin_threshold) noexcept {
		uint32_t		 spins = 0;
		const std::byte *ptr   = nullptr;

		while ((ptr = acquire_rx(out_type_id, out_actual_size)) == nullptr) {
			if (get_state() == BusState::FatalError) [[unlikely]]
				return nullptr;

			if (spins < spin_threshold) {
				cpu_relax();
				spins++;
			} else {
				layout_->indices.consumer_sleeping.store(1, std::memory_order_release);
				std::atomic_thread_fence(std::memory_order_seq_cst);

				if ((ptr = acquire_rx(out_type_id, out_actual_size)) != nullptr) {
					layout_->indices.consumer_sleeping.store(0, std::memory_order_relaxed);
					return ptr;
				}

				const uint64_t	 hb_before	 = layout_->indices.producer_heartbeat.load(std::memory_order_relaxed);
				const WaitResult wait_result = platform_wait(&layout_->indices.consumer_sleeping);
				layout_->indices.consumer_sleeping.store(0, std::memory_order_relaxed);
				if (wait_result == WaitResult::Interrupted) {
					return nullptr;
				}

				if (wait_result == WaitResult::Timeout) {
					const uint64_t hb_after = layout_->indices.producer_heartbeat.load(std::memory_order_relaxed);
					if (hb_before == hb_after && hb_before != 0) {
						if ((ptr = acquire_rx(out_type_id, out_actual_size)) != nullptr) {
							return ptr;
						}
						layout_->header.state.store(BusState::FatalError, std::memory_order_release);
						return nullptr;
					}
				}

				spins = 0;
			}
		}

		return ptr;
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
