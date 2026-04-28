#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>

#include <tachyon.hpp>
#include <tachyon/concepts.hpp>
#include <tachyon/shm.hpp>

#ifndef TACHYON_MSG_ALIGNMENT
#define TACHYON_MSG_ALIGNMENT 64
#endif // #ifndef TACHYON_MSG_ALIGNMENT

static_assert((TACHYON_MSG_ALIGNMENT & (TACHYON_MSG_ALIGNMENT - 1)) == 0, "TACHYON_MSG_ALIGNMENT must be a power of 2");
static_assert(TACHYON_MSG_ALIGNMENT >= 32, "TACHYON_MSG_ALIGNMENT must be at least 32 bytes");

namespace tachyon::core {
	constexpr uint32_t TACHYON_MAGIC   = 0x54414348;
	constexpr uint32_t TACHYON_VERSION = 0x04;

	constexpr uint32_t CONSUMER_AWAKE	  = 0;
	constexpr uint32_t CONSUMER_SLEEPING  = 1;
	constexpr uint32_t CONSUMER_PURE_SPIN = 2;

	enum class BusState : uint32_t {
		Uninitialized = 0,
		Initializing  = 1,
		Ready		  = 2,
		Disconnected  = 3,
		FatalError	  = 4,
		Unknown		  = 5
	};

	struct alignas(TACHYON_MSG_ALIGNMENT) MessageHeader {
		uint32_t size;
		uint32_t type_id; /* bits [0:15] = msg_type, bits [16:31] = route_id */
		uint32_t reserved_size;
		uint64_t correlation_id;
		uint8_t padding_[TACHYON_MSG_ALIGNMENT - (sizeof(uint32_t) * 4) - sizeof(uint64_t)];
	};

	struct alignas(128) ArenaHeader {
		uint32_t			  magic;
		uint32_t			  version;
		uint32_t			  capacity;
		uint32_t			  msg_alignment;
		std::atomic<BusState> state;
	};

	struct SPSCIndices {
		alignas(128) std::atomic<size_t> head{0};
		alignas(128) std::atomic<size_t> tail{0};
		alignas(128) std::atomic<uint32_t> consumer_sleeping{0};
		alignas(128) std::atomic<uint64_t> producer_heartbeat{0};
		alignas(128) std::atomic<uint64_t> consumer_heartbeat{0};
	};

	struct alignas(128) MemoryLayout {
		ArenaHeader header;
		SPSCIndices indices;

		[[nodiscard]] inline std::byte *data_arena() noexcept {
			return reinterpret_cast<std::byte *>(this + 1);
		}
	};

	struct alignas(32) RxView {
		const std::byte *ptr;
		size_t			 actual_size;
		size_t			 reserved_;
		uint32_t		 type_id; /* bits [0:15] = msg_type, bits [16:31] = route_id */
		uint32_t		 padding_;
	};

	static_assert(sizeof(RxView) == 32, "RxView must be exactly 32 bytes sized.");

	class TACHYON_API Arena {
		MemoryLayout *layout_{nullptr};
		size_t		  capacity_mask_{0};

		static constexpr size_t BATCH_SIZE = 32;
		alignas(64) size_t local_head_{0};
		size_t cached_tail_{0};
		size_t pending_tx_{0};
		size_t tx_reserved_size_{0};
		size_t pre_acquire_head_{0};
		alignas(64) size_t local_tail_{0};
		size_t cached_head_{0};
		size_t pending_rx_{0};
		size_t rx_reserved_size_{0};

		explicit Arena(MemoryLayout *layout, size_t capacity) noexcept;

	public:
		~Arena() = default;

		Arena(const Arena &) = delete;

		Arena &operator=(const Arena &) = delete;

		Arena(Arena &&other) noexcept;

		Arena &operator=(Arena &&other) noexcept;

		static auto format(std::span<std::byte> shm_span, size_t capacity) -> std::expected<Arena, ShmError>;

		static auto attach(std::span<std::byte> shm_span) -> std::expected<Arena, ShmError>;

		[[nodiscard]] std::byte *acquire_tx(size_t max_size) noexcept;

		[[nodiscard]] bool commit_tx(size_t actual_size, uint32_t type_id) noexcept;

		[[nodiscard]] bool rollback_tx() noexcept;

		[[nodiscard]] const std::byte *acquire_rx(uint32_t &out_type_id, size_t &out_actual_size) noexcept;

		[[nodiscard]] bool commit_rx() noexcept;

		[[nodiscard]] size_t acquire_rx_batch(RxView *views, size_t max_msgs) noexcept;

		bool commit_rx_batch(const RxView *views, size_t count) noexcept;

		[[nodiscard]] const std::byte *
		acquire_rx_spin(uint32_t &out_type_id, size_t &out_actual_size, uint32_t max_spins = 0) noexcept;

		[[nodiscard]] const std::byte *
		acquire_rx_blocking(uint32_t &out_type_id, size_t &out_actual_size, uint32_t spin_threshold = 10000) noexcept;

		void flush() noexcept;

		void flush_tx() noexcept;

		void set_consumer_sleeping(bool sleeping) const noexcept;

		void set_polling_mode(bool pure_spin) const noexcept;

		int wait_consumer_sleeping() const noexcept;

		uint64_t get_producer_heartbeat() const noexcept;

		void set_fatal_error() const noexcept;

		template <TachyonPayload T> [[nodiscard]] inline bool push(const uint32_t type_id, const T &payload) noexcept {
			if (std::byte *ptr = acquire_tx(sizeof(T))) [[likely]] {
				std::memcpy(ptr, &payload, sizeof(T));
				return commit_tx(sizeof(T), type_id);
			}
			return false;
		}

		template <TachyonPayload T> [[nodiscard]] inline bool pop(uint32_t &out_type_id, T &out_payload) noexcept {
			size_t actual_size = 0;
			if (const std::byte *ptr = acquire_rx(out_type_id, actual_size)) [[likely]] {
				if (actual_size == sizeof(T)) {
					std::memcpy(&out_payload, ptr, sizeof(T));
					return commit_rx();
				}
				(void)commit_rx();
			}
			return false;
		}

		[[nodiscard]] inline BusState get_state() const noexcept {
			return layout_->header.state.load(std::memory_order_acquire);
		}
	};
} // namespace tachyon::core
