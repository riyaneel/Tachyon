#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

#include <tachyon/shm.hpp>

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
		const int	fd = ::shm_open(path.c_str(), O_CREAT | O_RDWR | O_EXCL, 0600);
		if (fd == -1) [[unlikely]]
			return std::unexpected(ShmError::OpenFailed);

		if (::ftruncate(fd, static_cast<off_t>(size)) == -1) [[unlikely]] {
			::close(fd);
			::shm_unlink(path.c_str());
			return std::unexpected(ShmError::TruncateFailed);
		}

#if defined(__linux__)
		if (::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) == -1) {
		}
#endif // #if defined(__linux__)

		int flags = MAP_SHARED;
#if defined(__linux__)
		flags |= MAP_POPULATE;
#endif // #if defined(__linux__)

		void *ptr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, flags, fd, 0);
		if (ptr == MAP_FAILED) [[unlikely]] {
			::close(fd);
			::shm_unlink(path.c_str());
			return std::unexpected(ShmError::MapFailed);
		}

		return SharedMemory(ptr, size, std::move(path), fd, true);
	}

	auto SharedMemory::join(const std::string_view name, const size_t size) -> std::expected<SharedMemory, ShmError> {
		std::string path(name);
		const int	fd = ::shm_open(path.c_str(), O_RDWR, 0600);
		if (fd == -1) [[unlikely]]
			return std::unexpected(ShmError::OpenFailed);

		void *ptr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (ptr == MAP_FAILED) [[unlikely]] {
			::close(fd);
			return std::unexpected(ShmError::MapFailed);
		}

		return SharedMemory(ptr, size, std::move(path), fd, false);
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
		if (owner_ && !name_.empty()) {
			::shm_unlink(name_.c_str());
		}
	}
} // namespace tachyon::core
