#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <tachyon.h>
#include <tachyon.hpp>

static constexpr size_t	  CAPACITY		 = 1 << 20; // 1 MB
static constexpr size_t	  PAYLOAD		 = 32;
static constexpr size_t	  WARMUP		 = 10'000;
static constexpr int	  SCHED_PRIO	 = 99;
static constexpr uint32_t SPIN_THRESHOLD = 50'000;

static const char *SOCK_PA = "/tmp/tachyon_inter_bench_pa.sock";
static const char *SOCK_AP = "/tmp/tachyon_inter_bench_ap.sock";

static double pct(const std::vector<int64_t> &sorted, const double p) {
	if (sorted.empty())
		return 0.0;

	const auto idx = static_cast<size_t>(static_cast<double>(sorted.size()) * p);
	return static_cast<double>(sorted[std::min(idx, sorted.size() - 1)]);
}

static tachyon_bus_t *connect_with_retry(const char *path) {
	constexpr int  retries = 400;
	tachyon_bus_t *bus	   = nullptr;
	for (int i = 0; i < retries; ++i) {
		if (tachyon_bus_connect(path, &bus) == TACHYON_SUCCESS)
			return bus;

		usleep(5'000);
	}

	return nullptr;
}

static void pin_to_core(const int core_id) noexcept {
	if (core_id < 0)
		return;

#if defined(__linux__)
	cpu_set_t cs;
	CPU_ZERO(&cs);
	CPU_SET(static_cast<size_t>(core_id), &cs);
	if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cs) != 0) {
		std::fprintf(stderr, "[bench_inter] WARNING: failed to pin to core %d\n", core_id);
	}
#endif // #if defined(__linux__)
}

static void try_set_sched_fifo() noexcept {
	struct sched_param sp{};
	sp.sched_priority = SCHED_PRIO;
	if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
		std::fprintf(stderr, "[bench] WARNING: SCHED_FIFO failed - run with sudo\n");
	}
}

static void try_mlockall() noexcept {
	if (mlockall(MCL_CURRENT) != 0) {
		std::fprintf(stderr, "[bench] WARNING: mlockall failed (non-fatal)\n");
	}
}

static const void *rx_blocking(tachyon_bus_t *bus, uint32_t *tid, size_t *sz) noexcept {
	const void *ptr = nullptr;
	while (!((ptr = tachyon_acquire_rx_blocking(bus, tid, sz, SPIN_THRESHOLD)))) {
		if (tachyon_get_state(bus) == TACHYON_STATE_FATAL_ERROR) {
			std::fprintf(stderr, "[bench_inter] FATAL: bus error state\n");
			return nullptr;
		}
	}

	return ptr;
}

[[noreturn]] static void run_pong(const size_t iterations, const int core_id) {
	pin_to_core(core_id);
	try_set_sched_fifo();
	try_mlockall();

	tachyon_bus_t *rx = nullptr;
	tachyon_bus_t *tx = nullptr;

	std::thread t_listen([&] { tachyon_bus_listen(SOCK_AP, CAPACITY, &tx); });
	rx = connect_with_retry(SOCK_PA);
	t_listen.join();

	if (!rx || !tx) {
		std::fprintf(stderr, "[pong] handshake failed\n");
		_exit(1);
	}

	alignas(64) std::byte buf[PAYLOAD];
	uint32_t			  type_id	= 0;
	size_t				  actual_sz = 0;

	const size_t total = iterations + WARMUP + 1;
	for (size_t i = 0; i < total; ++i) {
		const void *src = rx_blocking(rx, &type_id, &actual_sz);
		if (!src)
			break;

		std::memcpy(buf, src, actual_sz);
		tachyon_commit_rx(rx);

		void *dst = nullptr;
		while ((dst = tachyon_acquire_tx(tx, actual_sz)) == nullptr)
			tachyon::cpu_relax();

		std::memcpy(dst, buf, actual_sz);
		tachyon_commit_tx(tx, actual_sz, type_id);
		tachyon_flush(tx);

		if (type_id == 0)
			break;
	}

	tachyon_bus_destroy(rx);
	tachyon_bus_destroy(tx);
	_exit(0);
}

