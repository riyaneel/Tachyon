#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <pthread.h>

#include <benchmark/benchmark.h>

#include <tachyon/arena.hpp>
#include <tachyon/shm.hpp>

using namespace tachyon::core;

static constexpr std::size_t BENCH_CAPACITY = 1 << 20; // 1 MB
static constexpr std::size_t BENCH_SHM_SIZE = sizeof(MemoryLayout) + BENCH_CAPACITY;
static constexpr std::size_t PAYLOAD_BYTES	= 32;
static constexpr std::size_t WARMUP_ITERS	= 10'000;

static int server_core_from_env() noexcept {
	const char *env = std::getenv("TACHYON_SERVER_CORE");
	if (!env)
		return -1;

	const int core = std::atoi(env);
	return (core >= 0) ? core : -1;
}

static void pin_to_core(const int core_id) noexcept {
	if (core_id < 0)
		return;

	cpu_set_t cs;
	CPU_ZERO(&cs);
	CPU_SET(static_cast<std::size_t>(core_id), &cs);
	if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cs) != 0) {
		std::fprintf(stderr, "[bench_intra] WARNING: failed to pin server thread to core %d\n", core_id);
	}
}

static double percentile(const std::vector<int64_t> &sorted, const double p) {
	if (sorted.empty())
		return 0.0;

	const auto idx = static_cast<std::size_t>(static_cast<double>(sorted.size()) * p);
	return static_cast<double>(sorted[std::min(idx, sorted.size() - 1)]);
}

class IntraRTT : public benchmark::Fixture {
public:
	std::optional<SharedMemory> shm_c2s_;
	std::optional<SharedMemory> shm_s2c_;
	std::optional<Arena>		tx_c2s_;
	std::optional<Arena>		rx_s2c_;
	std::optional<Arena>		rx_c2s_;
	std::optional<Arena>		tx_s2c_;
	std::thread					server_thread_;
	std::atomic<bool>			server_ready_{false};
	std::atomic<bool>			stop_{false};

	void SetUp(const benchmark::State &) override {
		stop_.store(false, std::memory_order_relaxed);
		server_ready_.store(false, std::memory_order_relaxed);

		shm_c2s_.emplace(SharedMemory::create("bench_c2s", BENCH_SHM_SIZE).value());
		shm_s2c_.emplace(SharedMemory::create("bench_s2c", BENCH_SHM_SIZE).value());

		Arena::format(shm_c2s_->data(), BENCH_CAPACITY).value();
		Arena::format(shm_s2c_->data(), BENCH_CAPACITY).value();

		tx_c2s_.emplace(Arena::attach(shm_c2s_->data()).value());
		rx_s2c_.emplace(Arena::attach(shm_s2c_->data()).value());
		rx_c2s_.emplace(Arena::attach(shm_c2s_->data()).value());
		tx_s2c_.emplace(Arena::attach(shm_s2c_->data()).value());

		const int server_core = server_core_from_env();
		server_thread_		  = std::thread([this, server_core] {
			   pin_to_core(server_core);

			   server_ready_.store(true, std::memory_order_release);
			   uint32_t	   type_id	 = 0;
			   std::size_t actual_sz = 0;

			   while (!stop_.load(std::memory_order_acquire)) {
				   const std::byte *src = rx_c2s_->acquire_rx_spin(type_id, actual_sz, 10'000);
				   if (!src) {
					   continue;
				   }

				   std::byte *dst = nullptr;
				   while ((dst = tx_s2c_->acquire_tx(actual_sz)) == nullptr) {
					   tachyon::cpu_relax();
				   }

				   std::memcpy(dst, src, actual_sz);
				   (void)rx_c2s_->commit_rx();
				   (void)tx_s2c_->commit_tx(actual_sz, type_id);
				   tx_s2c_->flush_tx();
			   }
		   });

		while (!server_ready_.load(std::memory_order_acquire)) {
			tachyon::cpu_relax();
		}

		alignas(64) constexpr std::byte payload[PAYLOAD_BYTES]{};
		for (std::size_t i = 0; i < WARMUP_ITERS; ++i) {
			std::byte *ptr = nullptr;
			while ((ptr = tx_c2s_->acquire_tx(PAYLOAD_BYTES)) == nullptr) {
				tachyon::cpu_relax();
			}

			std::memcpy(ptr, payload, PAYLOAD_BYTES);
			(void)tx_c2s_->commit_tx(PAYLOAD_BYTES, 1);
			tx_c2s_->flush_tx();

			uint32_t	tid = 0;
			std::size_t sz	= 0;
			while (rx_s2c_->acquire_rx_spin(tid, sz, 0) == nullptr) {
				tachyon::cpu_relax();
			}

			(void)rx_s2c_->commit_rx();
		}
	}

	void TearDown(const benchmark::State &) override {
		stop_.store(true, std::memory_order_release);
		if (tx_c2s_->acquire_tx(1)) {
			(void)tx_c2s_->commit_tx(0, 0);
			tx_c2s_->flush_tx();
		}

		server_thread_.join();

		tx_c2s_.reset();
		rx_s2c_.reset();
		rx_c2s_.reset();
		tx_s2c_.reset();
		shm_c2s_.reset();
		shm_s2c_.reset();
	}
};

BENCHMARK_DEFINE_F(IntraRTT, PingPong)(benchmark::State &state) {
	alignas(64) std::byte payload[PAYLOAD_BYTES]{};
	std::memset(payload, 0xAB, PAYLOAD_BYTES);

	std::vector<int64_t> latencies;
	latencies.reserve(static_cast<size_t>(state.max_iterations));

	for ([[maybe_unused]] auto _ : state) {
		const auto t0  = std::chrono::high_resolution_clock::now();
		std::byte *ptr = nullptr;
		while ((ptr = tx_c2s_->acquire_tx(PAYLOAD_BYTES)) == nullptr) {
			tachyon::cpu_relax();
		}

		std::memcpy(ptr, payload, PAYLOAD_BYTES);
		(void)tx_c2s_->commit_tx(PAYLOAD_BYTES, 1);
		tx_c2s_->flush_tx();

		uint32_t	type_id	  = 0;
		std::size_t actual_sz = 0;
		while (rx_s2c_->acquire_rx_spin(type_id, actual_sz, 0) == nullptr) {
			tachyon::cpu_relax();
		}

		(void)rx_s2c_->commit_rx();
		const auto t1 = std::chrono::high_resolution_clock::now();
		const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

		state.SetIterationTime(static_cast<double>(ns) * 1e-9);
		latencies.push_back(ns);
	}

	std::ranges::sort(latencies);
	const double n = static_cast<double>(state.iterations());

	state.counters["p50_ns"]			= percentile(latencies, 0.50);
	state.counters["p90_ns"]			= percentile(latencies, 0.90);
	state.counters["p99_ns"]			= percentile(latencies, 0.99);
	state.counters["p99.9_ns"]			= percentile(latencies, 0.999);
	state.counters["p99.99_ns"]			= percentile(latencies, 0.9999);
	state.counters["throughput_krtt_s"] = benchmark::Counter(n, benchmark::Counter::kIsRate) / 1e3;
}

BENCHMARK_REGISTER_F(IntraRTT, PingPong)
	->UseManualTime()
	->Iterations(1'000'000)
	->Repetitions(3)
	->DisplayAggregatesOnly(true)
	->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
