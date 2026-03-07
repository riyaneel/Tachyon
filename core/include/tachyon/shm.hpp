#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace tachyon::core {
	enum class ShmError : uint8_t {
		OpenFailed,
		TruncateFailed,
		MapFailed,
		UnlinkFailed,
		CloseFailed,
		InvalidSize,
		SealFailed
	};

	class SharedMemory {
		void	   *ptr_{nullptr};
		size_t		size_{0};
		std::string name_;
		int			fd_{-1};
		bool		owner_{false};
		uint8_t		padding_[11]{};

		explicit SharedMemory(void *ptr, const size_t size, std::string name, const int fd, const bool owner)
			: ptr_(ptr), size_(size), name_(std::move(name)), fd_(fd), owner_(owner) {}

	public:
		~SharedMemory();

		SharedMemory(const SharedMemory &) = delete;

		SharedMemory &operator=(const SharedMemory &) = delete;

		SharedMemory(SharedMemory &&other) noexcept;

		SharedMemory &operator=(SharedMemory &&other) noexcept;

		static auto create(std::string_view name, size_t size) -> std::expected<SharedMemory, ShmError>;

		static auto join(std::string_view name, size_t size) -> std::expected<SharedMemory, ShmError>;

		[[nodiscard]] auto data() const noexcept -> std::span<std::byte> {
			return {static_cast<std::byte *>(ptr_), size_};
		}

		[[nodiscard]] auto get_ptr() const noexcept -> void * {
			return ptr_;
		}

		[[nodiscard]] auto get_size() const noexcept -> size_t {
			return size_;
		}
	};
} // namespace tachyon::core