static void run_ping(const size_t iterations, const int core_id, const std::string &output_path) {
	pin_to_core(core_id);
	try_set_sched_fifo();
	try_mlockall();

	tachyon_bus_t *tx = nullptr;
	tachyon_bus_t *rx = nullptr;

	std::thread t_listen([&] { tachyon_bus_listen(SOCK_PA, CAPACITY, &tx); });
	rx = connect_with_retry(SOCK_AP);
	t_listen.join();

	if (!rx || !tx) {
		std::fprintf(stderr, "[ping] handshake failed\n");
		return;
	}

	std::printf("[inter] Handshake complete. Warming up (%zu iters)...\n", WARMUP);
	std::fflush(stdout);

	alignas(64) std::byte payload[PAYLOAD]{};
	std::memset(payload, 0xAB, PAYLOAD);
	uint32_t type_id   = 0;
	size_t	 actual_sz = 0;

	auto rtt_once = [&](const uint32_t tid) {
		void *ptr = nullptr;
		while ((ptr = tachyon_acquire_tx(tx, PAYLOAD)) == nullptr)
			tachyon::cpu_relax();

		std::memcpy(ptr, payload, PAYLOAD);
		tachyon_commit_tx(tx, PAYLOAD, tid);
		tachyon_flush(tx);
		if (rx_blocking(rx, &type_id, &actual_sz))
			tachyon_commit_rx(rx);
	};

	for (size_t i = 0; i < WARMUP; ++i)
		rtt_once(1);

	std::printf("[inter] Benchmarking (%zu iters)...\n", iterations);
	std::fflush(stdout);

	std::vector<int64_t> latencies;
	latencies.reserve(iterations);

	const auto bench_start = std::chrono::high_resolution_clock::now();
	for (size_t i = 0; i < iterations; ++i) {
		const auto t0 = std::chrono::high_resolution_clock::now();
		rtt_once(1);
		const auto t1 = std::chrono::high_resolution_clock::now();
		latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
	}

	const auto bench_end = std::chrono::high_resolution_clock::now();
	rtt_once(0);

	const double total_s = std::chrono::duration<double>(bench_end - bench_start).count();
	tachyon_bus_destroy(tx);
	tachyon_bus_destroy(rx);

	std::ranges::sort(latencies);

	double sum = 0.0;
	for (const auto v : latencies) {
		sum += static_cast<double>(v);
	}
	const double mean	= sum / static_cast<double>(iterations);
	const double oneway = pct(latencies, 0.50) / 2.0;

	std::printf("\n[ Inter-process RTT - %zu iterations, payload=%zu bytes ]\n", iterations, PAYLOAD);
	std::printf("Min:        %8.1f ns\n", static_cast<double>(latencies.front()));
	std::printf("p50:        %8.1f ns  (one-way %.1f ns)\n", pct(latencies, 0.50), oneway);
	std::printf("p90:        %8.1f ns\n", pct(latencies, 0.90));
	std::printf("p99:        %8.1f ns\n", pct(latencies, 0.99));
	std::printf("p99.9:      %8.1f ns\n", pct(latencies, 0.999));
	std::printf("p99.99:     %8.1f ns\n", pct(latencies, 0.9999));
	std::printf("Max:        %8.1f ns\n", static_cast<double>(latencies.back()));
	std::printf("Mean:       %8.1f ns\n", mean);
	std::printf("Throughput: %.1f K RTT/sec\n", static_cast<double>(iterations) / total_s / 1e3);

	if (!output_path.empty()) {
		if (FILE *f = std::fopen(output_path.c_str(), "w")) {
			std::fprintf(f, "{\n");
			std::fprintf(f, "  \"context\": {\n");
			std::fprintf(f, "    \"executable\": \"tachyon_bench_inter\",\n");
			std::fprintf(f, "    \"num_cpus\": %d,\n", static_cast<int>(std::thread::hardware_concurrency()));
			std::fprintf(f, "    \"payload_bytes\": %zu\n", PAYLOAD);
			std::fprintf(f, "  },\n");
			std::fprintf(f, "  \"benchmarks\": [\n");
			std::fprintf(f, "    {\n");
			std::fprintf(f, "      \"name\": \"Tachyon/InterProcess/PingPong\",\n");
			std::fprintf(f, "      \"run_name\": \"Tachyon/InterProcess/PingPong\",\n");
			std::fprintf(f, "      \"run_type\": \"iteration\",\n");
			std::fprintf(f, "      \"iterations\": %zu,\n", iterations);
			std::fprintf(f, "      \"real_time\": %.1f,\n", pct(latencies, 0.50));
			std::fprintf(f, "      \"cpu_time\": %.1f,\n", pct(latencies, 0.50));
			std::fprintf(f, "      \"time_unit\": \"ns\",\n");
			std::fprintf(f, "      \"p50_ns\": %.1f,\n", pct(latencies, 0.50));
			std::fprintf(f, "      \"p90_ns\": %.1f,\n", pct(latencies, 0.90));
			std::fprintf(f, "      \"p99_ns\": %.1f,\n", pct(latencies, 0.99));
			std::fprintf(f, "      \"p99.9_ns\": %.1f,\n", pct(latencies, 0.999));
			std::fprintf(f, "      \"p99.99_ns\": %.1f,\n", pct(latencies, 0.9999));
			std::fprintf(f, "      \"throughput_krtt_s\": %.1f\n", static_cast<double>(iterations) / total_s / 1e3);
			std::fprintf(f, "    }\n");
			std::fprintf(f, "  ]\n");
			std::fprintf(f, "}\n");
			std::fclose(f);
			std::printf("\nJSON written to %s\n", output_path.c_str());
		}
	}
}

