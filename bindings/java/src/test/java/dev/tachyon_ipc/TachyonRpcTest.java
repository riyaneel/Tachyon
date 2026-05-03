package dev.tachyon_ipc;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import static org.junit.jupiter.api.Assertions.*;

class TachyonRpcTest {
	private static final long CAP = 1L << 16;
	private static final int SPIN = 10_000;
	private static final int TIMEOUT = 2000; // ms for join

	private static Thread spawnCallee(String socketPath, AtomicReference<TachyonRpcBus> ref, CountDownLatch latch) {
		return Thread.ofVirtual().start(() -> {
			TachyonRpcBus bus = TachyonRpcBus.rpcListen(socketPath, CAP, CAP);
			ref.set(bus);
			latch.countDown();
		});
	}

	private static TachyonRpcBus connectWithRetry(String socketPath) throws InterruptedException {
		for (int attempt = 0; attempt < 100; attempt++) {
			try {
				return TachyonRpcBus.rpcConnect(socketPath);
			} catch (TachyonException e) {
				if (e.getCode() != 11) throw e; // TACHYON_ERR_NETWORK
				Thread.sleep(10);
			}
		}
		throw new RuntimeException("rpcConnect() failed after 100 retries on: " + socketPath);
	}

	@Test
	void testHandshake(@TempDir Path tempDir) throws Exception {
		String sock = tempDir.resolve("handshake.sock").toString();
		AtomicReference<TachyonRpcBus> calleeRef = new AtomicReference<>();
		CountDownLatch latch = new CountDownLatch(1);
		Thread t = spawnCallee(sock, calleeRef, latch);

		try (TachyonRpcBus caller = connectWithRetry(sock)) {
			assertTrue(latch.await(2, TimeUnit.SECONDS));
			try (TachyonRpcBus callee = calleeRef.get()) {
				assertNotNull(caller);
				assertNotNull(callee);
			}
		}
		t.join(TIMEOUT);
	}

	@Test
	void testRoundtrip(@TempDir Path tempDir) throws Exception {
		String sock = tempDir.resolve("roundtrip.sock").toString();
		AtomicReference<TachyonRpcBus> calleeRef = new AtomicReference<>();
		CountDownLatch latch = new CountDownLatch(1);
		Thread t = spawnCallee(sock, calleeRef, latch);
		byte[] request = "hello_rpc_java".getBytes();

		try (TachyonRpcBus caller = connectWithRetry(sock)) {
			assertTrue(latch.await(2, TimeUnit.SECONDS));
			try (TachyonRpcBus callee = calleeRef.get()) {

				Thread calleeSide = Thread.ofVirtual().start(() -> {
					try (RpcRxGuard rx = callee.serve(SPIN)) {
						long cid = rx.getCorrelationId();
						assertEquals(1, rx.getMsgType());
						assertArrayEquals(request, rx.getData());
						callee.reply(cid, reverse(rx.getData()), 2);
					}
				});

				long cid = caller.call(request, 1);
				assertTrue(cid > 0);

				try (RpcRxGuard rx = caller.waitResponse(cid, SPIN)) {
					assertEquals(2, rx.getMsgType());
					assertArrayEquals(reverse(request), rx.getData());
				}

				calleeSide.join(TIMEOUT);
			}
		}
		t.join(TIMEOUT);
	}

	@Test
	void testServeReplyTypeIdPreserved(@TempDir Path tempDir) throws Exception {
		String sock = tempDir.resolve("typeid.sock").toString();
		AtomicReference<TachyonRpcBus> calleeRef = new AtomicReference<>();
		CountDownLatch latch = new CountDownLatch(1);
		AtomicReference<Integer> receivedMsgType = new AtomicReference<>();
		Thread t = spawnCallee(sock, calleeRef, latch);

		try (TachyonRpcBus caller = connectWithRetry(sock)) {
			assertTrue(latch.await(2, TimeUnit.SECONDS));
			try (TachyonRpcBus callee = calleeRef.get()) {

				Thread calleeSide = Thread.ofVirtual().start(() -> {
					try (RpcRxGuard rx = callee.serve(SPIN)) {
						receivedMsgType.set(rx.getMsgType());
						callee.reply(rx.getCorrelationId(), new byte[]{0x01}, 99);
					}
				});

				long cid = caller.call(new byte[]{0x01}, 42);
				try (RpcRxGuard rx = caller.waitResponse(cid, SPIN)) {
					assertEquals(99, rx.getMsgType());
				}

				calleeSide.join(TIMEOUT);
				assertEquals(42, receivedMsgType.get());
			}
		}
		t.join(TIMEOUT);
	}

