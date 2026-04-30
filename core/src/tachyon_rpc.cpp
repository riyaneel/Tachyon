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
	const char *socket_path, const size_t cap_fwd, const size_t cap_rev, tachyon_rpc_bus_t **out_rpc
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

void tachyon_rpc_destroy(tachyon_rpc_bus_t *rpc) TACHYON_NOEXCEPT {
	if (rpc && rpc->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
		delete rpc;
	}
}

tachyon_error_t tachyon_rpc_call(
	tachyon_rpc_bus_t *rpc,
	const void		  *payload,
	const size_t	   size,
	const uint32_t	   msg_type,
	uint64_t		  *out_correlation_id
) TACHYON_NOEXCEPT {
	if (!rpc || !payload || !out_correlation_id) [[unlikely]] {
		return TACHYON_ERR_NULL_PTR;
	}

	std::byte *ptr = rpc->arena_fwd.acquire_tx(size);
	if (!ptr) [[unlikely]] {
		return TACHYON_ERR_FULL;
	}

	std::memcpy(ptr, payload, size);

	const uint64_t cid	   = rpc->correlation_counter.fetch_add(1, std::memory_order_relaxed);
	const uint32_t type_id = TACHYON_TYPE_ID(0, msg_type);
	if (!rpc->arena_fwd.commit_tx_rpc(size, type_id, cid)) [[unlikely]] {
		return TACHYON_ERR_SYSTEM;
	}

	rpc->arena_fwd.flush_tx();
	*out_correlation_id = cid;
	return TACHYON_SUCCESS;
}

const void *tachyon_rpc_wait(
	tachyon_rpc_bus_t *rpc,
	const uint64_t	   correlation_id,
	size_t			  *out_size,
	uint32_t		  *out_msg_type,
	const uint32_t	   spin_threshold
) TACHYON_NOEXCEPT {
	if (!rpc || !out_size || !out_msg_type) [[unlikely]] {
		return nullptr;
	}

	uint32_t		 type_id	 = 0;
	size_t			 actual_size = 0;
	uint64_t		 recv_cid	 = 0;
	uint32_t		 spins		 = 0;
	const std::byte *ptr		 = nullptr;

	for (;;) {
		ptr = rpc->arena_rev.acquire_rx_rpc(type_id, actual_size, recv_cid);
		if (ptr != nullptr) {
			break;
		}

		if (rpc->arena_rev.get_state() == BusState::FatalError) [[unlikely]] {
			return nullptr;
		}

		if (spins < spin_threshold) {
			tachyon::cpu_relax();
			spins++;
		} else {
			rpc->arena_rev.set_consumer_sleeping(true);
			ptr = rpc->arena_rev.acquire_rx_rpc(type_id, actual_size, recv_cid);
			if (ptr != nullptr) {
				rpc->arena_rev.set_consumer_sleeping(false);
				break;
			}

			const int wait_res = rpc->arena_rev.wait_consumer_sleeping();
			rpc->arena_rev.set_consumer_sleeping(false);
			if (wait_res == -1) // EINTR
				return nullptr;
			spins = 0;
		}
	}

	if (correlation_id != 0 && recv_cid != correlation_id) [[unlikely]] {
		rpc->arena_rev.set_fatal_error();
		return nullptr;
	}

	*out_size	  = actual_size;
	*out_msg_type = TACHYON_MSG_TYPE(type_id);
	return ptr;
}

tachyon_error_t tachyon_rpc_commit_rx(tachyon_rpc_bus_t *rpc) TACHYON_NOEXCEPT {
	if (!rpc) [[unlikely]] {
		return TACHYON_ERR_NULL_PTR;
	}
	return rpc->arena_rev.commit_rx() ? TACHYON_SUCCESS : TACHYON_ERR_SYSTEM;
}

