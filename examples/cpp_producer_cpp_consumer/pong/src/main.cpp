#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

#include <tachyon.h>

static constexpr int  PONG_CORE		 = 9;
static constexpr bool USE_SCHED_FIFO = true;

static constexpr const char *SOCK_PA  = "/tmp/tachyon_pa.sock"; // ping listens
static constexpr const char *SOCK_AP  = "/tmp/tachyon_ap.sock"; // pong listens
static constexpr size_t		 CAPACITY = 1 << 16;
static constexpr size_t		 PAYLOAD  = 32;

#define TACHYON_HOT [[gnu::always_inline]] [[gnu::hot]] inline
#define TACHYON_COLD [[gnu::noinline]] [[gnu::cold]]

TACHYON_COLD static void setup_thread(const int core) noexcept {
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(static_cast<size_t>(core), &cpuset);
	if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0)
		std::fprintf(stderr, "[pong] WARNING: failed to pin to core %d\n", core);
	else
		std::printf("[pong] Pinned to core %d\n", core);

	if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
		std::fprintf(stderr, "[pong] WARNING: mlockall failed (non-fatal)\n");

	if constexpr (USE_SCHED_FIFO) {
		struct sched_param sp{};
		sp.sched_priority = 99;
		if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
			std::fprintf(stderr, "[pong] WARNING: SCHED_FIFO failed — run with sudo or CAP_SYS_NICE\n");
		else
			std::printf("[pong] SCHED_FIFO priority 99\n");
	}
}

TACHYON_COLD static tachyon_bus_t *wait_connect(const char *path) noexcept {
	tachyon_bus_t *bus = nullptr;
	for (int i = 0; i < 400; ++i) {
		if (tachyon_bus_connect(path, &bus) == TACHYON_SUCCESS)
			return bus;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	return nullptr;
}

TACHYON_HOT static bool reflect_once(tachyon_bus_t *__restrict__ rx, tachyon_bus_t *__restrict__ tx) noexcept {
	alignas(64) std::byte buf[PAYLOAD];

	uint32_t type_id	 = 0;
	size_t	 actual_size = 0;

	const void *src = tachyon_acquire_rx_spin(rx, &type_id, &actual_size, 0);
	if (__builtin_expect(src == nullptr, 0))
		return false;

	std::memcpy(buf, src, actual_size);
	tachyon_commit_rx(rx);

	if (__builtin_expect(type_id == 0, 0))
		return false; // sentinel

	void *dst = tachyon_acquire_tx(tx, actual_size);
	while (__builtin_expect(dst == nullptr, 0))
		dst = tachyon_acquire_tx(tx, actual_size);

	std::memcpy(dst, buf, actual_size);
	tachyon_commit_tx(tx, actual_size, type_id);
	tachyon_flush(tx);

	return true;
}

int main() {
	setup_thread(PONG_CORE);

	tachyon_bus_t	 *tx		= nullptr;
	std::atomic<bool> listen_ok = false;

	std::thread t_listen([&] {
		std::printf("[pong] Listening on %s (pong→ping) ...\n", SOCK_AP);
		if (tachyon_bus_listen(SOCK_AP, CAPACITY, &tx) != TACHYON_SUCCESS) {
			std::fprintf(stderr, "[pong] FATAL: listen failed on %s\n", SOCK_AP);
			return;
		}
		listen_ok.store(true, std::memory_order_release);
		std::printf("[pong] Ping connected on %s\n", SOCK_AP);
	});

	std::printf("[pong] Connecting to %s (ping→pong, retrying) ...\n", SOCK_PA);
	tachyon_bus_t *rx = wait_connect(SOCK_PA);

	t_listen.join();

	if (rx == nullptr || !listen_ok.load()) {
		std::fprintf(stderr, "[pong] FATAL: handshake failed. Is ping running?\n");
		if (rx)
			tachyon_bus_destroy(rx);
		if (tx)
			tachyon_bus_destroy(tx);
		return 1;
	}

	tachyon_bus_set_polling_mode(rx, 1);
	tachyon_bus_set_polling_mode(tx, 1);

	std::printf("[pong] Handshake complete. Spinning on core %d ...\n\n", PONG_CORE);

	while (reflect_once(rx, tx)) {
	}

	std::printf("[pong] Sentinel received. Exiting.\n");
	tachyon_bus_destroy(rx);
	tachyon_bus_destroy(tx);
	return 0;
}
