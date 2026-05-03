#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>

#include <tachyon/arena.hpp>

using namespace tachyon::core;

static constexpr size_t ARENA_CAPACITY = 4096;
static constexpr size_t BUFFER_SIZE	   = sizeof(MemoryLayout) + ARENA_CAPACITY;

alignas(128) static std::byte g_buf_tx[BUFFER_SIZE];
alignas(128) static std::byte g_buf_rx[BUFFER_SIZE];

struct RpcFuzzCmd {
	uint32_t max_size;
	uint32_t actual_size;
	uint32_t type_id;
	uint64_t correlation_id;
	uint8_t	 force_corr_id_case;
} __attribute__((packed));

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, const size_t size) {
	if (size < sizeof(RpcFuzzCmd)) {
		return 0;
	}

	const RpcFuzzCmd *cmd	  = reinterpret_cast<const RpcFuzzCmd *>(data);
	const uint8_t	 *rx_data = data + sizeof(RpcFuzzCmd);
	const size_t	  rx_size = size - sizeof(RpcFuzzCmd);

	{
		std::memset(g_buf_tx, 0, BUFFER_SIZE);
		if (auto arena_res = Arena::format(std::span(g_buf_tx, BUFFER_SIZE), ARENA_CAPACITY); arena_res.has_value()) {
			Arena &producer = arena_res.value();

			if (const std::byte *ptr = producer.acquire_tx(cmd->max_size); ptr != nullptr) {
				const size_t actual =
					std::min(static_cast<size_t>(cmd->actual_size), static_cast<size_t>(cmd->max_size));

				uint64_t corr_id = cmd->correlation_id;
				if (cmd->force_corr_id_case % 3 == 1) {
					corr_id = 0;
				} else if (cmd->force_corr_id_case % 3 == 2) {
					corr_id = UINT64_MAX;
				}

				if (corr_id != 0) {
					if (!producer.commit_tx_rpc(actual, cmd->type_id, corr_id)) {
						__builtin_trap();
					}
				} else {
					(void)producer.commit_tx_rpc(actual, cmd->type_id, corr_id);
				}
			}
		}
	}

	{
		std::memset(g_buf_rx, 0, BUFFER_SIZE);
		auto *layout				 = new (g_buf_rx) MemoryLayout{};
		layout->header.magic		 = TACHYON_MAGIC;
		layout->header.version		 = TACHYON_VERSION;
		layout->header.capacity		 = static_cast<uint32_t>(ARENA_CAPACITY);
		layout->header.msg_alignment = TACHYON_MSG_ALIGNMENT;
		layout->header.state.store(BusState::Ready, std::memory_order_relaxed);

		layout->indices.head.store(ARENA_CAPACITY, std::memory_order_relaxed);
		layout->indices.tail.store(0, std::memory_order_relaxed);

		std::byte *arena_region = layout->data_arena();
		std::memcpy(arena_region, rx_data, std::min(rx_size, ARENA_CAPACITY));

		Arena consumer_rpc = Arena::attach(std::span(g_buf_rx, BUFFER_SIZE)).value();
		Arena consumer_std = Arena::attach(std::span(g_buf_rx, BUFFER_SIZE)).value();

		uint32_t		 type_id_rpc	 = 0;
		size_t			 actual_size_rpc = 0;
		uint64_t		 corr_id		 = 0;
		const std::byte *ptr_rpc		 = consumer_rpc.acquire_rx_rpc(type_id_rpc, actual_size_rpc, corr_id);
		const BusState	 state_rpc		 = consumer_rpc.get_state();

		uint32_t		 type_id_std	 = 0;
		size_t			 actual_size_std = 0;
		const std::byte *ptr_std		 = consumer_std.acquire_rx(type_id_std, actual_size_std);
		const BusState	 state_std		 = consumer_std.get_state();

		if ((ptr_rpc == nullptr) != (ptr_std == nullptr)) {
			__builtin_trap();
		}

		if (state_rpc != state_std) {
			__builtin_trap();
		}

		if (ptr_rpc != nullptr && ptr_std != nullptr) {
			if (ptr_rpc != ptr_std || type_id_rpc != type_id_std || actual_size_rpc != actual_size_std) {
				__builtin_trap();
			}

			(void)consumer_rpc.commit_rx();
			(void)consumer_std.commit_rx();
		}
	}

	return 0;
}
