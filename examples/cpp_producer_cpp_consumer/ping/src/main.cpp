#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <x86intrin.h>

#include <tachyon.h>

static constexpr int  PING_CORE		 = 8;
static constexpr int  PONG_CORE		 = 9;
static constexpr bool USE_SCHED_FIFO = true;

static constexpr const char *SOCK_PA  = "/tmp/tachyon_pa.sock"; // ping listens
static constexpr const char *SOCK_AP  = "/tmp/tachyon_ap.sock"; // pong listens
static constexpr size_t		 CAPACITY = 1 << 16;

static constexpr size_t PAYLOAD	   = 32;
static constexpr size_t WARMUP	   = 10'000;
static constexpr size_t ITERATIONS = 1'000'000;

#define TACHYON_HOT [[gnu::always_inline]] [[gnu::hot]] inline
#define TACHYON_COLD [[gnu::noinline]] [[gnu::cold]]

TACHYON_COLD static double calibrate_tsc_ns_per_tick() noexcept {
	struct timespec ts0{}, ts1{};
	clock_gettime(CLOCK_MONOTONIC, &ts0);
	const uint64_t tsc0 = __rdtsc();

	struct timespec target = ts0;
	target.tv_nsec += 10'000'000;
	if (target.tv_nsec >= 1'000'000'000L) {
		target.tv_sec++;
		target.tv_nsec -= 1'000'000'000L;
	}
	do {
		clock_gettime(CLOCK_MONOTONIC, &ts1);
	} while (ts1.tv_sec < target.tv_sec || (ts1.tv_sec == target.tv_sec && ts1.tv_nsec < target.tv_nsec));

	const uint64_t tsc1 = __rdtsc();
	const double   wall_ns =
		static_cast<double>(ts1.tv_sec - ts0.tv_sec) * 1e9 + static_cast<double>(ts1.tv_nsec - ts0.tv_nsec);
	const double ticks = static_cast<double>(tsc1 - tsc0);
	return wall_ns / ticks;
}

