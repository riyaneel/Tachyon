#include <atomic>
#include <chrono>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <thread>

#include <tachyon/arena.hpp>
#include <tachyon/shm.hpp>

using namespace tachyon::core;

constexpr size_t ARENA_CAPACITY = 65536 * 16;
constexpr size_t SHM_SIZE		= sizeof(MemoryLayout) + ARENA_CAPACITY;
constexpr size_t ITERATIONS		= 10'000'000;

void pin_thread_to_core(const int core_id) {
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(static_cast<size_t>(core_id), &cpuset);
	if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
		std::cerr << "CRITICAL WARNING: Failed to pin thread to core " << core_id << "\n";
	}
}

int main() {
	const auto shm_res = SharedMemory::create("tachyon_raw_bench", SHM_SIZE);
	if (!shm_res.has_value()) {
		std::cerr << "Failed to create SHM\n";
		return 1;
	}

	const auto &shm = shm_res.value();
	Arena::format(shm.data(), ARENA_CAPACITY).value();

	std::cout << "--- Tachyon IPC Benchmark ---\n";
	std::cout << "Messages: " << ITERATIONS << " | Size: 32 bytes\n";

	std::atomic producer_done{false};
	const auto	start = std::chrono::high_resolution_clock::now();

	std::thread t_prod([&]() {
		pin_thread_to_core(8);
		auto				  producer = Arena::attach(shm.data()).value();
		alignas(64) std::byte send_buffer[32]{};

		for (size_t i = 0; i < ITERATIONS; ++i) {
			while (!producer.try_push(1, {send_buffer, 32})) {
				tachyon::cpu_relax();
			}
		}

		producer.flush();
		producer_done.store(true, std::memory_order_release);
	});

	std::thread t_cons([&]() {
		pin_thread_to_core(9);
		auto				  consumer = Arena::attach(shm.data()).value();
		alignas(64) std::byte recv_buffer[64]{};
		size_t				  bytes_read	  = 0;
		size_t				  items_processed = 0;
		uint32_t			  type_id		  = 0;

		while (items_processed < ITERATIONS) {
			if (consumer.pop_blocking(type_id, recv_buffer, bytes_read, 1000)) {
				items_processed++;
			} else {
				if (producer_done.load(std::memory_order_acquire)) {
					break;
				}
			}
		}
		consumer.flush();
	});

	t_prod.join();
	t_cons.join();

	const auto	 end		 = std::chrono::high_resolution_clock::now();
	const auto	 duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
	const double seconds	 = static_cast<double>(duration_ns) / 1e9;

	const double items_per_sec	   = static_cast<double>(ITERATIONS) / seconds;
	const double bytes_per_sec	   = (static_cast<double>(ITERATIONS) * 32.0) / seconds;
	const double gigabytes_per_sec = bytes_per_sec / (1024.0 * 1024.0 * 1024.0);

	std::cout << "Time:       " << seconds * 1000.0 << " ms\n";
	std::cout << "Throughput: " << items_per_sec / 1e6 << " M msgs/sec\n";
	std::cout << "Bandwidth:  " << gigabytes_per_sec << " GiB/s\n";

	return 0;
}
