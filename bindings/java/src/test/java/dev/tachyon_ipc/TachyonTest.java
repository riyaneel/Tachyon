package dev.tachyon_ipc;

import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.file.Path;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import static org.junit.jupiter.api.Assertions.*;

public class TachyonTest {
	private static final long DEFAULT_CAPACITY = 1024 * 1024; // 1 MB
	private static final int SPIN_THRESHOLD = 100_000;

	private static Thread spawnListener(String socketPath, AtomicReference<TachyonBus> ref, CountDownLatch latch) {
		return Thread.ofVirtual().start(() -> {
			TachyonBus bus = TachyonBus.listen(socketPath, DEFAULT_CAPACITY);
			ref.set(bus);
			latch.countDown();
		});
	}

	private static TachyonBus connectWithRetry(String socketPath) throws InterruptedException {
		for (int attempt = 0; attempt < 100; attempt++) {
			try {
				return TachyonBus.connect(socketPath);
			} catch (TachyonException e) {
				if (e.getCode() != 11) throw e;
				Thread.sleep(10);
			}
		}
		throw new RuntimeException("connect() failed after 100 retries (1s) on: " + socketPath);
	}

	@BeforeAll
	static void init() {
		NativeLoader.load();
	}

	@Test
	void testNativeLoader() {
		assertDoesNotThrow(NativeLoader::load, "NativeLoader should load the library without errors");
		assertDoesNotThrow(NativeLoader::load, "The second call to NativeLoader must be idempotent");
	}

	@Test
	void testBusLifecycle(@TempDir Path tempDir) throws Exception {
		String socketPath = tempDir.resolve("lifecycle.sock").toString();
		AtomicReference<TachyonBus> producerRef = new AtomicReference<>();
		CountDownLatch latch = new CountDownLatch(1);
		Thread listenThread = spawnListener(socketPath, producerRef, latch);

		try (TachyonBus consumer = connectWithRetry(socketPath)) {
			assertTrue(latch.await(2, TimeUnit.SECONDS), "listen() must unblock after connect()");
			try (TachyonBus producer = producerRef.get()) {
				assertNotNull(producer);
				assertNotNull(consumer);
			}
		}
		listenThread.join(2000);

		String doublePath = tempDir.resolve("double_close.sock").toString();
		AtomicReference<TachyonBus> busRef = new AtomicReference<>();
		CountDownLatch latch2 = new CountDownLatch(1);
		Thread t2 = spawnListener(doublePath, busRef, latch2);
		TachyonBus dummy = connectWithRetry(doublePath);
		latch2.await(2, TimeUnit.SECONDS);
		dummy.close();
		TachyonBus bus = busRef.get();
		bus.close();
		assertDoesNotThrow(bus::close, "A double close must not throw an exception");
		t2.join(2000);

		String badPath = tempDir.resolve("not_exist.sock").toString();
		assertThrows(RuntimeException.class, () -> TachyonBus.connect(badPath),
				"Connecting to a non-existent socket must fail natively");
	}

	@Test
	void testProducerConsumerRoundTrip(@TempDir Path tempDir) throws Exception {
		String socketPath = tempDir.resolve("roundtrip.sock").toString();
		int typeId = 42;
		long msgValue = 9876543210L;

		AtomicReference<TachyonBus> producerRef = new AtomicReference<>();
		CountDownLatch latch = new CountDownLatch(1);
		Thread listenThread = spawnListener(socketPath, producerRef, latch);

		try (TachyonBus consumer = connectWithRetry(socketPath)) {
			assertTrue(latch.await(2, TimeUnit.SECONDS));
			try (TachyonBus producer = producerRef.get()) {

				try (TxGuard tx = producer.acquireTx(8)) {
					assertNotNull(tx, "The buffer should not be full");
					tx.getData().set(ValueLayout.JAVA_LONG, 0, msgValue);
					tx.commit(8, typeId);
				}

				try (RxGuard rx = consumer.acquireRx(SPIN_THRESHOLD)) {
					assertNotNull(rx, "The message should be received");
					assertEquals(typeId, rx.getTypeId(), "The TypeID must match");
					assertEquals(8, rx.getActualSize(), "The size must match");
					long receivedValue = rx.getData().get(ValueLayout.JAVA_LONG, 0);
					assertEquals(msgValue, receivedValue, "The payload (byte-exact) must match");
					rx.commit();
				}
			}
		}
		listenThread.join(2000);
	}

	@Test
	void testZeroCopyTxAndRx(@TempDir Path tempDir) throws Exception {
		String socketPath = tempDir.resolve("zerocopy.sock").toString();
		String testString = "Hello Zero-Copy FFM!";
		byte[] stringBytes = testString.getBytes();
		int size = stringBytes.length;

		AtomicReference<TachyonBus> producerRef = new AtomicReference<>();
		CountDownLatch latch = new CountDownLatch(1);
		Thread listenThread = spawnListener(socketPath, producerRef, latch);

		try (TachyonBus consumer = connectWithRetry(socketPath)) {
			assertTrue(latch.await(2, TimeUnit.SECONDS));
			try (TachyonBus producer = producerRef.get()) {

				try (TxGuard tx = producer.acquireTx(size)) {
					MemorySegment.copy(stringBytes, 0, tx.getData(), ValueLayout.JAVA_BYTE, 0L, size);
					tx.commit(size, 1);
				}

				try (RxGuard rx = consumer.acquireRx(SPIN_THRESHOLD)) {
					assertNotNull(rx);
					int actualSizeInt = (int) rx.getActualSize();
					byte[] receivedBytes = new byte[actualSizeInt];
					MemorySegment.copy(rx.getData(), ValueLayout.JAVA_BYTE, 0L, receivedBytes, 0, actualSizeInt);
					assertEquals(testString, new String(receivedBytes),
							"The string read via MemorySegment must be identical");
				}
			}
		}
		listenThread.join(2000);
	}

