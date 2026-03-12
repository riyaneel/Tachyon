#include <cstring>
#include <numeric>
#include <optional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <tachyon.hpp>
#include <tachyon/arena.hpp>
#include <tachyon/shm.hpp>

namespace tachyon::core::test {
	struct DummyOrder {
		uint64_t id;
		double	 price;
		uint32_t qty;
	};

	class ArenaTest : public ::testing::Test {
	protected:
		const std::string			test_name		  = "tachyon_test_arena";
		const size_t				arena_capacity	  = 4096;
		const size_t				required_shm_size = sizeof(MemoryLayout) + arena_capacity;
		std::optional<SharedMemory> shm_owner;

		void SetUp() override {
			auto result = SharedMemory::create(test_name, required_shm_size);
			ASSERT_TRUE(result.has_value());
			shm_owner = std::move(result.value());
		}
	};

	TEST_F(ArenaTest, FormatAndAttach) {
		const auto producer = Arena::format(shm_owner->data(), arena_capacity);
		ASSERT_TRUE(producer.has_value());
		EXPECT_EQ(producer->get_state(), BusState::Ready);

		const auto consumer = Arena::attach(shm_owner->data());
		ASSERT_TRUE(consumer.has_value());
	}

	TEST_F(ArenaTest, BasicLifecycle) {
		auto producer = Arena::format(shm_owner->data(), arena_capacity).value();
		auto consumer = Arena::attach(shm_owner->data()).value();

		std::byte *tx_ptr = producer.acquire_tx(sizeof(DummyOrder));
		ASSERT_NE(tx_ptr, nullptr);

		new (tx_ptr) DummyOrder{42, 9500.50, 100};
		EXPECT_TRUE(producer.commit_tx(sizeof(DummyOrder), 1));
		producer.flush();

		uint32_t		 type_id_out	 = 0;
		size_t			 actual_size_out = 0;
		const std::byte *rx_ptr			 = consumer.acquire_rx(type_id_out, actual_size_out);
		ASSERT_NE(rx_ptr, nullptr);

		EXPECT_EQ(type_id_out, 1);
		EXPECT_EQ(actual_size_out, sizeof(DummyOrder));

		const auto *recv_order = reinterpret_cast<const DummyOrder *>(rx_ptr);
		EXPECT_EQ(recv_order->id, 42);
		EXPECT_DOUBLE_EQ(recv_order->price, 9500.50);
		EXPECT_EQ(recv_order->qty, 100);

		EXPECT_TRUE(consumer.commit_rx());
	}

	TEST_F(ArenaTest, SizeDisparityReservedSize) {
		auto producer = Arena::format(shm_owner->data(), arena_capacity).value();
		auto consumer = Arena::attach(shm_owner->data()).value();

		std::byte *tx_ptr1 = producer.acquire_tx(1024);
		ASSERT_NE(tx_ptr1, nullptr);

		EXPECT_TRUE(producer.commit_tx(256, 42));
		producer.flush();

		uint32_t		 type_id	 = 0;
		size_t			 actual_size = 0;
		const std::byte *rx_ptr		 = consumer.acquire_rx(type_id, actual_size);
		ASSERT_NE(rx_ptr, nullptr);

		EXPECT_EQ(actual_size, 256);
		EXPECT_TRUE(consumer.commit_rx());

		std::byte *tx_ptr2 = producer.acquire_tx(10);
		ASSERT_NE(tx_ptr2, nullptr);

		const auto spatial_difference = static_cast<size_t>(tx_ptr2 - tx_ptr1);
		EXPECT_EQ(spatial_difference, 1024 + TACHYON_MSG_ALIGNMENT);
		EXPECT_TRUE(producer.commit_tx(10, 43));
	}

	TEST_F(ArenaTest, AVX2AlignmentCheck) {
		auto producer = Arena::format(shm_owner->data(), arena_capacity).value();
		auto consumer = Arena::attach(shm_owner->data()).value();

		const std::vector<size_t> random_sizes = {13, 27, 42, 100, 255};
		for (const size_t sz : random_sizes) {
			std::byte *tx_ptr = producer.acquire_tx(sz);
			ASSERT_NE(tx_ptr, nullptr);
			EXPECT_EQ(reinterpret_cast<uintptr_t>(tx_ptr) % 32, 0);
			EXPECT_TRUE(producer.commit_tx(sz, 1));
		}

		producer.flush();

		for (const size_t sz : random_sizes) {
			uint32_t		 type_id;
			size_t			 actual_size;
			const std::byte *rx_ptr = consumer.acquire_rx(type_id, actual_size);
			ASSERT_NE(rx_ptr, nullptr);
			EXPECT_EQ(reinterpret_cast<uintptr_t>(rx_ptr) % 32, 0);
			EXPECT_EQ(actual_size, sz);
			EXPECT_TRUE(consumer.commit_rx());
		}
	}

