#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <pthread.h>
#include <unistd.h>
#endif // #if defined(__linux__)

#include <gtest/gtest.h>

#include <tachyon.h>
#include <tachyon.hpp>
#include <tachyon/star.hpp>

namespace tachyon::core::test {
	static constexpr size_t CAP = 1u << 16;

	static std::string sock(const char *name) {
		return std::string("/tmp/tachyon_star_") + name + ".sock";
	}

	struct BusPair {
		tachyon_bus_t *listener	 = nullptr;
		tachyon_bus_t *connector = nullptr;

		BusPair() = default;

		BusPair(const BusPair &) = delete;

		BusPair &operator=(const BusPair &) = delete;

		BusPair(BusPair &&o) noexcept
			: listener(std::exchange(o.listener, nullptr)), connector(std::exchange(o.connector, nullptr)) {}

		BusPair &operator=(BusPair &&o) noexcept {
			if (this != &o) {
				clean();
				listener  = std::exchange(o.listener, nullptr);
				connector = std::exchange(o.connector, nullptr);
			}

			return *this;
		}

		~BusPair() noexcept {
			clean();
		}

	private:
		TACHYON_INLINE void clean() const noexcept {
			if (listener) {
				tachyon_bus_destroy(listener);
			}

			if (connector) {
				tachyon_bus_destroy(connector);
			}
		}
	};

