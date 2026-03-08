#include <cstring>
#include <utility>

#include <tachyon/arena.hpp>

namespace tachyon::core {
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

		auto *layout = reinterpret_cast<MemoryLayout *>(shm_span.data());

		layout->header.magic	= TACHYON_MAGIC;
		layout->header.version	= TACHYON_VERSION;
		layout->header.capacity = static_cast<uint32_t>(capacity);
		layout->indices.head.store(0, std::memory_order_relaxed);
		layout->indices.tail.store(0, std::memory_order_relaxed);
		layout->header.state.store(BusState::Ready, std::memory_order_release);

		return Arena(layout, capacity);
	}

	auto Arena::attach(const std::span<std::byte> shm_span) -> std::expected<Arena, ShmError> {
		if (shm_span.size() < sizeof(MemoryLayout)) [[unlikely]]
			return std::unexpected(ShmError::InvalidSize);

		auto *layout = reinterpret_cast<MemoryLayout *>(shm_span.data());
		if (layout->header.magic != TACHYON_MAGIC) [[unlikely]]
			return std::unexpected(ShmError::MapFailed);

		const size_t capacity = layout->header.capacity;
		if (!is_power_of_two(capacity) || shm_span.size() < sizeof(MemoryLayout) + capacity) [[unlikely]]
			return std::unexpected(ShmError::InvalidSize);

		return Arena(layout, capacity);
	}

	bool Arena::try_push(const std::span<const std::byte> data) noexcept {
		const size_t msg_size		= data.size();
		const size_t required_space = sizeof(uint32_t) + msg_size;
		const size_t capacity		= capacity_mask_ + 1;
		if (required_space > capacity) [[unlikely]]
			return false;

		if (local_head_ - cached_tail_ + required_space > capacity) {
			cached_tail_ = layout_->indices.tail.load(std::memory_order_acquire);
			if (local_head_ - cached_tail_ + required_space > capacity) [[unlikely]]
				return false;
		}

		const auto	 header_size = static_cast<uint32_t>(msg_size);
		const size_t header_idx	 = local_head_ & capacity_mask_;
		if (const size_t header_space = capacity - header_idx; header_space >= sizeof(uint32_t)) [[likely]] {
			std::memcpy(&layout_->data_arena()[header_idx], &header_size, sizeof(uint32_t));
		} else {
			std::memcpy(&layout_->data_arena()[header_idx], &header_size, header_space);
			std::memcpy(
				&layout_->data_arena()[0],
				reinterpret_cast<const std::byte *>(&header_size) + header_space,
				sizeof(uint32_t) - header_space
			);
		}

		const size_t payload_offset = local_head_ + sizeof(uint32_t);
		const size_t physical_idx	= payload_offset & capacity_mask_;
		if (const size_t space_until_end = capacity - physical_idx; space_until_end >= msg_size) [[likely]] {
			std::memcpy(&layout_->data_arena()[physical_idx], data.data(), msg_size);
		} else {
			std::memcpy(&layout_->data_arena()[physical_idx], data.data(), space_until_end);
			std::memcpy(&layout_->data_arena()[0], data.data() + space_until_end, msg_size - space_until_end);
		}

		local_head_ += required_space;
		pending_tx_++;

		if ((pending_tx_ & (BATCH_SIZE - 1)) == 0) [[unlikely]] {
			layout_->indices.head.store(local_head_, std::memory_order_release);
		}

		return true;
	}

	bool Arena::try_pop(std::span<std::byte> out_buffer, size_t &out_size) noexcept {
		if (cached_head_ <= local_tail_) {
			cached_head_ = layout_->indices.head.load(std::memory_order_acquire);
			if (cached_head_ <= local_tail_) [[likely]]
				return false;
		}

		uint32_t	 msg_size	= 0;
		const size_t header_idx = local_tail_ & capacity_mask_;
		if (const auto header_space = (capacity_mask_ + 1) - header_idx; header_space >= sizeof(uint32_t)) [[likely]] {
			std::memcpy(&msg_size, &layout_->data_arena()[header_idx], sizeof(uint32_t));
		} else {
			std::memcpy(&msg_size, &layout_->data_arena()[header_idx], header_space);
			std::memcpy(
				reinterpret_cast<std::byte *>(&msg_size) + header_space,
				&layout_->data_arena()[0],
				sizeof(uint32_t) - header_space
			);
		}

		if (out_buffer.size() < msg_size) [[unlikely]]
			return false;

		const size_t payload_offset	 = local_tail_ + sizeof(uint32_t);
		const size_t physical_idx	 = payload_offset & capacity_mask_;
		if (const auto space_until_end = (capacity_mask_ + 1) - physical_idx; space_until_end >= msg_size) [[likely]] {
			std::memcpy(out_buffer.data(), &layout_->data_arena()[physical_idx], msg_size);
		} else {
			std::memcpy(out_buffer.data(), &layout_->data_arena()[physical_idx], space_until_end);
			std::memcpy(out_buffer.data() + space_until_end, &layout_->data_arena()[0], msg_size - space_until_end);
		}

		out_size = msg_size;
		local_tail_ += sizeof(uint32_t) + msg_size;
		pending_rx_++;

		if ((pending_rx_ & (BATCH_SIZE - 1)) == 0) [[unlikely]] {
			layout_->indices.tail.store(local_tail_, std::memory_order_release);
		}

		return true;
	}

	void Arena::flush() noexcept {
		if ((pending_tx_ & (BATCH_SIZE - 1)) != 0) {
			layout_->indices.head.store(local_head_, std::memory_order_release);
			pending_tx_ = 0;
		}
		if ((pending_rx_ & (BATCH_SIZE - 1)) != 0) {
			layout_->indices.tail.store(local_tail_, std::memory_order_release);
			pending_rx_ = 0;
		}
	}
} // namespace tachyon::core
