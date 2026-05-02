#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

#include "../../protocol.hpp"
#include <tachyon.h>

static constexpr int		 FEED_CORE		= 8;
static constexpr bool		 USE_SCHED_FIFO = true;
static constexpr const char *SOCK			= "/tmp/tachyon_ob.sock";
static constexpr size_t		 CAP_FWD		= 1 << 16;
static constexpr size_t		 CAP_REV		= 1 << 16;

#define TACHYON_HOT [[gnu::always_inline]] [[gnu::hot]] inline
#define TACHYON_COLD [[gnu::noinline]] [[gnu::cold]]

static std::atomic<uint64_t> g_seq{0};

TACHYON_HOT static void fill_snapshot(BookSnapshot *snap, const uint64_t iid) noexcept {
	const uint64_t seq	= g_seq.fetch_add(1, std::memory_order_relaxed);
	snap->instrument_id = iid;
	snap->seq			= seq;
	const double base	= 100.0 + static_cast<double>(iid % 1000);
	for (int i = 0; i < 5; ++i) {
		snap->bids[i] = {base - (i + 1) * 0.01, 10.0 + i};
		snap->asks[i] = {base + (i + 1) * 0.01, 10.0 + i};
	}
}

TACHYON_COLD static void setup_thread(int core) noexcept {
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(static_cast<size_t>(core), &cpuset);
	if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0)
		std::fprintf(stderr, "[feed] WARNING: failed to pin to core %d\n", core);
	else
		std::printf("[feed] Pinned to core %d\n", core);

	if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
		std::fprintf(stderr, "[feed] WARNING: mlockall failed (non-fatal)\n");

	if constexpr (USE_SCHED_FIFO) {
		struct sched_param sp{};
		sp.sched_priority = 99;
		if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
			std::fprintf(stderr, "[feed] WARNING: SCHED_FIFO failed\n");
		else
			std::printf("[feed] SCHED_FIFO priority 99\n");
	}
}

int main() {
	setup_thread(FEED_CORE);

	tachyon_rpc_bus_t *rpc = nullptr;
	std::printf("[feed] Listening on %s ...\n", SOCK);
	if (tachyon_rpc_listen(SOCK, CAP_FWD, CAP_REV, &rpc) != TACHYON_SUCCESS) {
		std::fprintf(stderr, "[feed] FATAL: rpc_listen failed\n");
		return 1;
	}
	std::printf("[feed] Pricer connected. Serving book snapshots...\n\n");
	tachyon_rpc_set_polling_mode(rpc, 1);

	for (;;) {
		uint64_t cid      = 0;
		uint32_t msg_type = 0;
		size_t   sz       = 0;

		const void *req = tachyon_rpc_serve(rpc, &cid, &msg_type, &sz, UINT32_MAX);
		if (__builtin_expect(req == nullptr, 0))
			break; // EINTR

		if (__builtin_expect(msg_type == MSG_SENTINEL, 0)) {
			tachyon_rpc_commit_serve(rpc);
			break;
		}

		BookRequest breq{};
		std::memcpy(&breq, req, sizeof(BookRequest));
		tachyon_rpc_commit_serve(rpc);

		auto *slot = static_cast<BookSnapshot *>(tachyon_rpc_acquire_reply_tx(rpc, sizeof(BookSnapshot)));
		while (__builtin_expect(slot == nullptr, 0))
			slot = static_cast<BookSnapshot *>(tachyon_rpc_acquire_reply_tx(rpc, sizeof(BookSnapshot)));

		fill_snapshot(slot, breq.instrument_id);
		tachyon_rpc_commit_reply(rpc, cid, sizeof(BookSnapshot), MSG_BOOK_SNAPSHOT);
	}

	std::printf("[feed] Sentinel received. Exiting.\n");
	tachyon_rpc_destroy(rpc);
	return 0;
}