const void *tachyon_rpc_serve(
	tachyon_rpc_bus_t *rpc,
	uint64_t		  *out_correlation_id,
	uint32_t		  *out_msg_type,
	size_t			  *out_size,
	uint32_t		   spin_threshold
) TACHYON_NOEXCEPT {
	if (!rpc || !out_correlation_id || !out_msg_type || !out_size) [[unlikely]]
		return nullptr;

	uint32_t type_id	 = 0;
	size_t	 actual_size = 0;
	uint64_t cid		 = 0;
	uint32_t spins		 = 0;

	const std::byte *ptr = nullptr;

	for (;;) {
		ptr = rpc->arena_fwd.acquire_rx_rpc(type_id, actual_size, cid);
		if (ptr != nullptr) {
			break;
		}

		if (rpc->arena_fwd.get_state() == BusState::FatalError) [[unlikely]] {
			return nullptr;
		}

		if (spins < spin_threshold) {
			tachyon::cpu_relax();
			spins++;
		} else {
			rpc->arena_fwd.set_consumer_sleeping(true);
			ptr = rpc->arena_fwd.acquire_rx_rpc(type_id, actual_size, cid);
			if (ptr) {
				rpc->arena_fwd.set_consumer_sleeping(false);
				break;
			}

			const int wait_res = rpc->arena_fwd.wait_consumer_sleeping();
			rpc->arena_fwd.set_consumer_sleeping(false);
			if (wait_res == -1) // EINTR
				return nullptr;
			spins = 0;
		}
	}

	*out_correlation_id = cid;
	*out_msg_type		= TACHYON_MSG_TYPE(type_id);
	*out_size			= actual_size;
	return ptr;
}

tachyon_error_t tachyon_rpc_commit_serve(tachyon_rpc_bus_t *rpc) TACHYON_NOEXCEPT {
	if (!rpc) [[unlikely]] {
		return TACHYON_ERR_NULL_PTR;
	}
	return rpc->arena_fwd.commit_rx() ? TACHYON_SUCCESS : TACHYON_ERR_SYSTEM;
}

tachyon_error_t tachyon_rpc_reply(
	tachyon_rpc_bus_t *rpc, uint64_t correlation_id, const void *payload, size_t size, uint32_t msg_type
) TACHYON_NOEXCEPT {
	if (!rpc || !payload) [[unlikely]] {
		return TACHYON_ERR_NULL_PTR;
	}

	if (correlation_id == 0) [[unlikely]] {
		return TACHYON_ERR_INVALID_SZ;
	}

	std::byte *ptr = rpc->arena_rev.acquire_tx(size);
	if (!ptr) [[unlikely]] {
		return TACHYON_ERR_FULL;
	}

	std::memcpy(ptr, payload, size);

	const uint32_t type_id = TACHYON_TYPE_ID(0, msg_type);
	if (!rpc->arena_rev.commit_tx_rpc(size, type_id, correlation_id)) [[unlikely]] {
		return TACHYON_ERR_SYSTEM;
	}

	rpc->arena_rev.flush_tx();
	return TACHYON_SUCCESS;
}

void tachyon_rpc_set_polling_mode(const tachyon_rpc_bus_t *rpc, const int pure_spin) TACHYON_NOEXCEPT {
	if (rpc) [[likely]] {
		rpc->arena_fwd.set_polling_mode(pure_spin != 0);
		rpc->arena_rev.set_polling_mode(pure_spin != 0);
	}
}

tachyon_state_t tachyon_rpc_get_state(const tachyon_rpc_bus_t *rpc) TACHYON_NOEXCEPT {
	if (!rpc) [[unlikely]] {
		return TACHYON_STATE_UNKNOWN;
	}

	const BusState fwd_state = rpc->arena_fwd.get_state();
	const BusState rev_state = rpc->arena_rev.get_state();
	if (fwd_state == BusState::FatalError || rev_state == BusState::FatalError) {
		return TACHYON_STATE_FATAL_ERROR;
	}

	if (fwd_state == BusState::Ready && rev_state == BusState::Ready) {
		return TACHYON_STATE_READY;
	}

	return TACHYON_STATE_UNKNOWN;
}
}
