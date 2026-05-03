#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>

#include <tachyon/arena.hpp>

using namespace tachyon::core;

static constexpr size_t ARENA_CAPACITY = 4096;
static constexpr size_t BUFFER_SIZE	   = sizeof(MemoryLayout) + ARENA_CAPACITY;

alignas(128) static std::byte g_buf[BUFFER_SIZE];

struct FuzzCmd {
	uint8_t	 tail_mutation;
	uint64_t tail_val;
	uint32_t max_size;
	uint32_t actual_size;
	uint32_t type_id;
	uint8_t	 action;
} __attribute__((packed));

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, const size_t size) {
	if (size < sizeof(FuzzCmd)) {
		return 0;
	}

	std::memset(g_buf, 0, BUFFER_SIZE);
	constexpr std::span buf_span(g_buf, BUFFER_SIZE);

	auto arena_res = Arena::format(buf_span, ARENA_CAPACITY);
	if (!arena_res.has_value()) {
		return 0;
	}

	Arena &producer = arena_res.value();
	auto  *layout	= reinterpret_cast<MemoryLayout *>(g_buf);

	const std::byte *arena_start = layout->data_arena();
	const std::byte *arena_end	 = arena_start + ARENA_CAPACITY;

	const size_t   num_cmds = size / sizeof(FuzzCmd);
	const FuzzCmd *cmds		= reinterpret_cast<const FuzzCmd *>(data);

	for (size_t i = 0; i < std::min(num_cmds, static_cast<size_t>(1024)); ++i) {
		const FuzzCmd &cmd			= cmds[i];
		const uint64_t current_head = layout->indices.head.load(std::memory_order_relaxed);

		switch (cmd.tail_mutation % 4) {
		case 0:
			layout->indices.tail.store(current_head + cmd.tail_val + 1, std::memory_order_relaxed);
			break;
		case 1:
			layout->indices.tail.store(UINT64_MAX, std::memory_order_relaxed);
			break;
		case 2:
			layout->indices.tail.store(cmd.tail_val | 1ULL, std::memory_order_relaxed);
			break;
		case 3:
			layout->indices.tail.store(cmd.tail_val, std::memory_order_relaxed);
			break;
		}

		if (std::byte *ptr = producer.acquire_tx(cmd.max_size); ptr != nullptr) {
			if (ptr < arena_start || ptr + cmd.max_size > arena_end) {
				__builtin_trap();
			}

			if (cmd.max_size > 0) {
				ptr[0]				  = std::byte{0xAA};
				ptr[cmd.max_size - 1] = std::byte{0xBB};
			}

			if (cmd.action % 2 == 0) {
				const size_t actual = std::min(static_cast<size_t>(cmd.actual_size), static_cast<size_t>(cmd.max_size));
				if (!producer.commit_tx(actual, cmd.type_id)) {
					__builtin_trap();
				}
			} else {
				if (!producer.rollback_tx()) {
					__builtin_trap();
				}
			}
		}
	}

	return 0;
}
