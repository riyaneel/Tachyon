#include <new>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <tachyon.h>
#include <tachyon/arena.hpp>
#include <tachyon/shm.hpp>

using namespace tachyon::core;

struct tachyon_bus {
	SharedMemory		  shm;
	Arena				  arena;
	std::atomic<uint32_t> ref_count{1};
	std::atomic_flag	  producer_lock = ATOMIC_FLAG_INIT;
	std::atomic_flag	  consumer_lock = ATOMIC_FLAG_INIT;
	uint8_t				  padding_[42]{};

	tachyon_bus(SharedMemory &&s, Arena &&a) : shm(std::move(s)), arena(std::move(a)) {}
};

namespace {
	struct TasGuard {
		std::atomic_flag &flag_;

		explicit TasGuard(std::atomic_flag &flag) TACHYON_NOEXCEPT : flag_(flag) {
			while (flag_.test_and_set(std::memory_order_acquire))
				tachyon::cpu_relax();
		}

		~TasGuard() {
			flag_.clear(std::memory_order_release);
		}
	};

	struct TachyonHandshake {
		uint32_t magic;
		uint32_t version;
		uint32_t capacity;
		uint32_t shm_size;
	};
} // namespace

static tachyon_error_t map_shm_error(const ShmError error) TACHYON_NOEXCEPT {
	switch (error) {
	case ShmError::OpenFailed:
		return TACHYON_ERR_OPEN;
	case ShmError::TruncateFailed:
		return TACHYON_ERR_TRUNCATE;
	case ShmError::MapFailed:
		return TACHYON_ERR_MAP;
	case ShmError::InvalidSize:
		return TACHYON_ERR_INVALID_SZ;
	case ShmError::SealFailed:
		return TACHYON_ERR_SEAL;
	case ShmError::ChmodFailed:
		return TACHYON_ERR_CHMOD;
	default:
		return TACHYON_ERR_SYSTEM;
	}
}

extern "C" {

void tachyon_memory_barrier_acquire(void) TACHYON_NOEXCEPT {
	std::atomic_thread_fence(std::memory_order_acquire);
}

tachyon_error_t
tachyon_bus_listen(const char *socket_path, const size_t capacity, tachyon_bus_t **out_bus) TACHYON_NOEXCEPT {
	if (!socket_path || !out_bus || capacity == 0)
		return TACHYON_ERR_INVALID_SZ;

	const size_t required_shm_size = sizeof(MemoryLayout) + capacity;
	auto		 shm_res		   = SharedMemory::create(socket_path, required_shm_size);
	if (!shm_res.has_value())
		return map_shm_error(shm_res.error());

	auto arena_res = Arena::format(shm_res->data(), capacity);
	if (!arena_res.has_value())
		return map_shm_error(arena_res.error());

	auto *bus = new (std::nothrow) tachyon_bus(std::move(shm_res.value()), std::move(arena_res.value()));
	if (!bus)
		return TACHYON_ERR_MEM;

	const int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0) {
		delete bus;
		return TACHYON_ERR_NETWORK;
	}

	struct sockaddr_un addr = {};
	addr.sun_family			= AF_UNIX;
	std::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

	::unlink(socket_path);

	if (::bind(sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0 || ::listen(sock, 1) < 0) {
		::close(sock);
		delete bus;
		return TACHYON_ERR_NETWORK;
	}

	const int client_sock = ::accept(sock, nullptr, nullptr);
	if (client_sock < 0) {
		::close(sock);
		delete bus;
		return TACHYON_ERR_NETWORK;
	}

	struct msghdr	 msg{};
	char			 buf[CMSG_SPACE(sizeof(int))] = {};
	TachyonHandshake handshake					  = {
		   TACHYON_MAGIC, TACHYON_VERSION, static_cast<uint32_t>(capacity), static_cast<uint32_t>(required_shm_size)
	   };

	struct iovec io	   = {&handshake, sizeof(handshake)};
	msg.msg_iov		   = &io;
	msg.msg_iovlen	   = 1;
	msg.msg_control	   = buf;
	msg.msg_controllen = sizeof(buf);

	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

	if (!cmsg) {
		::close(client_sock);
		::close(sock);
		delete bus;
		return TACHYON_ERR_SYSTEM;
	}

	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type	 = SCM_RIGHTS;
	cmsg->cmsg_len	 = CMSG_LEN(sizeof(int));

	const int fd_to_send = bus->shm.get_fd();
	std::memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));

	if (::sendmsg(client_sock, &msg, 0) < 0) {
		::close(client_sock);
		::close(sock);
		delete bus;
		return TACHYON_ERR_NETWORK;
	}

	::close(client_sock);
	::close(sock);
	*out_bus = bus;
	return TACHYON_SUCCESS;
}

