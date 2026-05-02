#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <x86intrin.h>

#include "../../protocol.hpp"
#include <tachyon.h>

static constexpr int		 FEED_CORE		= 8;
static constexpr int		 PRICER_CORE	= 9;
static constexpr bool		 USE_SCHED_FIFO = true;
static constexpr const char *SOCK			= "/tmp/tachyon_ob.sock";
static constexpr size_t		 WARMUP			= 10'000;
static constexpr size_t		 ITERATIONS		= 1'000'000;

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
	return wall_ns / static_cast<double>(tsc1 - tsc0);
}

TACHYON_COLD static void setup_thread(int core) noexcept {
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(static_cast<size_t>(core), &cpuset);
	if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0)
		std::fprintf(stderr, "[pricer] WARNING: failed to pin to core %d\n", core);
	else
		std::printf("[pricer] Pinned to core %d\n", core);

	if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
		std::fprintf(stderr, "[pricer] WARNING: mlockall failed (non-fatal)\n");

	if constexpr (USE_SCHED_FIFO) {
		struct sched_param sp{};
		sp.sched_priority = 99;
		if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
			std::fprintf(stderr, "[pricer] WARNING: SCHED_FIFO failed\n");
		else
			std::printf("[pricer] SCHED_FIFO priority 99\n");
	}
}

TACHYON_COLD static tachyon_rpc_bus_t *connect_retry(const char *path) noexcept {
	tachyon_rpc_bus_t *rpc = nullptr;
	for (int i = 0; i < 400; ++i) {
		if (tachyon_rpc_connect(path, &rpc) == TACHYON_SUCCESS)
			return rpc;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	return nullptr;
}

TACHYON_HOT static void request_once(tachyon_rpc_bus_t *rpc, const uint64_t iid) noexcept {
	const BookRequest req{iid};
	uint64_t		  cid = 0;
	tachyon_rpc_call(rpc, &req, sizeof(req), MSG_BOOK_REQUEST, &cid);

	size_t		sz	 = 0;
	uint32_t	mt	 = 0;
	const void *snap = tachyon_rpc_wait(rpc, cid, &sz, &mt, UINT32_MAX);
	if (__builtin_expect(snap != nullptr, 1))
		tachyon_rpc_commit_rx(rpc);
}

static double pct_ns(const std::vector<uint64_t> &t, const double p, const double ns_per_tick) noexcept {
	return static_cast<double>(t[static_cast<size_t>(static_cast<double>(t.size()) * p)]) * ns_per_tick;
}

int main() {
	setup_thread(PRICER_CORE);

	std::printf("[pricer] Calibrating TSC...\n");
	const double ns_per_tick = calibrate_tsc_ns_per_tick();
	std::printf("[pricer] TSC: %.4f ns/tick  (%.3f GHz)\n\n", ns_per_tick, 1.0 / ns_per_tick);

	std::printf("[pricer] Connecting to feed on %s (retrying)...\n", SOCK);
	tachyon_rpc_bus_t *rpc = connect_retry(SOCK);
	if (!rpc) {
		std::fprintf(stderr, "[pricer] FATAL: could not connect. Is feed running?\n");
		return 1;
	}
	std::printf("[pricer] Connected. feed=core%d  pricer=core%d\n\n", FEED_CORE, PRICER_CORE);
	tachyon_rpc_set_polling_mode(rpc, 1);

	std::vector<uint64_t> ticks(ITERATIONS, 0ULL);

	std::printf("[pricer] Warming up  (%6zu iters)...\n", WARMUP);
	for (size_t i = 0; i < WARMUP; ++i)
		request_once(rpc, i % 16);

	std::printf("[pricer] Benchmarking (%6zu iters)...\n\n", ITERATIONS);
	const uint64_t bench_start = __rdtsc();

	for (size_t i = 0; i < ITERATIONS; ++i) {
		const uint64_t t0 = __rdtsc();
		request_once(rpc, i % 16);
		ticks[i] = __rdtsc() - t0;
	}

	const uint64_t bench_end = __rdtsc();
	const double   total_sec = static_cast<double>(bench_end - bench_start) * ns_per_tick / 1e9;

	const BookRequest sentinel{0};
	uint64_t		  cid = 0;
	tachyon_rpc_call(rpc, &sentinel, sizeof(sentinel), MSG_SENTINEL, &cid);

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
	std::cout << "│  Tachyon RPC — Order Book snapshot RTT          │\n";
	std::cout << "│  Payload: " << std::setw(4) << sizeof(BookSnapshot) << " bytes"
			  << "   Samples: " << std::setw(9) << ITERATIONS << "       │\n";
	std::cout << "│  Cores:  feed=" << std::setw(2) << FEED_CORE << "  pricer=" << std::setw(2) << PRICER_CORE
			  << "   rdtsc / spin-only │\n";
	std::cout << "├──────────────────────────────────┬──────────────┤\n";
	std::cout << "│  Metric                          │     RTT (ns) │\n";
	std::cout << "├──────────────────────────────────┼──────────────┤\n";

	auto row = [](const std::string &label, double val) {
		std::cout << "│  " << std::left << std::setw(32) << label << "│ " << std::right << std::fixed
				  << std::setprecision(1) << std::setw(12) << val << " │\n";
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

	tachyon_rpc_destroy(rpc);
	return 0;
}