	@Test
	void testCorrelationIdMonotonic(@TempDir Path tempDir) throws Exception {
		String sock = tempDir.resolve("cid_mono.sock").toString();
		AtomicReference<TachyonRpcBus> calleeRef = new AtomicReference<>();
		CountDownLatch latch = new CountDownLatch(1);
		int n = 4;
		Thread t = spawnCallee(sock, calleeRef, latch);

		try (TachyonRpcBus caller = connectWithRetry(sock)) {
			assertTrue(latch.await(2, TimeUnit.SECONDS));
			try (TachyonRpcBus callee = calleeRef.get()) {

				Thread calleeSide = Thread.ofVirtual().start(() -> {
					for (int i = 0; i < n; i++) {
						try (RpcRxGuard rx = callee.serve(SPIN)) {
							callee.reply(rx.getCorrelationId(), new byte[]{0x01}, 0);
						}
					}
				});

				List<Long> cids = new ArrayList<>();
				for (int i = 0; i < n; i++) {
					long cid = caller.call(new byte[]{0x01}, 0);
					cids.add(cid);
					try (RpcRxGuard rx = caller.waitResponse(cid, SPIN)) {
						assertTrue(rx.getCorrelationId() > 0);
					}
				}

				calleeSide.join(TIMEOUT);

				for (int i = 0; i < n - 1; i++)
					assertEquals(cids.get(i) + 1, cids.get(i + 1));
				cids.forEach(cid -> assertTrue(cid > 0));
			}
		}
		t.join(TIMEOUT);
	}

	@Test
	void testNInflightOrdered(@TempDir Path tempDir) throws Exception {
		String sock = tempDir.resolve("inflight.sock").toString();
		AtomicReference<TachyonRpcBus> calleeRef = new AtomicReference<>();
		CountDownLatch latch = new CountDownLatch(1);
		int n = 8;
		int[] sent = new int[n];
		for (int i = 0; i < n; i++) sent[i] = i * 100;
		Thread t = spawnCallee(sock, calleeRef, latch);

		try (TachyonRpcBus caller = connectWithRetry(sock)) {
			assertTrue(latch.await(2, TimeUnit.SECONDS));
			try (TachyonRpcBus callee = calleeRef.get()) {
				callee.setPollingMode(1);
				caller.setPollingMode(1);

				Thread calleeSide = Thread.ofVirtual().start(() -> {
					for (int i = 0; i < n; i++) {
						try (RpcRxGuard rx = callee.serve(Integer.MAX_VALUE)) {
							int val = ByteBuffer.wrap(rx.getData())
									.order(ByteOrder.LITTLE_ENDIAN).getInt();
							callee.reply(rx.getCorrelationId(), intToBytes(val), 0);
						}
					}
				});

				long[] cids = new long[n];
				for (int i = 0; i < n; i++)
					cids[i] = caller.call(intToBytes(sent[i]), 0);

				for (int i = 0; i < n; i++) {
					try (RpcRxGuard rx = caller.waitResponse(cids[i], Integer.MAX_VALUE)) {
						int val = ByteBuffer.wrap(rx.getData())
								.order(ByteOrder.LITTLE_ENDIAN).getInt();
						assertEquals(sent[i], val);
					}
				}

				calleeSide.join(TIMEOUT);
			}
		}
		t.join(TIMEOUT);
	}

	@Test
	void testReplyCorrelationIdZeroRejected(@TempDir Path tempDir) throws Exception {
		String sock = tempDir.resolve("cid_zero.sock").toString();
		AtomicReference<TachyonRpcBus> calleeRef = new AtomicReference<>();
		CountDownLatch latch = new CountDownLatch(1);
		Thread t = spawnCallee(sock, calleeRef, latch);

		try (TachyonRpcBus caller = connectWithRetry(sock)) {
			assertTrue(latch.await(2, TimeUnit.SECONDS));
			try (TachyonRpcBus callee = calleeRef.get()) {
				assertThrows(TachyonException.class,
						() -> callee.reply(0L, new byte[]{0x01}, 1));
			}
		}
		t.join(TIMEOUT);
	}

