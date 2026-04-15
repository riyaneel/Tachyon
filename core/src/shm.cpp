#include <atomic>
#include <fcntl.h>
#include <string>
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

#if defined(__linux__)
		const int fd = ::memfd_create(path.c_str(), MFD_ALLOW_SEALING | MFD_CLOEXEC);
		if (fd == -1) [[unlikely]]
			return std::unexpected(ShmError::OpenFailed);
#elif defined(__APPLE__) // #if defined(__linux__)
		static std::atomic<uint32_t> shm_counter{0};
		const std::string			 shm_name = std::string("/tachyon-") + std::to_string(::getpid()) + "-" +
									 std::to_string(shm_counter.fetch_add(1, std::memory_order_relaxed));
		const int fd = ::shm_open(shm_name.c_str(), O_CREAT | O_RDWR | O_EXCL, 0600);
		if (fd == -1) [[unlikely]]
			return std::unexpected(ShmError::OpenFailed);
		::shm_unlink(shm_name.c_str());
#else					 // #elif defined(__APPLE__)
		(void)path;
		return std::unexpected(ShmError::OpenFailed);
#endif					 // #elif defined(__APPLE__) #else

		if (::ftruncate(fd, static_cast<off_t>(size)) == -1) [[unlikely]] {
			::close(fd);
			return std::unexpected(ShmError::TruncateFailed);
		}

#if defined(__linux__)
		if (::fchmod(fd, 0600) == -1) [[unlikely]] {
			::close(fd);
			return std::unexpected(ShmError::ChmodFailed);
		}

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

#if defined(__linux__)
		::madvise(ptr, size, MADV_DONTFORK); // CoW safety
#endif										 // #if defined(__linux__)

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

#if defined(__linux__)
		::madvise(ptr, size, MADV_DONTFORK); // CoW safety
#endif										 // #if defined(__linux__)

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
