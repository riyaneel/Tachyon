#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

#include <pthread.h>
#include <zmq.h>

#include <benchmark/benchmark.h>

#include <tachyon.hpp>

static constexpr size_t PAYLOAD		 = 32;
static constexpr size_t WARMUP_ITERS = 10'000;

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
	CPU_SET(static_cast<size_t>(core_id), &cs);
	if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cs) != 0) {
		std::fprintf(stderr, "[bench_zmq] WARNING: failed to pin server thread to core %d\n", core_id);
	}
}

class ZmqInprocRTT : public benchmark::Fixture {
public:
	void			 *ctx_		 = nullptr;
	void			 *main_sock_ = nullptr;
	std::thread		  server_thread_;
	std::atomic<bool> server_ready_{false};
	std::atomic<bool> stop_{false};

	void SetUp(const benchmark::State &) override {
		stop_.store(false, std::memory_order_relaxed);
		server_ready_.store(false, std::memory_order_relaxed);
		ctx_	   = zmq_ctx_new();
		main_sock_ = zmq_socket(ctx_, ZMQ_PAIR);

		constexpr int hwm = 0;
		zmq_setsockopt(main_sock_, ZMQ_SNDHWM, &hwm, sizeof(hwm));
		zmq_setsockopt(main_sock_, ZMQ_RCVHWM, &hwm, sizeof(hwm));
		zmq_bind(main_sock_, "inproc://tachyon_pingpong");

		const int server_core = server_core_from_env();
		server_thread_		  = std::thread([this, server_core] {
			   pin_to_core(server_core);

			   void			*server_sock = zmq_socket(ctx_, ZMQ_PAIR);
			   constexpr int shwm		 = 0;
			   zmq_setsockopt(server_sock, ZMQ_SNDHWM, &shwm, sizeof(shwm));
			   zmq_setsockopt(server_sock, ZMQ_RCVHWM, &shwm, sizeof(shwm));

			   zmq_connect(server_sock, "inproc://tachyon_pingpong");

			   server_ready_.store(true, std::memory_order_release);

			   alignas(64) char buf[PAYLOAD];
			   while (!stop_.load(std::memory_order_acquire)) {
				   if (zmq_recv(server_sock, buf, PAYLOAD, ZMQ_DONTWAIT) < 0) {
					   if (zmq_errno() == EAGAIN) {
						   tachyon::cpu_relax();
						   continue;
					   }

					   if (zmq_errno() == ETERM) {
						   break;
					   }
				   }

				   zmq_send(server_sock, buf, PAYLOAD, 0);
			   }

			   zmq_close(server_sock);
		   });

		while (!server_ready_.load(std::memory_order_acquire)) {
			tachyon::cpu_relax();
		}

		alignas(64) char payload[PAYLOAD]{};
		for (size_t i = 0; i < WARMUP_ITERS; ++i) {
			zmq_send(main_sock_, payload, PAYLOAD, 0);
			zmq_recv(main_sock_, payload, PAYLOAD, 0);
		}
	}

	void TearDown(const benchmark::State &) override {
		stop_.store(true, std::memory_order_release);
		server_thread_.join();
		zmq_close(main_sock_);
		zmq_ctx_destroy(ctx_);
	}
};

BENCHMARK_DEFINE_F(ZmqInprocRTT, PingPong)(benchmark::State &state) {
	alignas(64) char	 payload[PAYLOAD]{};
	std::vector<int64_t> latencies;
	latencies.reserve(static_cast<size_t>(state.max_iterations));

	for ([[maybe_unused]] auto _ : state) {
		const auto t0 = std::chrono::high_resolution_clock::now();

		zmq_send(main_sock_, payload, PAYLOAD, 0);
		zmq_recv(main_sock_, payload, PAYLOAD, 0);

		const auto t1 = std::chrono::high_resolution_clock::now();
		const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

		state.SetIterationTime(static_cast<double>(ns) * 1e-9);
		latencies.push_back(ns);
	}

	std::ranges::sort(latencies);
	auto pct = [&](const double p) -> double {
		const size_t idx = static_cast<size_t>(static_cast<double>(latencies.size()) * p);
		return static_cast<double>(latencies[std::min(idx, latencies.size() - 1)]);
	};

	state.counters["p50_ns"]	= pct(0.50);
	state.counters["p90_ns"]	= pct(0.90);
	state.counters["p99_ns"]	= pct(0.99);
	state.counters["p99.9_ns"]	= pct(0.999);
	state.counters["p99.99_ns"] = pct(0.9999);
}

BENCHMARK_REGISTER_F(ZmqInprocRTT, PingPong)
	->UseManualTime()
	->Iterations(1'000'000)
	->Repetitions(3)
	->DisplayAggregatesOnly(true)
	->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
