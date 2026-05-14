#pragma once

#include <tachyon/arena.hpp>
#include <tachyon/shm.hpp>

struct alignas(64) tachyon_bus {
	tachyon::core::SharedMemory shm;
	tachyon::core::Arena		arena;
	std::atomic<uint32_t>		ref_count{1};

	tachyon_bus(tachyon::core::SharedMemory &&s, tachyon::core::Arena &&a) : shm(std::move(s)), arena(std::move(a)) {}
};
