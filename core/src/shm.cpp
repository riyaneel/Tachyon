#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include <tachyon/shm.hpp>

#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif // #ifndef MFD_ALLOW_SEALING

namespace tachyon::core {
	SharedMemory::~SharedMemory() {
		release();
	}

	SharedMemory::SharedMemory(SharedMemory &&other) noexcept
		: ptr_(std::exchange(other.ptr_, nullptr)), size_(std::exchange(other.size_, 0)),
		  fd_(std::exchange(other.fd_, -1)), owner_(std::exchange(other.owner_, false)), name_(std::move(other.name_)) {
	}

	SharedMemory &SharedMemory::operator=(SharedMemory &&other) noexcept {
		if (this != &other) [[likely]] {
			release();
			ptr_   = std::exchange(other.ptr_, nullptr);
			size_  = std::exchange(other.size_, 0);
			fd_	   = std::exchange(other.fd_, -1);
			owner_ = std::exchange(other.owner_, false);
			name_  = std::move(other.name_);
		}
		return *this;
	}

	auto SharedMemory::create(const std::string_view name, const size_t size) -> std::expected<SharedMemory, ShmError> {
		if (size == 0) [[unlikely]]
			return std::unexpected(ShmError::InvalidSize);

		std::string path(name);
		const int	fd = ::memfd_create(path.c_str(), MFD_ALLOW_SEALING);
		if (fd == -1) [[unlikely]]
			return std::unexpected(ShmError::OpenFailed);

		if (::ftruncate(fd, static_cast<off_t>(size)) == -1) [[unlikely]] {
			::close(fd);
			return std::unexpected(ShmError::TruncateFailed);
		}

		if (::fchmod(fd, 0600) == -1) [[unlikely]] {
			::close(fd);
			return std::unexpected(ShmError::ChmodFailed);
		}

#if defined(__linux__)
		if (::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) == -1) [[unlikely]] {
			::close(fd);
			return std::unexpected(ShmError::SealFailed);
		}
#endif // #if defined(__linux__)

		int flags = MAP_SHARED;
#if defined(__linux__)
		flags |= MAP_POPULATE;
#endif // #if defined(__linux__)

		void *ptr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, flags, fd, 0);
		if (ptr == MAP_FAILED) [[unlikely]] {
			::close(fd);
			return std::unexpected(ShmError::MapFailed);
		}

		return SharedMemory(ptr, size, std::move(path), fd, true);
	}

	auto SharedMemory::join(const int fd, const size_t size) -> std::expected<SharedMemory, ShmError> {
		if (fd == -1 || size == 0) [[unlikely]]
			return std::unexpected(ShmError::OpenFailed);

		int flags = MAP_SHARED;
#if defined(__linux__)
		flags |= MAP_POPULATE;
#endif // #if defined(__linux__)

		void *ptr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, flags, fd, 0);
		if (ptr == MAP_FAILED) [[unlikely]] {
			return std::unexpected(ShmError::MapFailed);
		}

		return SharedMemory(ptr, size, "", fd, false);
	}

	void SharedMemory::release() noexcept {
		if (ptr_ && ptr_ != MAP_FAILED) [[likely]] {
			::munmap(ptr_, size_);
			ptr_ = nullptr;
		}
		if (fd_ != -1) [[likely]] {
			::close(fd_);
			fd_ = -1;
		}
	}
} // namespace tachyon::core
