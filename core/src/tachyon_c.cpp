#include <cstring>
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
	SharedMemory shm;
	Arena		 arena;

	tachyon_bus(SharedMemory &&s, Arena &&a) : shm(std::move(s)), arena(std::move(a)) {}
};

static tachyon_error_t map_shm_error(const ShmError error) noexcept {
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
tachyon_error_t tachyon_bus_listen(const char *socket_path, const size_t capacity, tachyon_bus_t **out_bus) noexcept {
	if (!socket_path || !out_bus || capacity == 0)
		return TACHYON_ERR_INVALID_SZ;

	const size_t required_shm_size = sizeof(MemoryLayout) + capacity;
	auto		 shm_res		   = SharedMemory::create(socket_path, required_shm_size);
	if (!shm_res.has_value())
		return map_shm_error(shm_res.error());

	auto arena_res = Arena::format(shm_res->data(), capacity);
	if (!arena_res.has_value())
		return map_shm_error(arena_res.error());

	*out_bus = new (std::nothrow) tachyon_bus(std::move(shm_res.value()), std::move(arena_res.value()));
	if (!*out_bus)
		return TACHYON_ERR_MEM;

	const int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0)
		return TACHYON_ERR_NETWORK;

	struct sockaddr_un addr = {};
	addr.sun_family			= AF_UNIX;
	std::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

	::unlink(socket_path);

	if (::bind(sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0 || ::listen(sock, 1) < 0) {
		::close(sock);
		return TACHYON_ERR_NETWORK;
	}

	const int client_sock = ::accept(sock, nullptr, nullptr);
	if (client_sock < 0) {
		::close(sock);
		return TACHYON_ERR_NETWORK;
	}

	struct msghdr msg{};
	char		  buf[CMSG_SPACE(sizeof(int))] = {};

	struct iovec io	   = {const_cast<void *>(reinterpret_cast<const void *>("FD")), 2};
	msg.msg_iov		   = &io;
	msg.msg_iovlen	   = 1;
	msg.msg_control	   = buf;
	msg.msg_controllen = sizeof(buf);

	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

	if (!cmsg) {
		::close(client_sock);
		::close(sock);
		return TACHYON_ERR_SYSTEM;
	}

	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type	 = SCM_RIGHTS;
	cmsg->cmsg_len	 = CMSG_LEN(sizeof(int));

	const int fd_to_send = (*out_bus)->shm.get_fd();
	std::memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));

	if (::sendmsg(client_sock, &msg, 0) < 0) {
		::close(client_sock);
		::close(sock);
		return TACHYON_ERR_NETWORK;
	}

	::close(client_sock);
	::close(sock);
	return TACHYON_SUCCESS;
}

tachyon_error_t tachyon_bus_connect(const char *socket_path, tachyon_bus_t **out_bus) noexcept {
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

	struct msghdr msg{};
	char		  m_buffer[256];
	struct iovec  io = {m_buffer, sizeof(m_buffer)};
	msg.msg_iov		 = &io;
	msg.msg_iovlen	 = 1;

	char c_buffer[256];
	msg.msg_control	   = c_buffer;
	msg.msg_controllen = sizeof(c_buffer);

	if (::recvmsg(sock, &msg, 0) < 0) {
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

	struct stat st;
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

void tachyon_bus_destroy(const tachyon_bus_t *bus) noexcept {
	if (bus) {
		delete bus;
	}
}

tachyon_error_t tachyon_push(tachyon_bus_t *bus, const uint32_t type_id, const void *data, const size_t size) noexcept {
	if (!bus || !data)
		return TACHYON_ERR_NULL_PTR;
	const std::span payload{static_cast<const std::byte *>(data), size};
	return bus->arena.try_push(type_id, payload) ? TACHYON_SUCCESS : TACHYON_ERR_FULL;
}

tachyon_error_t tachyon_try_pop(
	tachyon_bus_t *bus, uint32_t *out_type_id, void *out_buffer, const size_t buffer_capacity, size_t *out_read_size
) noexcept {
	if (!bus || !out_type_id || !out_buffer || !out_read_size)
		return TACHYON_ERR_NULL_PTR;
	const std::span out_span{static_cast<std::byte *>(out_buffer), buffer_capacity};
	return bus->arena.try_pop(*out_type_id, out_span, *out_read_size) ? TACHYON_SUCCESS : TACHYON_ERR_EMPTY;
}

tachyon_error_t tachyon_pop_spin(
	tachyon_bus_t *bus,
	uint32_t	  *out_type_id,
	void		  *out_buffer,
	const size_t   buffer_capacity,
	size_t		  *out_read_size,
	const uint32_t max_spins
) noexcept {
	if (!bus || !out_type_id || !out_buffer || !out_read_size)
		return TACHYON_ERR_NULL_PTR;
	const std::span out_span{static_cast<std::byte *>(out_buffer), buffer_capacity};
	return bus->arena.pop_spin(*out_type_id, out_span, *out_read_size, max_spins) ? TACHYON_SUCCESS : TACHYON_ERR_EMPTY;
}

void tachyon_flush(tachyon_bus_t *bus) noexcept {
	if (bus)
		bus->arena.flush();
}
} // extern "C"
