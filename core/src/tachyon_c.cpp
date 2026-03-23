#include <new>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>

// Inline the constants to avoid requiring linux/mempolicy.h in all environments.
// These values are stable ABI since Linux 2.6.7.
#ifndef MPOL_PREFERRED
#define MPOL_PREFERRED 1
#endif // #ifndef MPOL_PREFERRED

#ifndef MPOL_MF_MOVE
#define MPOL_MF_MOVE (1 << 1)
#endif // #ifndef MPOL_MF_MOVE
#endif // #if defined(__linux__)

#include <tachyon.h>
#include <tachyon/arena.hpp>
#include <tachyon/shm.hpp>
#include <tachyon/transport.hpp>

using namespace tachyon::core;

struct alignas(64) tachyon_bus {
	SharedMemory		  shm;
	Arena				  arena;
	std::atomic<uint32_t> ref_count{1};
	std::atomic_flag	  producer_lock = ATOMIC_FLAG_INIT;
	std::atomic_flag	  consumer_lock = ATOMIC_FLAG_INIT;

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

static tachyon_error_t map_transport_error(const TransportError error) TACHYON_NOEXCEPT {
	switch (error) {
	case TransportError::ProtocolMismatch:
		return TACHYON_ERR_SYSTEM;
	case TransportError::Interrupted:
		return TACHYON_ERR_INTERRUPTED;
	default:
		return TACHYON_ERR_NETWORK;
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

	const TachyonHandshake handshake = {
		TACHYON_MAGIC,
		TACHYON_VERSION,
		static_cast<uint32_t>(capacity),
		static_cast<uint32_t>(required_shm_size),
		TACHYON_MSG_ALIGNMENT
	};

	if (auto transport_res = uds_export_shm(socket_path, bus->shm.get_fd(), handshake); !transport_res.has_value()) {
		delete bus;
		return map_transport_error(transport_res.error());
	}

	*out_bus = bus;
	return TACHYON_SUCCESS;
}

tachyon_error_t tachyon_bus_connect(const char *socket_path, tachyon_bus_t **out_bus) TACHYON_NOEXCEPT {
	if (!socket_path || !out_bus)
		return TACHYON_ERR_INVALID_SZ;

	auto transport_res = uds_import_shm(socket_path);
	if (!transport_res.has_value())
		return map_transport_error(transport_res.error());

	const auto &[received_fd, hs] = transport_res.value();
	if (hs.magic != TACHYON_MAGIC || hs.version != TACHYON_VERSION || hs.msg_alignment != TACHYON_MSG_ALIGNMENT) {
		close(received_fd);
		return TACHYON_ERR_SYSTEM;
	}

	auto shm_join = SharedMemory::join(received_fd, hs.shm_size);
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

tachyon_error_t tachyon_bus_set_numa_node(const tachyon_bus_t *bus, const int node_id) TACHYON_NOEXCEPT {
	if (!bus) [[unlikely]]
		return TACHYON_ERR_NULL_PTR;

#if defined(__linux__)
	if (node_id < 0) [[unlikely]]
		return TACHYON_ERR_INVALID_SZ;

	void  *ptr  = bus->shm.get_ptr();
	const size_t size = bus->shm.get_size();

	if (!ptr || size == 0) [[unlikely]]
		return TACHYON_ERR_NULL_PTR;

	if (node_id >= 64) [[unlikely]]
		return TACHYON_ERR_INVALID_SZ;

	const unsigned long nodeMask = 1UL << static_cast<unsigned int>(node_id);
	const unsigned long maxNode  = static_cast<unsigned long>(node_id) + 2UL;

	// MPOL_PREFERRED: allocate on the requested node when possible.
	// Falls back to other nodes rather than failing hard — production-safe.
	// MPOL_MF_MOVE: migrate pages already allocated by mmap(MAP_POPULATE).
	// Without this flag, pages allocated before this call would remain on
	// their original node, defeating the purpose entirely.
	const long ret = syscall(
		SYS_mbind,
		ptr,
		size,
		MPOL_PREFERRED,
		&nodeMask,
		maxNode,
		static_cast<unsigned long>(MPOL_MF_MOVE)
	);

	if (ret != 0) [[unlikely]]
		return TACHYON_ERR_SYSTEM;

	return TACHYON_SUCCESS;
#else  // #if defined(__linux__)
	// NUMA memory binding is a Linux-specific feature.
	// On macOS and other platforms, this is a no-op.
	(void)node_id;
	return TACHYON_SUCCESS;
#endif // #if defined(__linux__) #else
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

	uint32_t spins = 0;
	while (true) {
		while (bus->consumer_lock.test_and_set(std::memory_order_acquire))
			tachyon::cpu_relax();

		if (const std::byte *ptr = bus->arena.acquire_rx(*out_type_id, *out_actual_size)) {
			return ptr;
		}

		const auto state = bus->arena.get_state();
		bus->consumer_lock.clear(std::memory_order_release);
		if (state == BusState::FatalError)
			return nullptr;

		if (max_spins != 0 && spins >= max_spins)
			return nullptr;

		tachyon::cpu_relax();
		spins++;
	}
}

const void *tachyon_acquire_rx_blocking(
	tachyon_bus_t *bus, uint32_t *out_type_id, size_t *out_actual_size, const uint32_t spin_threshold
) TACHYON_NOEXCEPT {
	if (!bus || !out_type_id || !out_actual_size)
		return nullptr;

	uint32_t spins = 0;
	while (true) {
		while (bus->consumer_lock.test_and_set(std::memory_order_acquire))
			tachyon::cpu_relax();

		const std::byte *ptr = bus->arena.acquire_rx(*out_type_id, *out_actual_size);
		if (ptr) {
			return ptr;
		}

		const auto state = bus->arena.get_state();
		bus->consumer_lock.clear(std::memory_order_release);
		if (state == BusState::FatalError)
			return nullptr;

		if (spins < spin_threshold) {
			tachyon::cpu_relax();
			spins++;
		} else {
			while (bus->consumer_lock.test_and_set(std::memory_order_acquire))
				tachyon::cpu_relax();

			// Deliberate lost-wakeup window: ratched by WATCHDOG_TIMEOUT_US (200ms) retry.
			// Heartbeat-based dead-peer detection intentionally omitted — false positives on
			// idle producers. Proper detection deferred to Phase 8.3 (TSC watchdog).
			bus->arena.set_consumer_sleeping(true);
			ptr = bus->arena.acquire_rx(*out_type_id, *out_actual_size);
			if (ptr) {
				bus->arena.set_consumer_sleeping(false);
				return ptr;
			}

			bus->consumer_lock.clear(std::memory_order_release);

			const int wait_res = bus->arena.wait_consumer_sleeping();
			bus->arena.set_consumer_sleeping(false);
			if (wait_res == -1) // Interrupted
				return nullptr;

			spins = 0;
		}
	}
}

tachyon_error_t tachyon_commit_rx(tachyon_bus_t *bus) TACHYON_NOEXCEPT {
	if (!bus)
		return TACHYON_ERR_NULL_PTR;

	const bool success = bus->arena.commit_rx();
	bus->consumer_lock.clear(std::memory_order_release);

	return success ? TACHYON_SUCCESS : TACHYON_ERR_SYSTEM;
}

size_t
tachyon_acquire_rx_batch(tachyon_bus_t *bus, tachyon_msg_view_t *out_views, const size_t max_msgs) TACHYON_NOEXCEPT {
	if (!bus || !out_views || max_msgs == 0) [[unlikely]]
		return 0;

	while (bus->consumer_lock.test_and_set(std::memory_order_acquire)) {
		tachyon::cpu_relax();
	}

	auto		*cxx_views = reinterpret_cast<RxView *>(out_views);
	const size_t count	   = bus->arena.acquire_rx_batch(cxx_views, max_msgs);
	if (count == 0) [[unlikely]] {
		// DROP LOCK released immediately on empty to prevent consumer starvation
		bus->consumer_lock.clear(std::memory_order_release);
	}

	return count;
}

size_t tachyon_drain_batch(
	tachyon_bus_t *bus, tachyon_msg_view_t *out_views, const size_t max_msgs, const uint32_t spin_threshold
) TACHYON_NOEXCEPT {
	if (!bus || !out_views || max_msgs == 0)
		return 0;

	auto	*cxx_views = reinterpret_cast<RxView *>(out_views);
	uint32_t spins	   = 0;

	while (true) {
		while (bus->consumer_lock.test_and_set(std::memory_order_acquire))
			tachyon::cpu_relax();

		size_t count = bus->arena.acquire_rx_batch(cxx_views, max_msgs);
		if (count > 0) {
			return count;
		}

		const auto state = bus->arena.get_state();
		bus->consumer_lock.clear(std::memory_order_release);

		if (state == BusState::FatalError)
			return 0;

		if (spins < spin_threshold) {
			tachyon::cpu_relax();
			spins++;
		} else {
			while (bus->consumer_lock.test_and_set(std::memory_order_acquire))
				tachyon::cpu_relax();

			// Deliberate lost-wakeup window: ratched by WATCHDOG_TIMEOUT_US (200ms) retry.
			// Heartbeat-based dead-peer detection intentionally omitted — false positives on
			// idle producers. Proper detection deferred to Phase 8.3 (TSC watchdog).
			bus->arena.set_consumer_sleeping(true);
			count = bus->arena.acquire_rx_batch(cxx_views, max_msgs);
			if (count > 0) {
				bus->arena.set_consumer_sleeping(false);
				return count;
			}

			bus->consumer_lock.clear(std::memory_order_release);

			const int wait_res = bus->arena.wait_consumer_sleeping();
			bus->arena.set_consumer_sleeping(false);
			if (wait_res == -1) // Interrupted
				return 0;

			spins = 0;
		}
	}
}

tachyon_error_t
tachyon_commit_rx_batch(tachyon_bus_t *bus, const tachyon_msg_view_t *views, const size_t count) TACHYON_NOEXCEPT {
	if (!bus) [[unlikely]]
		return TACHYON_ERR_NULL_PTR;

	if (count == 0) [[unlikely]] {
		bus->consumer_lock.clear(std::memory_order_release);
		return TACHYON_SUCCESS;
	}

	const auto *cxx_views = reinterpret_cast<const RxView *>(views);
	const bool	success	  = bus->arena.commit_rx_batch(cxx_views, count);
	bus->consumer_lock.clear(std::memory_order_release);

	return success ? TACHYON_SUCCESS : TACHYON_ERR_SYSTEM;
}

void tachyon_flush(tachyon_bus_t *bus) TACHYON_NOEXCEPT {
	if (bus) {
		TasGuard p_lock(bus->producer_lock);
		bus->arena.flush_tx();
	}
}

tachyon_state_t tachyon_get_state(const tachyon_bus_t *bus) TACHYON_NOEXCEPT {
	if (!bus) [[unlikely]]
		return TACHYON_STATE_UNKNOWN;
	return static_cast<tachyon_state_t>(bus->arena.get_state());
}
} // extern "C"
