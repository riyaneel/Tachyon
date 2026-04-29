#include <atomic>

#include <tachyon.h>
#include <tachyon/arena.hpp>
#include <tachyon/shm.hpp>
#include <tachyon/transport.hpp>

using namespace tachyon::core;

struct alignas(64) tachyon_rpc_bus {
	SharedMemory		  shm_fwd;
	Arena				  arena_fwd; /* caller -> callee direction (caller writes, callee reads) */
	SharedMemory		  shm_rev;
	Arena				  arena_rev; /* callee -> caller direction (callee writes, caller reads) */
	std::atomic<uint64_t> correlation_counter{1}; /* 0 for reserved sentinel */
	std::atomic<uint32_t> ref_count{1};

	tachyon_rpc_bus(SharedMemory &&s_fwd, Arena &&a_fwd, SharedMemory &&s_rev, Arena &&a_rev)
		: shm_fwd(std::move(s_fwd)), arena_fwd(std::move(a_fwd)), shm_rev(std::move(s_rev)),
		  arena_rev(std::move(a_rev)) {}
};
