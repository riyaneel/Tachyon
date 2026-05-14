#pragma once

#include <tachyon/arena.hpp>

using namespace tachyon::core;

struct alignas(64) tachyon_bus {
	SharedMemory		  shm;
	Arena				  arena;
	std::atomic<uint32_t> ref_count{1};

	tachyon_bus(SharedMemory &&s, Arena &&a) : shm(std::move(s)), arena(std::move(a)) {}
};