tachyon_error_t tachyon_bus_connect(const char *socket_path, tachyon_bus_t **out_bus) TACHYON_NOEXCEPT {
	if (!socket_path || !out_bus)
		return TACHYON_ERR_INVALID_SZ;

	const int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0)
		return TACHYON_ERR_NETWORK;

	struct sockaddr_un addr = {};
	addr.sun_family			= AF_UNIX;
	std::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

	if (::connect(sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(sock);
		return TACHYON_ERR_NETWORK;
	}

	struct msghdr	 msg{};
	TachyonHandshake handshake{};
	struct iovec	 io = {&handshake, sizeof(handshake)};
	msg.msg_iov			= &io;
	msg.msg_iovlen		= 1;

	char c_buffer[256];
	msg.msg_control	   = c_buffer;
	msg.msg_controllen = sizeof(c_buffer);

	if (::recvmsg(sock, &msg, 0) < static_cast<ssize_t>(sizeof(handshake))) {
		::close(sock);
		return TACHYON_ERR_NETWORK;
	}

	if (handshake.magic != TACHYON_MAGIC || handshake.version != TACHYON_VERSION) {
		::close(sock);
		return TACHYON_ERR_NETWORK;
	}

	const struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) {
		::close(sock);
		return TACHYON_ERR_NETWORK;
	}

	int received_fd = -1;
	std::memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(int));

	::close(sock);

	if (received_fd < 0) {
		return TACHYON_ERR_SYSTEM;
	}

	struct stat st = {};
	if (::fstat(received_fd, &st) < 0) {
		::close(received_fd);
		return TACHYON_ERR_SYSTEM;
	}

	auto shm_join = SharedMemory::join(received_fd, static_cast<size_t>(st.st_size));
	if (!shm_join.has_value())
		return map_shm_error(shm_join.error());

	auto arena_res = Arena::attach(shm_join->data());
	if (!arena_res.has_value())
		return map_shm_error(arena_res.error());

	*out_bus = new (std::nothrow) tachyon_bus(std::move(shm_join.value()), std::move(arena_res.value()));
	if (!*out_bus)
		return TACHYON_ERR_MEM;

	return TACHYON_SUCCESS;
}

void tachyon_bus_ref(tachyon_bus_t *bus) TACHYON_NOEXCEPT {
	if (bus) [[likely]] {
		bus->ref_count.fetch_add(1, std::memory_order_relaxed);
	}
}

void tachyon_bus_destroy(tachyon_bus_t *bus) TACHYON_NOEXCEPT {
	if (bus && bus->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
		delete bus;
	}
}

void *tachyon_acquire_tx(tachyon_bus_t *bus, const size_t max_payload_size) TACHYON_NOEXCEPT {
	if (!bus || max_payload_size == 0)
		return nullptr;

	while (bus->producer_lock.test_and_set(std::memory_order_acquire)) {
		tachyon::cpu_relax();
	}

	std::byte *ptr = bus->arena.acquire_tx(max_payload_size);
	if (!ptr) {
		bus->producer_lock.clear(std::memory_order_release);
		return nullptr;
	}

	return ptr;
}

tachyon_error_t
tachyon_commit_tx(tachyon_bus_t *bus, const size_t actual_payload_size, const uint32_t type_id) TACHYON_NOEXCEPT {
	if (!bus)
		return TACHYON_ERR_NULL_PTR;

	const bool success = bus->arena.commit_tx(actual_payload_size, type_id);
	bus->producer_lock.clear(std::memory_order_release);

	return success ? TACHYON_SUCCESS : TACHYON_ERR_SYSTEM;
}

const void *tachyon_acquire_rx(tachyon_bus_t *bus, uint32_t *out_type_id, size_t *out_actual_size) TACHYON_NOEXCEPT {
	if (!bus || !out_type_id || !out_actual_size)
		return nullptr;

	while (bus->consumer_lock.test_and_set(std::memory_order_acquire)) {
		tachyon::cpu_relax();
	}

	const std::byte *ptr = bus->arena.acquire_rx(*out_type_id, *out_actual_size);
	if (!ptr) {
		bus->consumer_lock.clear(std::memory_order_release);
		return nullptr;
	}

	return ptr;
}

const void *tachyon_acquire_rx_spin(
	tachyon_bus_t *bus, uint32_t *out_type_id, size_t *out_actual_size, const uint32_t max_spins
) TACHYON_NOEXCEPT {
	if (!bus || !out_type_id || !out_actual_size)
		return nullptr;

	while (bus->consumer_lock.test_and_set(std::memory_order_acquire)) {
		tachyon::cpu_relax();
	}

	const std::byte *ptr = bus->arena.acquire_rx_spin(*out_type_id, *out_actual_size, max_spins);
	if (!ptr) {
		bus->consumer_lock.clear(std::memory_order_release);
		return nullptr;
	}

	return ptr;
}

const void *tachyon_acquire_rx_blocking(
	tachyon_bus_t *bus, uint32_t *out_type_id, size_t *out_actual_size, const uint32_t spin_threshold
) TACHYON_NOEXCEPT {
	if (!bus || !out_type_id || !out_actual_size)
		return nullptr;

	while (bus->consumer_lock.test_and_set(std::memory_order_acquire)) {
		tachyon::cpu_relax();
	}

	const std::byte *ptr = bus->arena.acquire_rx_blocking(*out_type_id, *out_actual_size, spin_threshold);
	if (!ptr) {
		bus->consumer_lock.clear(std::memory_order_release);
		return nullptr;
	}

	return ptr;
}

tachyon_error_t tachyon_commit_rx(tachyon_bus_t *bus) TACHYON_NOEXCEPT {
	if (!bus)
		return TACHYON_ERR_NULL_PTR;

	const bool success = bus->arena.commit_rx();
	bus->consumer_lock.clear(std::memory_order_release);

	return success ? TACHYON_SUCCESS : TACHYON_ERR_SYSTEM;
}

void tachyon_flush(tachyon_bus_t *bus) TACHYON_NOEXCEPT {
	if (bus) {
		TasGuard p_lock(bus->producer_lock);
		TasGuard c_lock(bus->consumer_lock);
		bus->arena.flush();
	}
}
} // extern "C"
