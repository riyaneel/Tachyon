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
#endif

namespace tachyon::core {
	constexpr uint32_t TACHYON_MAGIC   = 0x54414348;
	constexpr uint32_t TACHYON_VERSION = 0x01;

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
		uint32_t type_id;
		uint32_t reserved_size;
		uint8_t	 padding_[TACHYON_MSG_ALIGNMENT - sizeof(uint32_t) * 3];
	};

	struct alignas(128) ArenaHeader {
		uint32_t			  magic;
		uint32_t			  version;
		uint32_t			  capacity;
		std::atomic<BusState> state;
	};

	struct SPSCIndices {
		alignas(128) std::atomic<size_t> head{0};
		alignas(128) std::atomic<size_t> tail{0};
		alignas(128) std::atomic<uint32_t> consumer_sleeping{0};
	};

	struct alignas(128) MemoryLayout {
		ArenaHeader header;
		SPSCIndices indices;

		[[nodiscard]] inline std::byte *data_arena() noexcept {
			return reinterpret_cast<std::byte *>(this + 1);
		}
	};

	class TACHYON_API Arena {
		MemoryLayout *layout_{nullptr};
		size_t		  capacity_mask_{0};

		static constexpr size_t BATCH_SIZE = 32;
		size_t					local_head_{0};
		size_t					cached_tail_{0};
		size_t					pending_tx_{0};
		size_t					local_tail_{0};
		size_t					cached_head_{0};
		size_t					pending_rx_{0};
		size_t					tx_reserved_size_{0};
		size_t					rx_reserved_size_{0};

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

		[[nodiscard]] const std::byte *acquire_rx(uint32_t &out_type_id, size_t &out_actual_size) noexcept;

		[[nodiscard]] bool commit_rx() noexcept;

		[[nodiscard]] const std::byte *
		acquire_rx_spin(uint32_t &out_type_id, size_t &out_actual_size, uint32_t max_spins = 0) noexcept;

		[[nodiscard]] const std::byte *
		acquire_rx_blocking(uint32_t &out_type_id, size_t &out_actual_size, uint32_t spin_threshold = 10000) noexcept;

		void flush() noexcept;

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
