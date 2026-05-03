#include <cstdint>
#include <span>
#include <vector>

#include <tachyon/arena.hpp>

using namespace tachyon::core;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	std::vector		buffer(reinterpret_cast<const std::byte *>(data), reinterpret_cast<const std::byte *>(data) + size);
	const std::span shm_span(buffer);

	if (auto arena_res = Arena::attach(shm_span); arena_res.has_value()) {
		Arena	   &arena  = arena_res.value();
		const auto *layout = reinterpret_cast<MemoryLayout *>(buffer.data());
		if (const size_t capacity = layout->header.capacity; size < sizeof(MemoryLayout) + capacity) {
			__builtin_trap();
		}

		uint32_t type_id	 = 0;
		size_t	 actual_size = 0;
		(void)arena.acquire_rx(type_id, actual_size);
	}

	return 0;
}