	@Test
	void testSetPollingModeRoundtrip(@TempDir Path tempDir) throws Exception {
		String sock = tempDir.resolve("polling.sock").toString();
		AtomicReference<TachyonRpcBus> calleeRef = new AtomicReference<>();
		CountDownLatch latch = new CountDownLatch(1);
		Thread t = spawnCallee(sock, calleeRef, latch);

		try (TachyonRpcBus caller = connectWithRetry(sock)) {
			assertTrue(latch.await(2, TimeUnit.SECONDS));
			try (TachyonRpcBus callee = calleeRef.get()) {
				callee.setPollingMode(1);
				caller.setPollingMode(1);
				callee.setPollingMode(0);
				caller.setPollingMode(0);

				byte[] payload = new byte[]{0x42};

				Thread calleeSide = Thread.ofVirtual().start(() -> {
					try (RpcRxGuard rx = callee.serve(SPIN)) {
						callee.reply(rx.getCorrelationId(), payload, 0);
					}
				});

				long cid = caller.call(payload, 0);
				try (RpcRxGuard rx = caller.waitResponse(cid, SPIN)) {
					assertArrayEquals(payload, rx.getData());
				}

				calleeSide.join(TIMEOUT);
			}
		}
		t.join(TIMEOUT);
	}

	@Test
	void testGuardCommitIdempotent(@TempDir Path tempDir) throws Exception {
		String sock = tempDir.resolve("guard_commit.sock").toString();
		AtomicReference<TachyonRpcBus> calleeRef = new AtomicReference<>();
		CountDownLatch latch = new CountDownLatch(1);
		Thread t = spawnCallee(sock, calleeRef, latch);

		try (TachyonRpcBus caller = connectWithRetry(sock)) {
			assertTrue(latch.await(2, TimeUnit.SECONDS));
			try (TachyonRpcBus callee = calleeRef.get()) {

				Thread calleeSide = Thread.ofVirtual().start(() -> {
					try (RpcRxGuard rx = callee.serve(SPIN)) {
						callee.reply(rx.getCorrelationId(), new byte[]{0x01}, 0);
					}
				});

				long cid = caller.call(new byte[]{0x01}, 0);
				try (RpcRxGuard rx = caller.waitResponse(cid, SPIN)) {
					assertDoesNotThrow(rx::commit);
					assertDoesNotThrow(rx::commit); // idempotent
				}

				calleeSide.join(TIMEOUT);
			}
		}
		t.join(TIMEOUT);
	}

	@Test
	void testCloseIdempotent(@TempDir Path tempDir) throws Exception {
		String sock = tempDir.resolve("close.sock").toString();
		AtomicReference<TachyonRpcBus> calleeRef = new AtomicReference<>();
		CountDownLatch latch = new CountDownLatch(1);
		Thread t = spawnCallee(sock, calleeRef, latch);

		TachyonRpcBus caller = connectWithRetry(sock);
		assertTrue(latch.await(2, TimeUnit.SECONDS));
		TachyonRpcBus callee = calleeRef.get();

		assertDoesNotThrow(caller::close);
		assertDoesNotThrow(caller::close);
		assertDoesNotThrow(callee::close);
		assertDoesNotThrow(callee::close);

		t.join(TIMEOUT);
	}

	@Test
	void testStructPayload(@TempDir Path tempDir) throws Exception {
		String sock = tempDir.resolve("struct.sock").toString();
		AtomicReference<TachyonRpcBus> calleeRef = new AtomicReference<>();
		CountDownLatch latch = new CountDownLatch(1);
		Thread t = spawnCallee(sock, calleeRef, latch);

		int a = 0xDEAD, b = 0xBEEF;
		byte[] req = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN)
				.putInt(a).putInt(b).array();

		try (TachyonRpcBus caller = connectWithRetry(sock)) {
			assertTrue(latch.await(2, TimeUnit.SECONDS));
			try (TachyonRpcBus callee = calleeRef.get()) {

				Thread calleeSide = Thread.ofVirtual().start(() -> {
					try (RpcRxGuard rx = callee.serve(SPIN)) {
						ByteBuffer buf = ByteBuffer.wrap(rx.getData())
								.order(ByteOrder.LITTLE_ENDIAN);
						int x = buf.getInt(), y = buf.getInt();
						byte[] resp = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN)
								.putInt(x + y).putInt(x ^ y).array();
						callee.reply(rx.getCorrelationId(), resp, 1);
					}
				});

				long cid = caller.call(req, 1);
				try (RpcRxGuard rx = caller.waitResponse(cid, SPIN)) {
					ByteBuffer resp = ByteBuffer.wrap(rx.getData())
							.order(ByteOrder.LITTLE_ENDIAN);
					assertEquals(a + b, resp.getInt());
					assertEquals(a ^ b, resp.getInt());
				}

				calleeSide.join(TIMEOUT);
			}
		}
		t.join(TIMEOUT);
	}

	private static byte[] reverse(byte[] input) {
		byte[] out = new byte[input.length];
		for (int i = 0; i < input.length; i++) out[i] = input[input.length - 1 - i];
		return out;
	}

	private static byte[] intToBytes(int v) {
		return ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(v).array();
	}
}
