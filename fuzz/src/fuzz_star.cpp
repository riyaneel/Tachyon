#include <algorithm>
#include <atomic>
#include <cstring>
#include <span>
#include <string>

#include <tachyon.h>
#include <tachyon/arena.hpp>
#include <tachyon/shm.hpp>
#include <tachyon/star.hpp>

using namespace tachyon::core;

static constexpr size_t MAX_SPOKES		   = 6;
static constexpr size_t ARENA_CAPACITY	   = 4096;
static constexpr size_t BUF_SIZE		   = sizeof(MemoryLayout) + ARENA_CAPACITY;
static constexpr size_t MAX_VIEWS		   = 64;
static constexpr size_t MAX_MSGS_PER_SPOKE = 8;
static constexpr size_t MAX_MSG_PAYLOAD	   = 64;

static SharedMemory *g_shm[MAX_SPOKES];
static int			 g_fds[MAX_SPOKES];
alignas(32) static tachyon_msg_view_t g_views[MAX_VIEWS];
static size_t g_spoke_indices[MAX_VIEWS];

struct Reader {
	const uint8_t *ptr;
	const uint8_t *end;

	uint8_t u8(const uint8_t def = 0) noexcept {
		return (ptr < end) ? *ptr++ : def;
	}

	uint32_t u32(const uint32_t def = 0) noexcept {
		uint32_t	 v = def;
		const size_t n = std::min(sizeof(uint32_t), static_cast<size_t>(end - ptr));
		std::memcpy(&v, ptr, n);
		ptr += n;
		return v;
	}

	uint64_t u64(const uint64_t def = 0) noexcept {
		uint64_t	 v = def;
		const size_t n = std::min(sizeof(uint64_t), static_cast<size_t>(end - ptr));
		std::memcpy(&v, ptr, n);
		ptr += n;
		return v;
	}

	const uint8_t *advance(size_t &n) noexcept {
		const size_t available = static_cast<size_t>(end - ptr);
		n					   = std::min(n, available);
		const uint8_t *cur	   = ptr;
		ptr += n;
		return cur;
	}
};

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
	for (size_t i = 0; i < MAX_SPOKES; ++i) {
		auto res = SharedMemory::create("tachyon_fuzz_star_" + std::to_string(i), BUF_SIZE);
		if (!res.has_value()) {
			__builtin_trap();
		}
		g_shm[i] = new (std::nothrow) SharedMemory(std::move(*res));
		g_fds[i] = g_shm[i]->get_fd();
	}

	return 0;
}

static const std::byte *arena_base(const size_t i) noexcept {
	return g_shm[i]->data().data() + sizeof(MemoryLayout);
}

static const std::byte *arena_end(const size_t i) noexcept {
	return arena_base(i) + ARENA_CAPACITY;
}

static bool setup_spoke(const size_t i, Reader &reader, tachyon_bus **out) noexcept {
	auto prod_res = Arena::format(g_shm[i]->data(), ARENA_CAPACITY);
	if (!prod_res.has_value()) {
		return false;
	}
	Arena producer = std::move(*prod_res);

	const size_t n_msgs = reader.u8() % (MAX_MSGS_PER_SPOKE + 1);
	for (size_t m = 0; m < n_msgs; ++m) {
		size_t		   payload_size = reader.u8() % (MAX_MSG_PAYLOAD + 1);
		const uint32_t type_id		= reader.u32();
		const uint8_t *payload		= reader.advance(payload_size);

		std::byte *ptr = producer.acquire_tx(payload_size);
		if (!ptr) {
			break;
		}
		if (payload_size > 0) {
			std::memcpy(ptr, payload, payload_size);
		}

		(void)producer.commit_tx(payload_size, type_id);
	}
	producer.flush_tx();

	auto cons_res = Arena::attach(g_shm[i]->data());
	if (!cons_res.has_value()) {
		return false;
	}

	auto join_res = SharedMemory::join(g_fds[i], BUF_SIZE);
	if (!join_res.has_value()) {
		return false;
	}

	*out = new (std::nothrow) tachyon_bus(std::move(*join_res), std::move(*cons_res));
	return true;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, const size_t size) {
	if (size < 3) {
		return 0;
	}

	Reader		   reader{data, data + size};
	const size_t   n_spokes	 = (reader.u8() % MAX_SPOKES) + 1;
	const size_t   max_total = (reader.u8() % MAX_VIEWS) + 1;
	const uint64_t budget_us = reader.u64(200);

	tachyon_bus	  *buses[MAX_SPOKES]{};
	tachyon_bus_t *bus_ptrs[MAX_SPOKES]{};
	size_t		   n_ready = 0;
	for (size_t i = 0; i < n_spokes; ++i) {
		if (!setup_spoke(i, reader, &buses[i]))
			break;
		bus_ptrs[i] = buses[i];
		++n_ready;
	}

	if (n_ready > 0) {
		if (auto star_res = StarBus::create(bus_ptrs, n_ready, nullptr); star_res.has_value()) {
			StarBus &star = *star_res;

			const size_t received = star.poll(g_views, max_total, budget_us, g_spoke_indices);
			if (received > max_total) {
				__builtin_trap();
			}

			for (size_t i = 0; i < received; ++i) {
				if (g_spoke_indices[i] >= n_ready) {
					__builtin_trap();
				}
			}

			for (size_t i = 0; i < received; ++i) {
				const void *ptr = g_views[i].ptr;
				if (ptr == nullptr) {
					continue;
				}

				const size_t spoke = g_spoke_indices[i];
				const auto	*base  = arena_base(spoke);
				const auto	*aend  = arena_end(spoke);
				const auto	*cptr  = static_cast<const std::byte *>(ptr);
				if (cptr < base || cptr >= aend) {
					__builtin_trap();
				}

				if (g_views[i].actual_size > static_cast<size_t>(aend - cptr)) {
					__builtin_trap();
				}

				if (g_views[i].actual_size > 0) {
					const volatile uint8_t sink = static_cast<uint8_t>(*cptr);
					(void)sink;
				}
			}

			if (!star.commit()) {
				__builtin_trap();
			}

			const uint8_t  tx_spoke_raw = reader.u8();
			const uint8_t  tx_action	= reader.u8();
			const size_t   tx_size		= reader.u8() % (MAX_MSG_PAYLOAD + 1);
			const uint32_t tx_type_id	= reader.u32();
			if (tx_action < 2) {
				if (const void *ptr = star.acquire_tx(tx_spoke_raw, tx_size)) {
					const size_t spoke = tx_spoke_raw;
					const auto	*base  = arena_base(spoke);
					const auto	*aend  = arena_end(spoke);
					if (const auto *cptr = static_cast<const std::byte *>(ptr); cptr < base || cptr >= aend) {
						__builtin_trap();
					}

					if (tx_action == 0) {
						(void)star.commit_tx(tx_spoke_raw, tx_size, tx_type_id);
					} else {
						(void)star.rollback_tx(tx_spoke_raw);
					}
				}
			}
		}
	}

	for (size_t i = 0; i < n_ready; ++i) {
		tachyon_bus_destroy(buses[i]);
	}

	return 0;
}
