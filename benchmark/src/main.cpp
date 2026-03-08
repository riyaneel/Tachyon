#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <thread>
#include <vector>

#include <tachyon/arena.hpp>
#include <tachyon/shm.hpp>

using namespace tachyon::core;

constexpr size_t ARENA_CAPACITY = 65536 * 16;
constexpr size_t SHM_SIZE		= sizeof(MemoryLayout) + ARENA_CAPACITY;
constexpr size_t ITERATIONS		= 1'000'000;
constexpr size_t WARMUP_ITERS	= 10'000;

void pin_thread_to_core(const int core_id) {
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(static_cast<size_t>(core_id), &cpuset);
	if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
		std::cerr << "CRITICAL WARNING: Failed to pin thread to core " << core_id << "\n";
	}
}

int main() {
	const auto shm_c2s_res = SharedMemory::create("tachyon_c2s", SHM_SIZE);
	const auto shm_s2c_res = SharedMemory::create("tachyon_s2c", SHM_SIZE);
	if (!shm_c2s_res.has_value() || !shm_s2c_res.has_value()) {
		std::cerr << "Failed to create Ping-Pong SHMs\n";
		return 1;
	}

	const auto &shm_c2s = shm_c2s_res.value();
	const auto &shm_s2c = shm_s2c_res.value();

	Arena::format(shm_c2s.data(), ARENA_CAPACITY).value();
	Arena::format(shm_s2c.data(), ARENA_CAPACITY).value();

	std::cout << "--- Tachyon IPC Latency Analyzer (Phase 7.4) ---\n";
	std::cout << "Mode:      Ping-Pong RTT (Request/Response)\n";
	std::cout << "Payload:   32 bytes\n";
	std::cout << "Samples:   " << ITERATIONS << "\n";

	std::atomic server_ready{false};

	std::thread t_server([&]() {
		pin_thread_to_core(9);
		auto rx = Arena::attach(shm_c2s.data()).value();
		auto tx = Arena::attach(shm_s2c.data()).value();

		alignas(64) std::byte buffer[32]{};
		size_t				  bytes_read = 0;
		uint32_t			  type_id	 = 0;

		server_ready.store(true, std::memory_order_release);

		constexpr size_t total_runs = ITERATIONS + WARMUP_ITERS;
		for (size_t i = 0; i < total_runs; ++i) {
			(void)rx.pop_spin(type_id, buffer, bytes_read, 0);
			while (!tx.try_push(type_id, {buffer, bytes_read})) {
				tachyon::cpu_relax();
			}
			tx.flush();
		}
	});

	std::thread t_client([&]() {
		pin_thread_to_core(8);
		auto tx = Arena::attach(shm_c2s.data()).value();
		auto rx = Arena::attach(shm_s2c.data()).value();

		while (!server_ready.load(std::memory_order_acquire)) {
			tachyon::cpu_relax();
		}

		alignas(64) std::byte send_buffer[32]{};
		alignas(64) std::byte recv_buffer[64]{};
		size_t				  bytes_read = 0;
		uint32_t			  type_id	 = 0;
		std::vector<uint64_t> latencies(ITERATIONS);

		for (size_t i = 0; i < WARMUP_ITERS; ++i) {
			while (!tx.try_push(1, {send_buffer, 32})) {
				tachyon::cpu_relax();
			}
			tx.flush();
			(void)rx.pop_spin(type_id, recv_buffer, bytes_read, 0);
		}

		const auto bench_start = std::chrono::high_resolution_clock::now();
		for (size_t i = 0; i < ITERATIONS; ++i) {
			const auto start = std::chrono::high_resolution_clock::now();

			while (!tx.try_push(1, {send_buffer, 32})) {
				tachyon::cpu_relax();
			}
			tx.flush();

			(void)rx.pop_spin(type_id, recv_buffer, bytes_read, 0);

			const auto end = std::chrono::high_resolution_clock::now();
			latencies[i] =
				static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
		}

		const auto	 bench_end = std::chrono::high_resolution_clock::now();
		const double total_sec =
			static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(bench_end - bench_start).count()) /
			1e9;

		std::ranges::sort(latencies);

		std::cout << "\n[ RTT Latency Percentiles (ns) ]\n";
		std::cout << "Min:     " << std::setw(8) << latencies[0] << " ns\n";
		std::cout << "p50:     " << std::setw(8) << latencies[static_cast<size_t>(ITERATIONS * 0.50)]
				  << " ns (Median)\n";
		std::cout << "p90:     " << std::setw(8) << latencies[static_cast<size_t>(ITERATIONS * 0.90)] << " ns\n";
		std::cout << "p99:     " << std::setw(8) << latencies[static_cast<size_t>(ITERATIONS * 0.99)] << " ns\n";
		std::cout << "p99.9:   " << std::setw(8) << latencies[static_cast<size_t>(ITERATIONS * 0.999)]
				  << " ns (Tail Latency)\n";
		std::cout << "p99.99:  " << std::setw(8) << latencies[static_cast<size_t>(ITERATIONS * 0.9999)] << " ns\n";
		std::cout << "Max:     " << std::setw(8) << latencies.back() << " ns\n";
		std::cout << "\nThroughput: " << (static_cast<double>(ITERATIONS) / total_sec) / 1e3 << " K RTT/sec\n";
	});

	t_server.join();
	t_client.join();

	return 0;
}
