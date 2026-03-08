#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include <tachyon.hpp>
#include <tachyon/concepts.hpp>
#include <tachyon/shm.hpp>

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

		explicit Arena(MemoryLayout *layout, size_t capacity) noexcept;

	public:
		~Arena() = default;

		Arena(const Arena &) = delete;

		Arena &operator=(const Arena &) = delete;

		Arena(Arena &&other) noexcept;

		Arena &operator=(Arena &&other) noexcept;

		static auto format(std::span<std::byte> shm_span, size_t capacity) -> std::expected<Arena, ShmError>;

		static auto attach(std::span<std::byte> shm_span) -> std::expected<Arena, ShmError>;

		[[nodiscard]] bool try_push(uint32_t type_id, std::span<const std::byte> data) noexcept;

		[[nodiscard]] bool try_pop(uint32_t &out_type_id, std::span<std::byte> out_buffer, size_t &out_size) noexcept;

		[[nodiscard]] bool pop_spin(
			uint32_t &out_type_id, std::span<std::byte> out_buffer, size_t &out_size, uint32_t max_spins = 0
		) noexcept;

		[[nodiscard]] bool pop_blocking(
			uint32_t &out_type_id, std::span<std::byte> out_buffer, size_t &out_size, uint32_t spin_threshold = 10000
		) noexcept;

		void flush() noexcept;

		template <TachyonPayload T> [[nodiscard]] inline bool push(const uint32_t type_id, const T &payload) noexcept {
			return try_push(type_id, std::span{reinterpret_cast<const std::byte *>(&payload), sizeof(T)});
		}

		template <TachyonPayload T> [[nodiscard]] inline bool pop(uint32_t &out_type_id, T &out_payload) noexcept {
			size_t read_bytes = 0;
			if (const std::span out_span{reinterpret_cast<std::byte *>(&out_payload), sizeof(T)};
				try_pop(out_type_id, out_span, read_bytes)) [[likely]] {
				return read_bytes == sizeof(T);
			}

			return false;
		}

		[[nodiscard]] inline BusState get_state() const noexcept {
			return layout_->header.state.load(std::memory_order_acquire);
		}
	};
} // namespace tachyon::core
