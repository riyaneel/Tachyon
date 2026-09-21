#include <cstdint>

#include <emscripten/heap.h>

#include <gtest/gtest.h>

#include "tachyon.h"
#include <tachyon/arena.hpp>
#include <tachyon/shm.hpp>

namespace tachyon::core::test {
	namespace {
		constexpr size_t MAX_VALID_CAPACITY = size_t{1} << 30;

		class WasmTest : public testing::Test {};
	} // namespace

	TEST_F(WasmTest, CapacityBounds) {
		tachyon_bus_t *bus = nullptr;

		EXPECT_EQ(tachyon_bus_listen("/wasm/zero", 0, &bus), TACHYON_ERR_INVALID_SZ);
		EXPECT_EQ(tachyon_bus_listen("/wasm/odd", 4095, &bus), TACHYON_ERR_INVALID_SZ);
		EXPECT_EQ(tachyon_bus_listen("/wasm/int32max", INT32_MAX, &bus), TACHYON_ERR_INVALID_SZ);
		EXPECT_EQ(tachyon_bus_listen("/wasm/over", MAX_VALID_CAPACITY * 2, &bus), TACHYON_ERR_INVALID_SZ);
		EXPECT_EQ(bus, nullptr);

		ASSERT_EQ(tachyon_bus_listen("/wasm/ok", 1 << 16, &bus), TACHYON_SUCCESS);
		ASSERT_NE(bus, nullptr);
		tachyon_bus_destroy(bus);
	}

	TEST_F(WasmTest, LargestCapacityIsNotRejectedByValidation) {
		tachyon_bus_t		 *bus = nullptr;
		const tachyon_error_t rc  = tachyon_bus_listen("/wasm/max", MAX_VALID_CAPACITY, &bus);
		EXPECT_NE(rc, TACHYON_ERR_INVALID_SZ);
		if (rc == TACHYON_SUCCESS) {
			ASSERT_NE(bus, nullptr);
			tachyon_bus_destroy(bus);
		} else {
			EXPECT_EQ(rc, TACHYON_ERR_MAP);
			EXPECT_EQ(bus, nullptr);
		}
	}

	TEST_F(WasmTest, ShmSizeIsAlsoBounded) {
		const auto oversized = SharedMemory::create("/wasm/shm", static_cast<size_t>(INT32_MAX) + 1);
		ASSERT_FALSE(oversized.has_value());
		EXPECT_EQ(oversized.error(), ShmError::InvalidSize);
	}

	TEST_F(WasmTest, JoinIsUnavailable) {
		for (const int fd : {-1, 0, 3, 4096}) {
			const auto joined = SharedMemory::join(fd, 4096);
			ASSERT_FALSE(joined.has_value());
			EXPECT_EQ(joined.error(), ShmError::OpenFailed);
		}
	}

	TEST_F(WasmTest, ConnectIsUnavailable) {
		tachyon_bus_t *bus = nullptr;
		EXPECT_NE(tachyon_bus_connect("/wasm/nothing", &bus), TACHYON_SUCCESS);
		EXPECT_EQ(bus, nullptr);
	}

	TEST_F(WasmTest, HeapArenaRoundTrip) {
		tachyon_bus_t *bus = nullptr;
		ASSERT_EQ(tachyon_bus_listen("/wasm/roundtrip", 1 << 16, &bus), TACHYON_SUCCESS);

		void *base = tachyon_bus_get_shm_ptr(bus);
		ASSERT_NE(base, nullptr);
		EXPECT_EQ(reinterpret_cast<uintptr_t>(base) % alignof(MemoryLayout), 0U);

		void *tx = tachyon_acquire_tx(bus, 4);
		ASSERT_NE(tx, nullptr);
		static_cast<uint8_t *>(tx)[0] = 0x5A;
		ASSERT_EQ(tachyon_commit_tx(bus, 4, 7), TACHYON_SUCCESS);
		tachyon_flush(bus);

		uint32_t	type_id = 0;
		size_t		size	= 0;
		const void *rx		= tachyon_acquire_rx(bus, &type_id, &size);
		ASSERT_NE(rx, nullptr);
		EXPECT_EQ(type_id, 7U);
		EXPECT_EQ(size, size_t{4});
		EXPECT_EQ(static_cast<const uint8_t *>(rx)[0], 0x5A);
		EXPECT_EQ(tachyon_commit_rx(bus), TACHYON_SUCCESS);

		tachyon_bus_destroy(bus);
	}

	TEST_F(WasmTest, HeapGrowthKeepsArenaPointersValid) {
		tachyon_bus_t *small = nullptr;
		ASSERT_EQ(tachyon_bus_listen("/wasm/growth-small", 1 << 16, &small), TACHYON_SUCCESS);

		const void *base_before = tachyon_bus_get_shm_ptr(small);
		ASSERT_NE(base_before, nullptr);

		void *tx = tachyon_acquire_tx(small, 8);
		ASSERT_NE(tx, nullptr);
		static_cast<uint8_t *>(tx)[0] = 0xC3;
		static_cast<uint8_t *>(tx)[7] = 0x3C;

		size_t capacity = 1;
		while (capacity < emscripten_get_heap_size()) {
			capacity <<= 1;
		}

		const size_t   heap_before = emscripten_get_heap_size();
		tachyon_bus_t *big		   = nullptr;
		if (const auto rc = tachyon_bus_listen("/wasm/growth-big", capacity, &big); rc == TACHYON_SUCCESS) {
			ASSERT_NE(big, nullptr);
			EXPECT_GT(emscripten_get_heap_size(), heap_before);
		} else {
			EXPECT_EQ(rc, TACHYON_ERR_MAP);
			EXPECT_EQ(big, nullptr);
		}

		EXPECT_EQ(tachyon_bus_get_shm_ptr(small), base_before);
		EXPECT_EQ(static_cast<const uint8_t *>(tx)[0], 0xC3);
		EXPECT_EQ(static_cast<const uint8_t *>(tx)[7], 0x3C);

		ASSERT_EQ(tachyon_commit_tx(small, 8, 5), TACHYON_SUCCESS);
		tachyon_flush(small);

		uint32_t	type_id = 0;
		size_t		size	= 0;
		const void *rx		= tachyon_acquire_rx(small, &type_id, &size);
		ASSERT_NE(rx, nullptr);
		EXPECT_EQ(type_id, 5U);
		EXPECT_EQ(size, size_t{8});
		EXPECT_EQ(static_cast<const uint8_t *>(rx)[0], 0xC3);
		EXPECT_EQ(tachyon_commit_rx(small), TACHYON_SUCCESS);

		if (big != nullptr) {
			tachyon_bus_destroy(big);
		}
		tachyon_bus_destroy(small);
	}
} // namespace tachyon::core::test