	TEST_F(ArenaTest, WrapAroundSkipMarkerZeroCopy) {
		auto producer = Arena::format(shm_owner->data(), arena_capacity).value();
		auto consumer = Arena::attach(shm_owner->data()).value();

		std::byte *tx_ptr1 = producer.acquire_tx(3936);
		ASSERT_NE(tx_ptr1, nullptr);
		EXPECT_TRUE(producer.commit_tx(3936, 1));
		producer.flush();

		uint32_t		 tid;
		size_t			 sz;
		const std::byte *rx_ptr1 = consumer.acquire_rx(tid, sz);
		EXPECT_TRUE(consumer.commit_rx());
		consumer.flush();

		std::byte *tx_ptr2 = producer.acquire_tx(100);
		ASSERT_NE(tx_ptr2, nullptr);
		EXPECT_EQ(reinterpret_cast<uintptr_t>(tx_ptr2) % 32, 0);
		tx_ptr2[0]	= std::byte{'A'};
		tx_ptr2[99] = std::byte{'Z'};
		EXPECT_TRUE(producer.commit_tx(100, 2));
		producer.flush();

		const std::byte *rx_ptr2 = consumer.acquire_rx(tid, sz);
		ASSERT_NE(rx_ptr2, nullptr);
		EXPECT_EQ(sz, 100);
		EXPECT_EQ(tid, 2);
		EXPECT_EQ(rx_ptr2[0], std::byte{'A'});
		EXPECT_EQ(rx_ptr2[99], std::byte{'Z'});

		EXPECT_EQ(rx_ptr2, rx_ptr1);
		EXPECT_TRUE(consumer.commit_rx());
	}

	TEST_F(ArenaTest, EdgeCases) {
		auto	 producer = Arena::format(shm_owner->data(), arena_capacity).value();
		auto	 consumer = Arena::attach(shm_owner->data()).value();
		uint32_t tid;
		size_t	 sz;

		EXPECT_EQ(consumer.acquire_rx(tid, sz), nullptr);

		std::byte *tx_ptr = producer.acquire_tx(0);
		ASSERT_NE(tx_ptr, nullptr);
		EXPECT_TRUE(producer.commit_tx(0, 99));
		producer.flush();

		const std::byte *rx_ptr = consumer.acquire_rx(tid, sz);
		ASSERT_NE(rx_ptr, nullptr);
		EXPECT_EQ(sz, 0);
		EXPECT_EQ(tid, 99);
		EXPECT_TRUE(consumer.commit_rx());

		EXPECT_EQ(producer.acquire_tx(arena_capacity * 2), nullptr);
		EXPECT_EQ(producer.acquire_tx(arena_capacity), nullptr);

		size_t msg_count = 0;
		while (true) {
			if (producer.acquire_tx(32) != nullptr) {
				EXPECT_TRUE(producer.commit_tx(32, 1));
				msg_count++;
			} else {
				break;
			}
		}

		producer.flush();
		EXPECT_GT(msg_count, 0);
		EXPECT_EQ(producer.acquire_tx(32), nullptr);

		size_t rx_count = 0;
		while (consumer.acquire_rx(tid, sz) != nullptr) {
			EXPECT_TRUE(consumer.commit_rx());
			rx_count++;
		}

		EXPECT_EQ(rx_count, msg_count);

		EXPECT_NE(producer.acquire_tx(32), nullptr);
		EXPECT_TRUE(producer.commit_tx(32, 1));
	}

	TEST_F(ArenaTest, ConcurrentStress) {
		auto producer = Arena::format(shm_owner->data(), arena_capacity).value();
		auto consumer = Arena::attach(shm_owner->data()).value();

		constexpr size_t ITERATIONS = 1'000'000;
		std::atomic		 start_flag{false};

		std::thread t_prod([&]() {
			while (!start_flag.load(std::memory_order_acquire)) {
			}

			for (size_t i = 0; i < ITERATIONS; ++i) {
				std::byte *ptr = nullptr;
				while ((ptr = producer.acquire_tx(sizeof(size_t))) == nullptr) {
					cpu_relax();
				}
				std::memcpy(ptr, &i, sizeof(size_t));
				EXPECT_TRUE(producer.commit_tx(sizeof(size_t), 1));
			}
			producer.flush();
		});

		std::thread t_cons([&]() {
			while (!start_flag.load(std::memory_order_acquire)) {
			}

			for (size_t i = 0; i < ITERATIONS; ++i) {
				size_t			 expected_val = 0;
				size_t			 recv_size	  = 0;
				uint32_t		 type_id	  = 0;
				const std::byte *ptr		  = nullptr;

				while ((ptr = consumer.acquire_rx(type_id, recv_size)) == nullptr) {
					cpu_relax();
				}

				EXPECT_EQ(recv_size, sizeof(size_t));
				std::memcpy(&expected_val, ptr, sizeof(size_t));
				ASSERT_EQ(expected_val, i);
				EXPECT_TRUE(consumer.commit_rx());
			}
			consumer.flush();
		});

		start_flag.store(true, std::memory_order_release);
		t_prod.join();
		t_cons.join();
	}

	TEST_F(ArenaTest, AdaptiveSpinningFutex) {
		auto producer = Arena::format(shm_owner->data(), arena_capacity).value();
		auto consumer = Arena::attach(shm_owner->data()).value();

		std::atomic start_flag{false};
		std::atomic consumer_ready{false};

		std::thread t_cons([&]() {
			consumer_ready.store(true, std::memory_order_release);
			while (!start_flag.load(std::memory_order_acquire)) {
			}

			uint32_t type_id   = 0;
			size_t	 recv_size = 0;

			const std::byte *ptr = consumer.acquire_rx_blocking(type_id, recv_size, 1);
			ASSERT_NE(ptr, nullptr);
			EXPECT_EQ(type_id, 99);
			EXPECT_TRUE(consumer.commit_rx());
		});

		while (!consumer_ready.load(std::memory_order_acquire)) {
		}
		start_flag.store(true, std::memory_order_release);

		std::this_thread::sleep_for(std::chrono::milliseconds(50));

		std::byte *ptr = producer.acquire_tx(16);
		ASSERT_NE(ptr, nullptr);
		EXPECT_TRUE(producer.commit_tx(16, 99));
		producer.flush();

		t_cons.join();
	}
} // namespace tachyon::core::test