	@Test
	void testRollback(@TempDir Path tempDir) throws Exception {
		String socketPath = tempDir.resolve("rollback.sock").toString();

		AtomicReference<TachyonBus> producerRef = new AtomicReference<>();
		CountDownLatch latch = new CountDownLatch(1);
		Thread listenThread = spawnListener(socketPath, producerRef, latch);

		try (TachyonBus consumer = connectWithRetry(socketPath)) {
			assertTrue(latch.await(2, TimeUnit.SECONDS));
			try (TachyonBus producer = producerRef.get()) {

				try (TxGuard tx = producer.acquireTx(8)) {
					tx.getData().set(ValueLayout.JAVA_LONG, 0, 1111L);
					tx.rollback();
				}

				try (TxGuard tx = producer.acquireTx(8)) {
					tx.getData().set(ValueLayout.JAVA_LONG, 0, 2222L);
					tx.commit(8, 99);
				}

				try (RxGuard rx = consumer.acquireRx(SPIN_THRESHOLD)) {
					assertNotNull(rx);
					long val = rx.getData().get(ValueLayout.JAVA_LONG, 0);
					assertEquals(2222L, val, "The consumer must only see the post-rollback message");
					assertEquals(99, rx.getTypeId());
				}
			}
		}
		listenThread.join(2000);
	}

	@Test
	void testBatch(@TempDir Path tempDir) throws Exception {
		String socketPath = tempDir.resolve("batch.sock").toString();
		int batchSize = 10;

		AtomicReference<TachyonBus> producerRef = new AtomicReference<>();
		CountDownLatch latch = new CountDownLatch(1);
		Thread listenThread = spawnListener(socketPath, producerRef, latch);

		try (TachyonBus consumer = connectWithRetry(socketPath)) {
			assertTrue(latch.await(2, TimeUnit.SECONDS));
			try (TachyonBus producer = producerRef.get()) {
				for (int i = 0; i < batchSize; i++) {
					try (TxGuard tx = producer.acquireTx(4)) {
						tx.getData().set(ValueLayout.JAVA_INT, 0, i);
						tx.commitUnflushed(4, 100 + i);
					}
				}

				producer.flush();

				int count = 0;
				try (RxBatchGuard batch = consumer.drainBatch(batchSize, SPIN_THRESHOLD)) {
					assertEquals(batchSize, batch.getCount(), "The batch must contain all messages");
					for (RxMsgView view : batch) {
						assertEquals(100 + count, view.getTypeId());
						int val = view.getData().get(ValueLayout.JAVA_INT, 0);
						assertEquals(count, val, "The messages must be in chronological order");
						count++;
					}
					batch.commit();
				}
				assertEquals(batchSize, count, "The iterator must have traversed all elements");
			}
		}
		listenThread.join(2000);
	}

	@Test
	void testTypeIdEncoding() {
		assertEquals(42, TypeId.of(0, 42));
		assertEquals(0,  TypeId.routeId(42));
		assertEquals(42, TypeId.msgType(42));

		int id = TypeId.of(1, 99);
		assertEquals(1,  TypeId.routeId(id));
		assertEquals(99, TypeId.msgType(id));

		assertEquals(0, TypeId.of(0, 0));
	}

	@Test
	void testTypeIdRoundTripOverBus(@TempDir Path tempDir) throws Exception {
		String socketPath = tempDir.resolve("typeid_rt.sock").toString();

		AtomicReference<TachyonBus> producerRef = new AtomicReference<>();
		CountDownLatch latch = new CountDownLatch(1);
		Thread listenThread = spawnListener(socketPath, producerRef, latch);

		try (TachyonBus consumer = connectWithRetry(socketPath)) {
			assertTrue(latch.await(2, TimeUnit.SECONDS));
			try (TachyonBus producer = producerRef.get()) {

				try (TxGuard tx = producer.acquireTx(4)) {
					tx.getData().set(ValueLayout.JAVA_INT, 0, 0);
					tx.commit(4, TypeId.of(0, 42));
				}

				try (RxGuard rx = consumer.acquireRx(SPIN_THRESHOLD)) {
					assertNotNull(rx);
					assertEquals(0,  TypeId.routeId(rx.getTypeId()), "routeId must be 0");
					assertEquals(42, TypeId.msgType(rx.getTypeId()), "msgType must be 42");
				}
			}
		}
		listenThread.join(2000);
	}

	@Test
	void testAbiMismatch() {
		TachyonException ex = assertThrows(AbiMismatchException.class, () -> {
			throw TachyonException.fromCode(14);
		});
		assertTrue(ex instanceof AbiMismatchException, "Must be an instance of AbiMismatchException");
		assertEquals(14, ex.getCode(), "The error code must be 14");
	}
}
