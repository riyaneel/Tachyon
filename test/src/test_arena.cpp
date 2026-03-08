#include <cstring>
#include <numeric>
#include <optional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#if defined(__x86_64__)
#include <xmmintrin.h>
#endif

#include <tachyon/arena.hpp>
#include <tachyon/shm.hpp>

namespace tachyon::core::test {

	class ArenaTest : public ::testing::Test {
	protected:
		const std::string			test_name		  = "tachyon_test_arena";
		const size_t				arena_capacity	  = 4096;
		const size_t				required_shm_size = 384 + arena_capacity;
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

	TEST_F(ArenaTest, SinglePushPop) {
		auto producer = Arena::format(shm_owner->data(), arena_capacity).value();
		auto consumer = Arena::attach(shm_owner->data()).value();

		const std::string msg = "HFT_Tachyon_Speed";
		const std::span	  send_data(reinterpret_cast<const std::byte *>(msg.data()), msg.size());
		EXPECT_TRUE(producer.try_push(send_data));

		producer.flush();
		std::byte recv_buf[128];
		size_t	  recv_size = 0;
		EXPECT_TRUE(consumer.try_pop(recv_buf, recv_size));

		consumer.flush();
		EXPECT_EQ(recv_size, msg.size());
		const std::string recv_msg(reinterpret_cast<const char *>(recv_buf), recv_size);
		EXPECT_EQ(msg, recv_msg);
	}

	TEST_F(ArenaTest, WrapAroundSplitMemcpy) {
		auto producer = Arena::format(shm_owner->data(), arena_capacity).value();
		auto consumer = Arena::attach(shm_owner->data()).value();

		std::vector dummy_pad(4000, std::byte{0x01});
		EXPECT_TRUE(producer.try_push(dummy_pad));
		producer.flush();

		size_t				   recv_size = 0;
		std::vector<std::byte> recv_pad(4000);
		EXPECT_TRUE(consumer.try_pop(recv_pad, recv_size));
		consumer.flush();

		std::string wrap_msg(200, 'X');
		wrap_msg[0]	  = 'A';
		wrap_msg[199] = 'Z';

		const std::span send_data(reinterpret_cast<const std::byte *>(wrap_msg.data()), wrap_msg.size());
		EXPECT_TRUE(producer.try_push(send_data));
		producer.flush();

		std::vector<std::byte> recv_wrap(256);
		EXPECT_TRUE(consumer.try_pop(recv_wrap, recv_size));
		consumer.flush();

		EXPECT_EQ(recv_size, 200);

		const std::string recv_str(reinterpret_cast<const char *>(recv_wrap.data()), recv_size);
		EXPECT_EQ(recv_str.front(), 'A');
		EXPECT_EQ(recv_str.back(), 'Z');
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
				const std::span payload(reinterpret_cast<const std::byte *>(&i), sizeof(i));
				while (!producer.try_push(payload)) {
#if defined(__x86_64__)
					_mm_pause();
#endif
				}
			}
			producer.flush();
		});

		std::thread t_cons([&]() {
			while (!start_flag.load(std::memory_order_acquire)) {
			}

			for (size_t i = 0; i < ITERATIONS; ++i) {
				size_t	  expected_val = 0;
				size_t	  recv_size	   = 0;
				std::byte buf[sizeof(size_t)];

				while (!consumer.try_pop(buf, recv_size)) {
#if defined(__x86_64__)
					_mm_pause();
#endif
				}

				EXPECT_EQ(recv_size, sizeof(size_t));
				std::memcpy(&expected_val, buf, sizeof(size_t));
				ASSERT_EQ(expected_val, i);
			}
			consumer.flush();
		});

		start_flag.store(true, std::memory_order_release);
		t_prod.join();
		t_cons.join();
	}
} // namespace tachyon::core::test