int main(const int argc, char **argv) {
	size_t		iterations = 1'000'000;
	std::string output_path;

	const char *env_ping  = std::getenv("TACHYON_PING_CORE");
	const char *env_pong  = std::getenv("TACHYON_PONG_CORE");
	const int	ping_core = env_ping ? std::atoi(env_ping) : -1;
	const int	pong_core = env_pong ? std::atoi(env_pong) : -1;

	static constexpr option long_opts[] = {
		{"iterations", required_argument, nullptr, 'i'},
		{"output", required_argument, nullptr, 'o'},
		{nullptr, 0, nullptr, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "i:o:", long_opts, nullptr)) != -1) {
		switch (opt) {
		case 'i':
			iterations = static_cast<size_t>(std::stoull(optarg));
			break;
		case 'o':
			output_path = optarg;
			break;
		default:
			std::fprintf(stderr, "Usage: %s [--iterations N] [--output path.json]\n", argv[0]);
			return 1;
		}
	}

	if (ping_core >= 0 && pong_core >= 0) {
		std::printf("[inter] Core pinning: ping=%d pong=%d\n", ping_core, pong_core);
	} else {
		std::fprintf(
			stderr,
			"[inter] WARNING: TACHYON_PING_CORE/TACHYON_PONG_CORE not set - "
			"results will be unreliable\n"
		);
	}

	::unlink(SOCK_PA);
	::unlink(SOCK_AP);

	const pid_t child = fork();
	if (child < 0) {
		std::perror("fork");
		return 1;
	}

	if (child == 0)
		run_pong(iterations, pong_core);

	usleep(20'000);
	run_ping(iterations, ping_core, output_path);

	int status = 0;
	waitpid(child, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		std::fprintf(stderr, "[bench] pong exited abnormally (status=%d)\n", WEXITSTATUS(status));
		return 1;
	}

	::unlink(SOCK_PA);
	::unlink(SOCK_AP);
	return 0;
}
