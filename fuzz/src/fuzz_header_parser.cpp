#include <cstdint>
#include <cstring>
#include <span>

#include <tachyon/arena.hpp>

using namespace tachyon::core;

static constexpr size_t ARENA_CAPACITY = 4096;
static constexpr size_t BUF_SIZE	   = sizeof(MemoryLayout) + ARENA_CAPACITY;

alignas(128) static std::byte g_buf[BUF_SIZE];

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, const size_t size) {
	if (size == 0)
		return 0;

	std::memset(g_buf, 0, BUF_SIZE);

	auto *layout				 = new (g_buf) MemoryLayout{};
	layout->header.magic		 = TACHYON_MAGIC;
	layout->header.version		 = TACHYON_VERSION;
	layout->header.capacity		 = static_cast<uint32_t>(ARENA_CAPACITY);
	layout->header.msg_alignment = TACHYON_MSG_ALIGNMENT;
	layout->header.state.store(BusState::Ready, std::memory_order_relaxed);

	layout->indices.head.store(ARENA_CAPACITY, std::memory_order_relaxed);
	layout->indices.tail.store(0, std::memory_order_relaxed);
	layout->indices.consumer_sleeping.store(0, std::memory_order_relaxed);
	layout->indices.producer_heartbeat.store(0, std::memory_order_relaxed);
	layout->indices.consumer_heartbeat.store(0, std::memory_order_relaxed);

	std::byte *arena_region = layout->data_arena();
	std::memcpy(arena_region, data, std::min(size, ARENA_CAPACITY));

	const std::span buf_span(g_buf, BUF_SIZE);
	auto			arena_res = Arena::attach(buf_span);
	if (!arena_res.has_value())
		return 0;
	Arena &consumer = arena_res.value();

	uint32_t type_id	 = 0;
	size_t	 actual_size = 0;

	if (const std::byte *ptr = consumer.acquire_rx(type_id, actual_size); ptr != nullptr) {
		if (actual_size > 0) {
			const volatile uint8_t sink = static_cast<uint8_t>(*ptr);
			(void)sink;
		}

		(void)consumer.commit_rx();
	}

	return 0;
}