TACHYON_COLD static void setup_thread(const int core) noexcept {
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(static_cast<size_t>(core), &cpuset);
	if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0)
		std::fprintf(stderr, "[ping] WARNING: failed to pin to core %d\n", core);
	else
		std::printf("[ping] Pinned to core %d\n", core);

	if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
		std::fprintf(stderr, "[ping] WARNING: mlockall failed (non-fatal)\n");

	if constexpr (USE_SCHED_FIFO) {
		struct sched_param sp{};
		sp.sched_priority = 99;
		if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
			std::fprintf(stderr, "[ping] WARNING: SCHED_FIFO failed — run with sudo or CAP_SYS_NICE\n");
		else
			std::printf("[ping] SCHED_FIFO priority 99\n");
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

TACHYON_HOT static void rtt_once(
	tachyon_bus_t *__restrict__ tx, tachyon_bus_t *__restrict__ rx, const std::byte *__restrict__ payload
) noexcept {
	void *ptr = tachyon_acquire_tx(tx, PAYLOAD);
	while (__builtin_expect(ptr == nullptr, 0))
		ptr = tachyon_acquire_tx(tx, PAYLOAD);
	std::memcpy(ptr, payload, PAYLOAD);
	tachyon_commit_tx(tx, PAYLOAD, 1);
	tachyon_flush(tx);

	uint32_t	type_id		= 0;
	size_t		actual_size = 0;
	const void *rptr		= tachyon_acquire_rx_spin(rx, &type_id, &actual_size, 0);
	if (__builtin_expect(rptr != nullptr, 1))
		tachyon_commit_rx(rx);
}

static double pct_ns(const std::vector<uint64_t> &ticks, const double p, const double ns_per_tick) noexcept {
	return static_cast<double>(ticks[static_cast<size_t>(static_cast<double>(ticks.size()) * p)]) * ns_per_tick;
}

int main() {
	setup_thread(PING_CORE);

	std::printf("[ping] Calibrating TSC ...\n");
	const double ns_per_tick = calibrate_tsc_ns_per_tick();
	std::printf("[ping] TSC: %.4f ns/tick  (%.3f GHz)\n\n", ns_per_tick, 1.0 / ns_per_tick);

	tachyon_bus_t	 *tx		= nullptr;
	std::atomic<bool> listen_ok = false;

	std::thread t_listen([&] {
		std::printf("[ping] Listening on %s (ping→pong) ...\n", SOCK_PA);
		if (tachyon_bus_listen(SOCK_PA, CAPACITY, &tx) != TACHYON_SUCCESS) {
			std::fprintf(stderr, "[ping] FATAL: listen failed on %s\n", SOCK_PA);
			return;
		}
		listen_ok.store(true, std::memory_order_release);
		std::printf("[ping] Pong connected on %s\n", SOCK_PA);
	});

	std::printf("[ping] Connecting to %s (retrying) ...\n", SOCK_AP);
	tachyon_bus_t *rx = wait_connect(SOCK_AP);
	t_listen.join();

	if (rx == nullptr || !listen_ok.load()) {
		std::fprintf(stderr, "[ping] FATAL: handshake failed. Is pong running?\n");
		if (rx)
			tachyon_bus_destroy(rx);
		if (tx)
			tachyon_bus_destroy(tx);
		return 1;
	}

	std::printf("[ping] Handshake complete. ping=core%d  pong=core%d\n\n", PING_CORE, PONG_CORE);

	alignas(64) std::byte payload[PAYLOAD]{};
	std::memset(payload, 0xAB, PAYLOAD);

	std::vector<uint64_t> ticks(ITERATIONS, 0ULL);
	std::ranges::fill(ticks, 0ULL);

	std::printf("[ping] Warming up  (%6zu iters) ...\n", WARMUP);
	for (size_t i = 0; i < WARMUP; ++i)
		rtt_once(tx, rx, payload);

	std::printf("[ping] Benchmarking (%6zu iters) ...\n\n", ITERATIONS);

	const uint64_t bench_start_tsc = __rdtsc();

	for (size_t i = 0; i < ITERATIONS; ++i) {
		const uint64_t t0 = __rdtsc();
		rtt_once(tx, rx, payload);
		const uint64_t t1 = __rdtsc();
		ticks[i]		  = t1 - t0;
	}

	const uint64_t bench_end_tsc = __rdtsc();
	const double   total_sec	 = static_cast<double>(bench_end_tsc - bench_start_tsc) * ns_per_tick / 1e9;

	void *sptr = tachyon_acquire_tx(tx, PAYLOAD);
	while (sptr == nullptr)
		sptr = tachyon_acquire_tx(tx, PAYLOAD);
	std::memset(sptr, 0, PAYLOAD);
	tachyon_commit_tx(tx, PAYLOAD, 0);
	tachyon_flush(tx);

	std::ranges::sort(ticks);

	double sum = 0.0;
	for (const auto t : ticks)
		sum += static_cast<double>(t) * ns_per_tick;
	const double mean_ns = sum / static_cast<double>(ITERATIONS);

	double var = 0.0;
	for (const auto t : ticks) {
		const double d = static_cast<double>(t) * ns_per_tick - mean_ns;
		var += d * d;
	}
	const double stddev_ns	  = std::sqrt(var / static_cast<double>(ITERATIONS));
	const double throughput_k = static_cast<double>(ITERATIONS) / total_sec / 1e3;

	auto ns = [&](double p) { return pct_ns(ticks, p, ns_per_tick); };

	std::cout << "┌─────────────────────────────────────────────────┐\n";
	std::cout << "│  Tachyon SHM — inter-process RTT benchmark      │\n";
	std::cout << "│  Payload: " << std::setw(4) << PAYLOAD << " bytes"
			  << "   Samples: " << std::setw(9) << ITERATIONS << "       │\n";
	std::cout << "│  Cores:   ping=" << std::setw(2) << PING_CORE << "  pong=" << std::setw(2) << PONG_CORE
			  << "   rdtsc / spin-only  │\n";
	std::cout << "├──────────────────────────────────┬──────────────┤\n";
	std::cout << "│  Metric                          │     RTT (ns) │\n";
	std::cout << "├──────────────────────────────────┼──────────────┤\n";

	auto row = [](const std::string &label, double val) {
		std::cout << "│  " << std::left << std::setw(33) << label << "│ " << std::right << std::fixed
				  << std::setprecision(1) << std::setw(11) << val << " │\n";
	};

	row("Min", static_cast<double>(ticks.front()) * ns_per_tick);
	row("p50  (median)", ns(0.50));
	row("p90", ns(0.90));
	row("p99", ns(0.99));
	row("p99.9", ns(0.999));
	row("p99.99", ns(0.9999));
	row("Max", static_cast<double>(ticks.back()) * ns_per_tick);

	std::cout << "├──────────────────────────────────┼──────────────┤\n";
	row("Mean", mean_ns);
	row("Std dev", stddev_ns);
	std::cout << "├──────────────────────────────────┼──────────────┤\n";
	row("One-way p50 estimate", ns(0.50) / 2.0);

	row("Throughput (K RTT/s)", throughput_k);
	std::cout << "└──────────────────────────────────┴──────────────┘\n\n";

	tachyon_bus_destroy(tx);
	tachyon_bus_destroy(rx);
	return 0;
}