	static BusPair make_pair(const std::string &path) {
		BusPair		pair;
		std::thread t([&] { tachyon_bus_listen(path.c_str(), CAP, &pair.listener); });

		tachyon_error_t err = TACHYON_ERR_NETWORK;
		for (int i = 0; i < 200 && err != TACHYON_SUCCESS; ++i) {
			err = tachyon_bus_connect(path.c_str(), &pair.connector);
			if (err != TACHYON_SUCCESS) {
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}

		t.join();
		return pair;
	}

	static bool send_u8(tachyon_bus_t *producer, const uint16_t route_id, const uint8_t value) {
		void *ptr = tachyon_acquire_tx(producer, sizeof(uint8_t));
		if (!ptr) {
			return false;
		}

		*static_cast<uint8_t *>(ptr) = value;
		if (tachyon_commit_tx(producer, sizeof(uint8_t), TACHYON_TYPE_ID(route_id, 1)) != TACHYON_SUCCESS) {
			return false;
		}

		tachyon_flush(producer);
		return true;
	}

	class StarBusTest : public ::testing::Test {};

	TEST_F(StarBusTest, NullGuards) {
		tachyon_star_t *star = nullptr;

		EXPECT_EQ(tachyon_star_create(nullptr, 1, nullptr, &star), TACHYON_ERR_NULL_PTR);
		EXPECT_EQ(tachyon_star_create(reinterpret_cast<tachyon_bus_t **>(1), 0, nullptr, &star), TACHYON_ERR_NULL_PTR);
		EXPECT_EQ(
			tachyon_star_create(reinterpret_cast<tachyon_bus_t **>(1), 1, nullptr, nullptr), TACHYON_ERR_NULL_PTR
		);

		EXPECT_EQ(tachyon_star_poll(nullptr, nullptr, 0, 0, nullptr), 0u);
		EXPECT_EQ(tachyon_star_commit(nullptr), TACHYON_ERR_NULL_PTR);
		EXPECT_EQ(tachyon_star_acquire_tx(nullptr, 0, 0), nullptr);
		EXPECT_EQ(tachyon_star_commit_tx(nullptr, 0, 0, 0), TACHYON_ERR_NULL_PTR);
		EXPECT_EQ(tachyon_star_rollback_tx(nullptr, 0), TACHYON_ERR_NULL_PTR);
		EXPECT_EQ(tachyon_star_get_state(nullptr, 0), TACHYON_STATE_UNKNOWN);
		EXPECT_EQ(tachyon_star_n_spokes(nullptr), 0u);
		tachyon_star_flush(nullptr, 0);
		tachyon_star_destroy(nullptr);
	}

	TEST_F(StarBusTest, SpokeIdxOutOfBounds) {
		const auto p = make_pair(sock("oob"));
		ASSERT_NE(p.listener, nullptr);
		ASSERT_NE(p.connector, nullptr);

		tachyon_bus_t *buses[] = {p.connector};

		const auto res = StarBus::create(buses, 1, nullptr);
		ASSERT_TRUE(res.has_value());
		EXPECT_EQ(res->acquire_tx(1, 8), nullptr);
		EXPECT_FALSE(res->commit_tx(1, 0, 0));
		EXPECT_FALSE(res->rollback_tx(1));
		EXPECT_EQ(res->get_state(1), TACHYON_STATE_UNKNOWN);

		tachyon_star_t *star = nullptr;
		ASSERT_EQ(tachyon_star_create(buses, 1, nullptr, &star), TACHYON_SUCCESS);
		EXPECT_EQ(tachyon_star_acquire_tx(star, 99, 8), nullptr);
		EXPECT_EQ(tachyon_star_commit_tx(star, 99, 0, 0), TACHYON_ERR_INVALID_SZ);
		EXPECT_EQ(tachyon_star_rollback_tx(star, 99), TACHYON_ERR_INVALID_SZ);
		EXPECT_EQ(tachyon_star_get_state(star, 99), TACHYON_STATE_UNKNOWN);
		tachyon_star_flush(star, 99);
		tachyon_star_destroy(star);
	}

	TEST_F(StarBusTest, NSpokesReturnsCorrectCount) {
		const auto p0 = make_pair(sock("ns0"));
		const auto p1 = make_pair(sock("ns1"));
		const auto p2 = make_pair(sock("ns2"));
		ASSERT_NE(p0.connector, nullptr);
		ASSERT_NE(p1.connector, nullptr);
		ASSERT_NE(p2.connector, nullptr);

		tachyon_bus_t *buses[] = {p0.connector, p1.connector, p2.connector};

		const auto res = StarBus::create(buses, 3, nullptr);
		ASSERT_TRUE(res.has_value());
		EXPECT_EQ(res->n_spokes(), 3u);

		tachyon_star_t *star = nullptr;
		ASSERT_EQ(tachyon_star_create(buses, 2, nullptr, &star), TACHYON_SUCCESS);
		EXPECT_EQ(tachyon_star_n_spokes(star), 2u);
		tachyon_star_destroy(star);
	}

	TEST_F(StarBusTest, RoundRobinDrainsAll) {
		constexpr size_t N_SPOKES = 4;
		constexpr size_t N_MSGS	  = 8;

		BusPair pairs[N_SPOKES];
		for (size_t i = 0; i < N_SPOKES; ++i) {
			pairs[i] = make_pair(sock(("rr" + std::to_string(i)).c_str()));
			ASSERT_NE(pairs[i].listener, nullptr) << "spoke " << i;
			ASSERT_NE(pairs[i].connector, nullptr) << "spoke " << i;
		}

		for (size_t i = 0; i < N_SPOKES; ++i) {
			for (size_t m = 0; m < N_MSGS; ++m) {
				ASSERT_TRUE(send_u8(pairs[i].listener, static_cast<uint16_t>(i), static_cast<uint8_t>(m)));
			}
		}

		tachyon_bus_t *buses[N_SPOKES];
		for (size_t i = 0; i < N_SPOKES; ++i) {
			buses[i] = pairs[i].connector;
		}

		auto res = StarBus::create(buses, N_SPOKES, nullptr);
		ASSERT_TRUE(res.has_value());

		constexpr size_t   TOTAL = N_SPOKES * N_MSGS;
		tachyon_msg_view_t views[TOTAL];
		size_t			   indices[TOTAL];
		const size_t	   n = res->poll(views, TOTAL, 5'000, indices);
		EXPECT_EQ(n, TOTAL);

		size_t per_spoke[N_SPOKES] = {};
		for (size_t i = 0; i < n; ++i) {
			const size_t spoke = indices[i];
			ASSERT_LT(spoke, N_SPOKES);
			EXPECT_EQ(TACHYON_ROUTE_ID(views[i].type_id), static_cast<uint16_t>(spoke));
			per_spoke[spoke]++;
		}

		for (size_t i = 0; i < N_SPOKES; ++i) {
			EXPECT_EQ(per_spoke[i], N_MSGS) << "spoke " << i;
		}

		EXPECT_TRUE(res->commit());
	}

	TEST_F(StarBusTest, BudgetUsRespected) {
		const auto p = make_pair(sock("bud"));
		ASSERT_NE(p.connector, nullptr);

		tachyon_bus_t *buses[] = {p.connector};
		auto		   res	   = StarBus::create(buses, 1, nullptr);
		ASSERT_TRUE(res.has_value());
		constexpr uint64_t BUDGET_US = 500;

		struct timespec ts_start, ts_end;
		clock_gettime(CLOCK_MONOTONIC, &ts_start);

		tachyon_msg_view_t views[64];
		const size_t	   n = res->poll(views, 64, BUDGET_US, nullptr);

		clock_gettime(CLOCK_MONOTONIC, &ts_end);
		EXPECT_EQ(n, 0u);

		const uint64_t elapsed_us =
			(static_cast<uint64_t>(ts_end.tv_sec - ts_start.tv_sec) * 1'000'000ULL) +
			(static_cast<uint64_t>(ts_end.tv_nsec) - static_cast<uint64_t>(ts_start.tv_nsec)) / 1'000ULL;
		EXPECT_LT(elapsed_us, BUDGET_US * 10)
			<< "poll overshot: elapsed=" << elapsed_us << " µs budget=" << BUDGET_US << " µs";
	}

	TEST_F(StarBusTest, FanOutAcquireCommitTx) {
		constexpr size_t N = 3;
		BusPair			 pairs[N];
		for (size_t i = 0; i < N; ++i) {
			pairs[i] = make_pair(sock(("tx" + std::to_string(i)).c_str()));
			ASSERT_NE(pairs[i].listener, nullptr) << "spoke " << i;
			ASSERT_NE(pairs[i].connector, nullptr) << "spoke " << i;
		}

		tachyon_bus_t *buses[N];
		for (size_t i = 0; i < N; ++i) {
			buses[i] = pairs[i].connector;
		}

		const auto res = StarBus::create(buses, N, nullptr);
		ASSERT_TRUE(res.has_value());

		for (size_t i = 0; i < N; ++i) {
			void *ptr = res->acquire_tx(i, sizeof(uint32_t));
			ASSERT_NE(ptr, nullptr) << "spoke " << i;
			*static_cast<uint32_t *>(ptr) = static_cast<uint32_t>(i * 100);
			EXPECT_TRUE(res->commit_tx(i, sizeof(uint32_t), TACHYON_TYPE_ID(0, 42)));
		}

		for (size_t i = 0; i < N; ++i) {
			uint32_t	type_id		= 0;
			size_t		actual_size = 0;
			const void *ptr			= tachyon_acquire_rx_spin(pairs[i].listener, &type_id, &actual_size, 100'000);
			ASSERT_NE(ptr, nullptr) << "spoke " << i;
			EXPECT_EQ(actual_size, sizeof(uint32_t));
			EXPECT_EQ(*static_cast<const uint32_t *>(ptr), static_cast<uint32_t>(i * 100));
			EXPECT_EQ(tachyon_commit_rx(pairs[i].listener), TACHYON_SUCCESS);
		}
	}

	TEST_F(StarBusTest, RollbackTxFreesSlot) {
		const auto p = make_pair(sock("rb"));
		ASSERT_NE(p.listener, nullptr);
		ASSERT_NE(p.connector, nullptr);

		tachyon_bus_t *buses[] = {p.connector};
		const auto	   res	   = StarBus::create(buses, 1, nullptr);
		ASSERT_TRUE(res.has_value());

		void *ptr = res->acquire_tx(0, sizeof(uint64_t));
		ASSERT_NE(ptr, nullptr);
		*static_cast<uint64_t *>(ptr) = 0xBADBADBADBADBADULL;
		EXPECT_TRUE(res->rollback_tx(0));

		ptr = res->acquire_tx(0, sizeof(uint64_t));
		ASSERT_NE(ptr, nullptr);
		*static_cast<uint64_t *>(ptr) = 0xDEADBEEFCAFEBABEULL;
		EXPECT_TRUE(res->commit_tx(0, sizeof(uint64_t), TACHYON_TYPE_ID(0, 1)));

		uint32_t	type_id		= 0;
		size_t		actual_size = 0;
		const void *rx			= tachyon_acquire_rx_spin(p.listener, &type_id, &actual_size, 100'000);
		ASSERT_NE(rx, nullptr);
		EXPECT_EQ(*static_cast<const uint64_t *>(rx), 0xDEADBEEFCAFEBABEULL);
		tachyon_commit_rx(p.listener);

		uint32_t	t2 = 0;
		size_t		s2 = 0;
		const void *r2 = tachyon_acquire_rx(p.listener, &t2, &s2);
		EXPECT_EQ(r2, nullptr);
	}

	TEST_F(StarBusTest, SpokeIndicesMapsCorrectly) {
		constexpr size_t N = 3;
		BusPair			 pairs[N];
		for (size_t i = 0; i < N; ++i) {
			pairs[i] = make_pair(sock(("idx" + std::to_string(i)).c_str()));
			ASSERT_NE(pairs[i].connector, nullptr) << "spoke " << i;
		}

		for (size_t i = 0; i < N; ++i) {
			ASSERT_TRUE(send_u8(pairs[i].listener, static_cast<uint16_t>(i), static_cast<uint8_t>(i)));
		}

		tachyon_bus_t *buses[N];
		for (size_t i = 0; i < N; ++i) {
			buses[i] = pairs[i].connector;
		}

		auto res = StarBus::create(buses, N, nullptr);
		ASSERT_TRUE(res.has_value());

		tachyon_msg_view_t views[N];
		size_t			   indices[N];
		const size_t	   total = res->poll(views, N, 5'000, indices);
		ASSERT_EQ(total, N);

		for (size_t i = 0; i < total; ++i) {
			EXPECT_EQ(indices[i], TACHYON_ROUTE_ID(views[i].type_id))
				<< "view " << i << ": spoke_index=" << indices[i] << " route_id=" << TACHYON_ROUTE_ID(views[i].type_id);
		}

		EXPECT_TRUE(res->commit());
	}

	TEST_F(StarBusTest, FatalErrorIsolation) {
		const auto p0 = make_pair(sock("fe0"));
		const auto p1 = make_pair(sock("fe1"));
		ASSERT_NE(p0.connector, nullptr);
		ASSERT_NE(p1.connector, nullptr);

		ASSERT_TRUE(send_u8(p1.listener, 1, 0xAB));

		tachyon_bus_t *buses[] = {p0.connector, p1.connector};
		auto		   res	   = StarBus::create(buses, 2, nullptr);
		ASSERT_TRUE(res.has_value());

		tachyon_msg_view_t views[8];
		size_t			   indices[8];
		const size_t	   total = res->poll(views, 8, 5'000, indices);
		ASSERT_GE(total, 1u);

		bool found = false;
		for (size_t i = 0; i < total; ++i) {
			if (indices[i] == 1) {
				EXPECT_EQ(TACHYON_ROUTE_ID(views[i].type_id), 1u);
				EXPECT_EQ(views[i].actual_size, sizeof(uint8_t));
				EXPECT_EQ(*static_cast<const uint8_t *>(views[i].ptr), uint8_t{0xAB});
				found = true;
			}
		}

		EXPECT_TRUE(found) << "message from spoke 1 not received";
		EXPECT_TRUE(res->commit());
	}

#if defined(__linux__)
	TEST_F(StarBusTest, NumaBindingApplied) {
		const auto p = make_pair(sock("numa"));
		ASSERT_NE(p.listener, nullptr);
		ASSERT_NE(p.connector, nullptr);

		tachyon_bus_t *buses[]	  = {p.connector};
		constexpr int  node_ids[] = {0};

		auto res = StarBus::create(buses, 1, node_ids);
		ASSERT_TRUE(res.has_value());

		ASSERT_TRUE(send_u8(p.listener, 0, 0x42));

		tachyon_msg_view_t view;
		size_t			   idx;
		const size_t	   n = res->poll(&view, 1, 5'000, &idx);

		ASSERT_EQ(n, 1u);
		EXPECT_EQ(*static_cast<const uint8_t *>(view.ptr), uint8_t{0x42});
		EXPECT_TRUE(res->commit());
	}
#endif // #if defined(__linux__)

	TEST_F(StarBusTest, CpuRelaxOncePerEmptyPass) {
		constexpr size_t N = 16;
		BusPair			 pairs[N];
		for (size_t i = 0; i < N; ++i) {
			pairs[i] = make_pair(sock(("cr" + std::to_string(i)).c_str()));
			ASSERT_NE(pairs[i].connector, nullptr) << "spoke " << i;
		}

		tachyon_bus_t *buses[N];
		for (size_t i = 0; i < N; ++i) {
			buses[i] = pairs[i].connector;
		}

		auto res = StarBus::create(buses, N, nullptr);
		ASSERT_TRUE(res.has_value());
		constexpr uint64_t BUDGET_US = 200;

		struct timespec ts_start, ts_end;
		clock_gettime(CLOCK_MONOTONIC, &ts_start);

		tachyon_msg_view_t views[1];
		(void)res->poll(views, 1, BUDGET_US, nullptr);
		clock_gettime(CLOCK_MONOTONIC, &ts_end);

		const uint64_t elapsed_us =
			(static_cast<uint64_t>(ts_end.tv_sec - ts_start.tv_sec) * 1'000'000ULL) +
			(static_cast<uint64_t>(ts_end.tv_nsec) - static_cast<uint64_t>(ts_start.tv_nsec)) / 1'000ULL;
		EXPECT_LT(elapsed_us, BUDGET_US * 10)
			<< N << " spokes, elapsed=" << elapsed_us << " µs budget=" << BUDGET_US << " µs";
	}

	TEST_F(StarBusTest, BusRefHeldAfterCallerDestroys) {
		auto p = make_pair(sock("ref"));
		ASSERT_NE(p.listener, nullptr);
		ASSERT_NE(p.connector, nullptr);

		tachyon_bus_t *raw = p.connector;
		p.connector		   = nullptr;

		tachyon_bus_t *buses[] = {raw};
		auto		   res	   = StarBus::create(buses, 1, nullptr);
		ASSERT_TRUE(res.has_value());

		tachyon_bus_destroy(raw);
		ASSERT_TRUE(send_u8(p.listener, 0, 0xCC));

		tachyon_msg_view_t view;
		const size_t	   n = res->poll(&view, 1, 5'000, nullptr);

		ASSERT_EQ(n, 1u);
		EXPECT_EQ(*static_cast<const uint8_t *>(view.ptr), uint8_t{0xCC});
		EXPECT_TRUE(res->commit());
	}
} // namespace tachyon::core::test
