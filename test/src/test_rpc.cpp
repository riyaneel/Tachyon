#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <thread>

#include <pthread.h>

#include <gtest/gtest.h>

#include <tachyon.h>

namespace tachyon::core::test {
	static constexpr size_t	  CAP  = 1u << 16;
	static constexpr uint32_t SPIN = 10'000;

	static std::string sock(const char *name) {
		return std::string("/tmp/tachyon_rpc_") + name + ".sock";
	}

	static tachyon_rpc_bus_t *rpc_retry(const char *path) {
		tachyon_rpc_bus_t *r = nullptr;
		for (int i = 0; i < 200; ++i) {
			if (tachyon_rpc_connect(path, &r) == TACHYON_SUCCESS)
				return r;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return nullptr;
	}

	static void sigusr1_noop(int) {}

	TEST(RpcBus, Handshake) {
		const auto		   path		= sock("handshake");
		tachyon_rpc_bus_t *listener = nullptr;
		std::thread		   t([&] { tachyon_rpc_listen(path.c_str(), CAP, CAP * 2, &listener); });
		tachyon_rpc_bus_t *connector = rpc_retry(path.c_str());
		t.join();

		ASSERT_NE(listener, nullptr);
		ASSERT_NE(connector, nullptr);
		EXPECT_EQ(tachyon_rpc_get_state(listener), TACHYON_STATE_READY);
		EXPECT_EQ(tachyon_rpc_get_state(connector), TACHYON_STATE_READY);

		tachyon_rpc_destroy(listener);
		tachyon_rpc_destroy(connector);
	}

	TEST(RpcBus, SpscConnectorVsRpcListener) {
		const auto		   path		= sock("spsc_mismatch");
		tachyon_rpc_bus_t *listener = nullptr;
		std::thread		   t([&] { tachyon_rpc_listen(path.c_str(), CAP, CAP, &listener); });

		std::this_thread::sleep_for(std::chrono::milliseconds(20));

		tachyon_bus_t  *spsc = nullptr;
		tachyon_error_t err	 = TACHYON_ERR_NETWORK;
		for (int i = 0; i < 200 && err == TACHYON_ERR_NETWORK; ++i) {
			err = tachyon_bus_connect(path.c_str(), &spsc);
			if (err == TACHYON_ERR_NETWORK)
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		t.join();

		EXPECT_EQ(err, TACHYON_ERR_ABI_MISMATCH);
		EXPECT_EQ(spsc, nullptr);
		if (listener)
			tachyon_rpc_destroy(listener);
	}

	TEST(RpcBus, Roundtrip) {
		const auto		   path		= sock("roundtrip");
		tachyon_rpc_bus_t *listener = nullptr;
		std::thread		   t([&] { tachyon_rpc_listen(path.c_str(), CAP, CAP, &listener); });
		tachyon_rpc_bus_t *caller = rpc_retry(path.c_str());
		t.join();
		ASSERT_NE(listener, nullptr);
		ASSERT_NE(caller, nullptr);
		tachyon_rpc_set_polling_mode(listener, 1);
		tachyon_rpc_set_polling_mode(caller, 1);

		std::thread callee([&] {
			uint64_t	cid = 0;
			uint32_t	mt	= 0;
			size_t		sz	= 0;
			const void *req = tachyon_rpc_serve(listener, &cid, &mt, &sz, SPIN);
			ASSERT_NE(req, nullptr);
			EXPECT_GT(cid, 0u);
			EXPECT_EQ(mt, 7u);
			EXPECT_EQ(sz, 4u);
			uint32_t v;
			std::memcpy(&v, req, 4);
			EXPECT_EQ(v, 0xDEADBEEFu);
			tachyon_rpc_commit_serve(listener);
			const uint32_t reply = 0xCAFEBABEu;
			EXPECT_EQ(tachyon_rpc_reply(listener, cid, &reply, 4, 9), TACHYON_SUCCESS);
		});

		constexpr uint32_t payload = 0xDEADBEEFu;
		uint64_t		   out_cid = 0;
		ASSERT_EQ(tachyon_rpc_call(caller, &payload, 4, 7, &out_cid), TACHYON_SUCCESS);
		EXPECT_GT(out_cid, 0u);

		size_t		sz	 = 0;
		uint32_t	mt	 = 0;
		const void *resp = tachyon_rpc_wait(caller, out_cid, &sz, &mt, SPIN);
		ASSERT_NE(resp, nullptr);
		EXPECT_EQ(sz, 4u);
		EXPECT_EQ(mt, 9u);
		uint32_t resp_val;
		std::memcpy(&resp_val, resp, 4);
		EXPECT_EQ(resp_val, 0xCAFEBABEu);
		tachyon_rpc_commit_rx(caller);

		callee.join();
		tachyon_rpc_destroy(listener);
		tachyon_rpc_destroy(caller);
	}

	TEST(RpcBus, CorrelationOrdering) {
		const auto		   path		= sock("ordering");
		tachyon_rpc_bus_t *listener = nullptr;
		std::thread		   t([&] { tachyon_rpc_listen(path.c_str(), CAP, CAP, &listener); });
		tachyon_rpc_bus_t *caller = rpc_retry(path.c_str());
		t.join();
		ASSERT_NE(listener, nullptr);
		ASSERT_NE(caller, nullptr);
		tachyon_rpc_set_polling_mode(listener, 1);
		tachyon_rpc_set_polling_mode(caller, 1);

		constexpr int N = 8;

		std::thread callee([&] {
			for (int i = 0; i < N; ++i) {
				uint64_t	cid = 0;
				uint32_t	mt	= 0;
				size_t		sz	= 0;
				const void *req = tachyon_rpc_serve(listener, &cid, &mt, &sz, SPIN);
				ASSERT_NE(req, nullptr);
				uint32_t v;
				std::memcpy(&v, req, 4);
				tachyon_rpc_commit_serve(listener);
				EXPECT_EQ(tachyon_rpc_reply(listener, cid, &v, 4, 1), TACHYON_SUCCESS);
			}
		});

		uint64_t cids[N];
		uint32_t sent[N];
		for (int i = 0; i < N; ++i) {
			sent[i] = static_cast<uint32_t>(i * 100);
			ASSERT_EQ(tachyon_rpc_call(caller, &sent[i], 4, 1, &cids[i]), TACHYON_SUCCESS);
			EXPECT_EQ(cids[i], static_cast<uint64_t>(i + 1)); // counter starts at 1
		}

		for (int i = 0; i < N; ++i) {
			size_t		sz	 = 0;
			uint32_t	mt	 = 0;
			const void *resp = tachyon_rpc_wait(caller, cids[i], &sz, &mt, SPIN);
			ASSERT_NE(resp, nullptr);
			uint32_t v;
			std::memcpy(&v, resp, 4);
			EXPECT_EQ(v, sent[i]);
			tachyon_rpc_commit_rx(caller);
		}

		callee.join();
		tachyon_rpc_destroy(listener);
		tachyon_rpc_destroy(caller);
	}

	TEST(RpcBus, ReplyCorrelationZeroRejected) {
		const auto		   path		= sock("cid_zero");
		tachyon_rpc_bus_t *listener = nullptr;
		std::thread		   t([&] { tachyon_rpc_listen(path.c_str(), CAP, CAP, &listener); });
		tachyon_rpc_bus_t *caller = rpc_retry(path.c_str());
		t.join();
		ASSERT_NE(listener, nullptr);
		ASSERT_NE(caller, nullptr);

		const uint32_t payload = 42u;
		EXPECT_EQ(tachyon_rpc_reply(listener, 0, &payload, 4, 1), TACHYON_ERR_INVALID_SZ);

		tachyon_rpc_destroy(listener);
		tachyon_rpc_destroy(caller);
	}

	TEST(RpcBus, EintrOnWait) {
		const auto		   path		= sock("eintr_wait");
		tachyon_rpc_bus_t *listener = nullptr;
		std::thread		   t([&] { tachyon_rpc_listen(path.c_str(), CAP, CAP, &listener); });
		tachyon_rpc_bus_t *caller = rpc_retry(path.c_str());
		t.join();
		ASSERT_NE(listener, nullptr);
		ASSERT_NE(caller, nullptr);

		struct sigaction sa{};
		sa.sa_handler = sigusr1_noop;
		sigaction(SIGUSR1, &sa, nullptr);

		const uint32_t payload = 1u;
		uint64_t	   out_cid = 0;
		ASSERT_EQ(tachyon_rpc_call(caller, &payload, 4, 1, &out_cid), TACHYON_SUCCESS);

		std::atomic<pthread_t>	  tid{0};
		std::atomic<const void *> result{reinterpret_cast<const void *>(1)};

		std::thread waiter([&] {
			tid.store(pthread_self(), std::memory_order_release);
			size_t	 sz = 0;
			uint32_t mt = 0;
			// spin_threshold=0 -> enters futex immediately
			result.store(tachyon_rpc_wait(caller, out_cid, &sz, &mt, 0));
		});

		while (tid.load(std::memory_order_acquire) == 0)
			std::this_thread::yield();
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
		pthread_kill(tid.load(std::memory_order_acquire), SIGUSR1);
		waiter.join();

		EXPECT_EQ(result.load(), nullptr);

		tachyon_rpc_destroy(listener);
		tachyon_rpc_destroy(caller);
	}

	TEST(RpcBus, EintrOnServe) {
		const auto		   path		= sock("eintr_serve");
		tachyon_rpc_bus_t *listener = nullptr;
		std::thread		   t([&] { tachyon_rpc_listen(path.c_str(), CAP, CAP, &listener); });
		tachyon_rpc_bus_t *caller = rpc_retry(path.c_str());
		t.join();
		ASSERT_NE(listener, nullptr);
		ASSERT_NE(caller, nullptr);

		struct sigaction sa{};
		sa.sa_handler = sigusr1_noop;
		sigaction(SIGUSR1, &sa, nullptr);

		std::atomic<pthread_t>	  tid{0};
		std::atomic<const void *> result{reinterpret_cast<const void *>(1)};

		std::thread server([&] {
			tid.store(pthread_self(), std::memory_order_release);
			uint64_t cid = 0;
			uint32_t mt	 = 0;
			size_t	 sz	 = 0;
			result.store(tachyon_rpc_serve(listener, &cid, &mt, &sz, 0));
		});

		while (tid.load(std::memory_order_acquire) == 0)
			std::this_thread::yield();
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
		pthread_kill(tid.load(std::memory_order_acquire), SIGUSR1);
		server.join();

		EXPECT_EQ(result.load(), nullptr);

		tachyon_rpc_destroy(listener);
		tachyon_rpc_destroy(caller);
	}

	TEST(RpcBus, FatalErrorRevIsolated) {
		const auto		   path		= sock("fatal_isolated");
		tachyon_rpc_bus_t *listener = nullptr;
		std::thread		   t([&] { tachyon_rpc_listen(path.c_str(), CAP, CAP, &listener); });
		tachyon_rpc_bus_t *caller = rpc_retry(path.c_str());
		t.join();
		ASSERT_NE(listener, nullptr);
		ASSERT_NE(caller, nullptr);
		tachyon_rpc_set_polling_mode(listener, 1);
		tachyon_rpc_set_polling_mode(caller, 1);

		const uint32_t payload = 99u;
		uint64_t	   cid	   = 0;
		ASSERT_EQ(tachyon_rpc_call(caller, &payload, 4, 1, &cid), TACHYON_SUCCESS);

		{
			uint64_t	recv_cid = 0;
			uint32_t	mt		 = 0;
			size_t		sz		 = 0;
			const void *req		 = tachyon_rpc_serve(listener, &recv_cid, &mt, &sz, SPIN);
			ASSERT_NE(req, nullptr);
			tachyon_rpc_commit_serve(listener);
			const uint32_t r = 0u;
			ASSERT_EQ(tachyon_rpc_reply(listener, recv_cid + 1, &r, 4, 1), TACHYON_SUCCESS);
		}

		{
			size_t		sz	 = 0;
			uint32_t	mt	 = 0;
			const void *resp = tachyon_rpc_wait(caller, cid, &sz, &mt, SPIN);
			EXPECT_EQ(resp, nullptr);
			EXPECT_EQ(tachyon_rpc_get_state(caller), TACHYON_STATE_FATAL_ERROR);
		}

		uint64_t cid2 = 0;
		ASSERT_EQ(tachyon_rpc_call(caller, &payload, 4, 2, &cid2), TACHYON_SUCCESS);
		{
			uint64_t	recv_cid = 0;
			uint32_t	mt		 = 0;
			size_t		sz		 = 0;
			const void *req		 = tachyon_rpc_serve(listener, &recv_cid, &mt, &sz, SPIN);
			ASSERT_NE(req, nullptr);
			EXPECT_EQ(mt, 2u);
			EXPECT_EQ(recv_cid, cid2);
			tachyon_rpc_commit_serve(listener);
		}

		tachyon_rpc_destroy(listener);
		tachyon_rpc_destroy(caller);
	}

	TEST(RpcBus, SetPollingMode) {
		const auto		   path		= sock("polling");
		tachyon_rpc_bus_t *listener = nullptr;
		std::thread		   t([&] { tachyon_rpc_listen(path.c_str(), CAP, CAP, &listener); });
		tachyon_rpc_bus_t *caller = rpc_retry(path.c_str());
		t.join();
		ASSERT_NE(listener, nullptr);
		ASSERT_NE(caller, nullptr);

		tachyon_rpc_set_polling_mode(listener, 1);
		tachyon_rpc_set_polling_mode(caller, 1);
		EXPECT_EQ(tachyon_rpc_get_state(listener), TACHYON_STATE_READY);
		EXPECT_EQ(tachyon_rpc_get_state(caller), TACHYON_STATE_READY);

		tachyon_rpc_set_polling_mode(listener, 0);
		tachyon_rpc_set_polling_mode(caller, 0);
		EXPECT_EQ(tachyon_rpc_get_state(listener), TACHYON_STATE_READY);
		EXPECT_EQ(tachyon_rpc_get_state(caller), TACHYON_STATE_READY);

		tachyon_rpc_destroy(listener);
		tachyon_rpc_destroy(caller);
	}
} // namespace tachyon::core::test
