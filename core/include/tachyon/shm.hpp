#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include <tachyon.hpp>

namespace tachyon::core {
	enum class ShmError : uint8_t {
		OpenFailed,
		TruncateFailed,
		MapFailed,
		UnlinkFailed,
		CloseFailed,
		InvalidSize,
		SealFailed,
		ChmodFailed
	};

	class TACHYON_API alignas(64) SharedMemory {
		void	   *ptr_{nullptr};
		size_t		size_{0};
		int			fd_{-1};
		bool		owner_{false};
		std::string name_;

		explicit SharedMemory(void *ptr, const size_t size, std::string name, const int fd, const bool owner)
			: ptr_(ptr), size_(size), fd_(fd), owner_(owner), name_(std::move(name)) {}

		void release() noexcept;

	public:
		~SharedMemory();

		SharedMemory(const SharedMemory &) = delete;

		SharedMemory &operator=(const SharedMemory &) = delete;

		SharedMemory(SharedMemory &&other) noexcept;

		SharedMemory &operator=(SharedMemory &&other) noexcept;

		static auto create(std::string_view name, size_t size) -> std::expected<SharedMemory, ShmError>;

		static auto join(int fd, size_t size) -> std::expected<SharedMemory, ShmError>;

		[[nodiscard]] inline auto data() const noexcept -> std::span<std::byte> {
			return {static_cast<std::byte *>(ptr_), size_};
		}

		[[nodiscard]] inline auto get_ptr() const noexcept -> void * {
			return ptr_;
		}

		[[nodiscard]] inline auto get_size() const noexcept -> size_t {
			return size_;
		}

		[[nodiscard]] inline auto get_fd() const noexcept -> int {
			return fd_;
		}
	};
} // namespace tachyon::core
