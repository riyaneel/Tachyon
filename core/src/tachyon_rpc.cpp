#include <atomic>
#include <new>

#include <tachyon.h>
#include <tachyon/arena.hpp>
#include <tachyon/shm.hpp>
#include <tachyon/transport.hpp>

using namespace tachyon::core;

struct alignas(64) tachyon_rpc_bus {
	SharedMemory		  shm_fwd;
	Arena				  arena_fwd; /* caller -> callee direction (caller writes, callee reads) */
	SharedMemory		  shm_rev;
	Arena				  arena_rev;			  /* callee -> caller direction (callee writes, caller reads) */
	std::atomic<uint64_t> correlation_counter{1}; /* 0 for reserved sentinel */
	std::atomic<uint32_t> ref_count{1};

	tachyon_rpc_bus(SharedMemory &&s_fwd, Arena &&a_fwd, SharedMemory &&s_rev, Arena &&a_rev)
		: shm_fwd(std::move(s_fwd)), arena_fwd(std::move(a_fwd)), shm_rev(std::move(s_rev)),
		  arena_rev(std::move(a_rev)) {}
};

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

static tachyon_error_t map_transport_error(const TransportError error) TACHYON_NOEXCEPT {
	switch (error) {
	case TransportError::ProtocolMismatch:
		return TACHYON_ERR_ABI_MISMATCH;
	case TransportError::Interrupted:
		return TACHYON_ERR_INTERRUPTED;
	default:
		return TACHYON_ERR_NETWORK;
	}
}

extern "C" {

tachyon_error_t tachyon_rpc_listen(
	const char *socket_path, size_t cap_fwd, size_t cap_rev, tachyon_rpc_bus_t **out_rpc
) TACHYON_NOEXCEPT {
	if (!socket_path || !out_rpc || cap_fwd == 0 || cap_rev == 0) {
		return TACHYON_ERR_INVALID_SZ;
	}

	const size_t shm_size_fwd = sizeof(MemoryLayout) + cap_fwd;
	const size_t shm_size_rev = sizeof(MemoryLayout) + cap_rev;

	auto shm_fwd = SharedMemory::create(socket_path, shm_size_fwd);
	if (!shm_fwd.has_value())
		return map_shm_error(shm_fwd.error());

	auto shm_rev = SharedMemory::create(socket_path, shm_size_rev);
	if (!shm_rev.has_value())
		return map_shm_error(shm_rev.error());

	auto arena_fwd = Arena::format(shm_fwd->data(), cap_fwd);
	if (!arena_fwd.has_value())
		return map_shm_error(arena_fwd.error());

	auto arena_rev = Arena::format(shm_rev->data(), cap_rev);
	if (!arena_rev.has_value())
		return map_shm_error(arena_rev.error());

	auto *rpc = new (std::nothrow) tachyon_rpc_bus(
		std::move(shm_fwd.value()),
		std::move(arena_fwd.value()),
		std::move(shm_rev.value()),
		std::move(arena_rev.value())
	);
	if (!rpc) {
		return TACHYON_ERR_MEM;
	}

	const TachyonHandshake handshake = {
		.magic		   = TACHYON_MAGIC,
		.version	   = TACHYON_VERSION,
		.capacity_fwd  = static_cast<uint32_t>(cap_fwd),
		.shm_size_fwd  = static_cast<uint32_t>(shm_size_fwd),
		.capacity_rev  = static_cast<uint32_t>(cap_rev),
		.shm_size_rev  = static_cast<uint32_t>(shm_size_rev),
		.msg_alignment = TACHYON_MSG_ALIGNMENT,
		.flags		   = TACHYON_FLAGS_RPC
	};

	if (auto transport_res = uds_export_shm_rpc(socket_path, rpc->shm_fwd.get_fd(), rpc->shm_rev.get_fd(), handshake);
		!transport_res.has_value()) {
		delete rpc;
		return map_transport_error(transport_res.error());
	}

	*out_rpc = rpc;
	return TACHYON_SUCCESS;
}

tachyon_error_t tachyon_rpc_connect(const char *socket_path, tachyon_rpc_bus_t **out_rpc) TACHYON_NOEXCEPT {
	if (!socket_path || !out_rpc) {
		return TACHYON_ERR_INVALID_SZ;
	}

	auto transport = uds_import_shm_rpc(socket_path);
	if (!transport.has_value()) {
		return map_transport_error(transport.error());
	}

	const auto &[fd_fwd, fd_rev, hs] = transport.value();

	if (hs.magic != TACHYON_MAGIC || hs.version != TACHYON_VERSION || hs.msg_alignment != TACHYON_MSG_ALIGNMENT ||
		hs.flags != TACHYON_FLAGS_RPC) {
		::close(fd_fwd);
		::close(fd_rev);
		return TACHYON_ERR_ABI_MISMATCH;
	}

	auto shm_fwd = SharedMemory::join(fd_fwd, hs.shm_size_fwd);
	if (!shm_fwd.has_value())
		return map_shm_error(shm_fwd.error());

	auto shm_rev = SharedMemory::join(fd_rev, hs.shm_size_rev);
	if (!shm_rev.has_value())
		return map_shm_error(shm_rev.error());

	auto arena_fwd = Arena::attach(shm_fwd->data());
	if (!arena_fwd.has_value())
		return map_shm_error(arena_fwd.error());

	auto arena_rev = Arena::attach(shm_rev->data());
	if (!arena_rev.has_value())
		return map_shm_error(arena_rev.error());

	auto *rpc = new (std::nothrow) tachyon_rpc_bus(
		std::move(shm_fwd.value()),
		std::move(arena_fwd.value()),
		std::move(shm_rev.value()),
		std::move(arena_rev.value())
	);
	if (!rpc) {
		return TACHYON_ERR_MEM;
	}

	*out_rpc = rpc;
	return TACHYON_SUCCESS;
}
}
