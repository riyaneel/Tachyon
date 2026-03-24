#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include <tachyon.h>

static constexpr const char *SOCKET_PATH  = "/tmp/tachyon_cpp_inference.sock";
static constexpr size_t		 ITERATIONS	  = 500'000;
static constexpr size_t		 BATCH_SIZE	  = 32;
static constexpr size_t		 FRAME_FLOATS = 256;
static constexpr size_t		 FRAME_BYTES  = FRAME_FLOATS * sizeof(float);

static constexpr uint32_t TYPE_FEATURES = 1;
static constexpr uint32_t TYPE_SENTINEL = 0;

static void make_features(const size_t frame, float *buf) {
	const float base = static_cast<float>(frame);
	for (size_t i = 0; i < FRAME_FLOATS; ++i) {
		buf[i] = base + static_cast<float>(i) * 0.001f;
	}
}

static void *acquire_with_backpressure(tachyon_bus_t *bus, const size_t size) {
	for (unsigned spins = 0;;) {
		void *ptr = tachyon_acquire_tx(bus, size);
		if (ptr != nullptr)
			return ptr;

		if (++spins < 32) {
#if defined(__x86_64__) || defined(_M_X64)
			__asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__)
			__asm__ volatile("yield" ::: "memory");
#endif
		} else {
			std::this_thread::sleep_for(std::chrono::microseconds(10));
			spins = 0;
		}
	}
}

int main() {
	std::printf("[producer] Connecting to %s ...\n", SOCKET_PATH);
	std::printf("[producer] Waiting for Python consumer to listen ...\n");

	tachyon_bus_t *bus = nullptr;
	for (int attempt = 1; attempt <= 100; ++attempt) {
		if (tachyon_bus_connect(SOCKET_PATH, &bus) == TACHYON_SUCCESS)
			break;
		if (attempt == 1)
			std::printf("[producer] Consumer not ready, retrying ...\n");
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	if (bus == nullptr) {
		std::fprintf(
			stderr,
			"[producer] Failed to connect after 100 attempts — "
			"is the consumer running?\n"
		);
		return 1;
	}

	std::printf("[producer] Connected. Sending %zu feature vectors (batch=%zu) ...\n", ITERATIONS, BATCH_SIZE);

	float	   features[FRAME_FLOATS];
	const auto start = std::chrono::steady_clock::now();

	for (size_t i = 0; i < ITERATIONS; ++i) {
		make_features(i, features);

		void *ptr = acquire_with_backpressure(bus, FRAME_BYTES);
		std::memcpy(ptr, features, FRAME_BYTES);
		tachyon_commit_tx(bus, FRAME_BYTES, TYPE_FEATURES);

		if ((i + 1) % BATCH_SIZE == 0)
			tachyon_flush(bus);
	}

	/* Sentinel */
	void *ptr = acquire_with_backpressure(bus, FRAME_BYTES);
	std::memset(ptr, 0, FRAME_BYTES);
	tachyon_commit_tx(bus, FRAME_BYTES, TYPE_SENTINEL);
	tachyon_flush(bus);

	const auto	 end = std::chrono::steady_clock::now();
	const double ms	 = std::chrono::duration<double, std::milli>(end - start).count();
	const double fps = static_cast<double>(ITERATIONS) / ms * 1000.0;

	std::printf("[producer] Done. %zu frames in %.1f ms (%.0f frames/sec)\n", ITERATIONS, ms, fps);

	tachyon_bus_destroy(bus);
	return 0;
}