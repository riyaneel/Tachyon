package dev.tachyon_ipc;

import org.openjdk.jmh.annotations.*;
import org.openjdk.jmh.infra.Blackhole;
import org.openjdk.jmh.runner.Runner;
import org.openjdk.jmh.runner.RunnerException;
import org.openjdk.jmh.runner.options.Options;
import org.openjdk.jmh.runner.options.OptionsBuilder;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

@BenchmarkMode({Mode.AverageTime, Mode.SampleTime})
@OutputTimeUnit(TimeUnit.NANOSECONDS)
@Warmup(iterations = 5, time = 1)
@Measurement(iterations = 10, time = 1)
@Fork(1)
@State(Scope.Benchmark)
public class BenchIpc {
	private static final long CAPACITY = 1 << 22; // 4 MB
	private static final int PAYLOAD_BYTES = 32;
	private static final int BENCH_SPIN = 50_000;
	private static final int BG_SPIN = 50_000;

	private static final String SOCK_THROUGHPUT = "/tmp/tachyon_jmh_throughput.sock";
	private static final String SOCK_PP_AB = "/tmp/tachyon_jmh_pp_ab.sock";
	private static final String SOCK_PP_BA = "/tmp/tachyon_jmh_pp_ba.sock";
	private static final String SOCK_BATCH = "/tmp/tachyon_jmh_batch.sock";

	private TachyonBus throughputProducer;
	private TachyonBus throughputConsumer;
	private Thread throughputDrainThread;
	private AtomicBoolean throughputRunning;

	private TachyonBus pingTx;
	private TachyonBus pingRx;
	private TachyonBus pongTx;
	private TachyonBus pongRx;
	private Thread pongThread;
	private AtomicBoolean pongRunning;

	private TachyonBus batchProducer;
	private TachyonBus batchConsumer;
	private Thread batchProducerThread;
	private AtomicBoolean batchRunning;

	private static TachyonBus connectWithRetry(String path) throws InterruptedException {
		for (int i = 0; i < 200; i++) {
			try {
				return TachyonBus.connect(path);
			} catch (TachyonException e) {
				if (e.getCode() != 11) throw e;
				Thread.sleep(10);
			}
		}
		throw new RuntimeException("connect() timed out: " + path);
	}

	private static Thread spawnListener(String path,
	                                    AtomicReference<TachyonBus> ref,
	                                    CountDownLatch latch) {
		return Thread.ofVirtual().start(() -> {
			ref.set(TachyonBus.listen(path, CAPACITY));
			latch.countDown();
		});
	}

	@Setup(Level.Trial)
	public void setup() throws Exception {
		NativeLoader.load();

		{
			AtomicReference<TachyonBus> ref = new AtomicReference<>();
			CountDownLatch latch = new CountDownLatch(1);
			spawnListener(SOCK_THROUGHPUT, ref, latch);
			throughputConsumer = connectWithRetry(SOCK_THROUGHPUT);
			latch.await(2, TimeUnit.SECONDS);
			throughputProducer = ref.get();
			throughputProducer.setPollingMode(1);
			throughputConsumer.setPollingMode(1);

			throughputRunning = new AtomicBoolean(true);
			throughputDrainThread = Thread.ofPlatform().daemon(true).start(() -> {
				while (throughputRunning.get()) {
					try (RxBatchGuard batch = throughputConsumer.drainBatch(64, BG_SPIN)) {
						batch.commit();
					} catch (Exception ignored) {
					}
				}
			});
		}

		{
			AtomicReference<TachyonBus> abRef = new AtomicReference<>();
			AtomicReference<TachyonBus> baRef = new AtomicReference<>();
			CountDownLatch abLatch = new CountDownLatch(1);
			CountDownLatch baLatch = new CountDownLatch(1);

			spawnListener(SOCK_PP_AB, abRef, abLatch);
			pingTx = connectWithRetry(SOCK_PP_AB);
			abLatch.await(2, TimeUnit.SECONDS);
			pongRx = abRef.get();

			spawnListener(SOCK_PP_BA, baRef, baLatch);
			pingRx = connectWithRetry(SOCK_PP_BA);
			baLatch.await(2, TimeUnit.SECONDS);
			pongTx = baRef.get();

			for (TachyonBus b : new TachyonBus[]{pingTx, pingRx, pongTx, pongRx})
				b.setPollingMode(1);

			pongRunning = new AtomicBoolean(true);
			pongThread = Thread.ofVirtual().start(() -> {
				while (pongRunning.get()) {
					RxGuard rx = pongRx.acquireRx(BG_SPIN);
					if (rx == null) continue;
					try {
						try (TxGuard tx = pongTx.acquireTx(PAYLOAD_BYTES)) {
							tx.getData().copyFrom(rx.getData());
							tx.commit(PAYLOAD_BYTES, rx.getTypeId());
						}
					} finally {
						rx.close();
					}
				}
			});

			for (int i = 0; i < 1_000; i++) {
				try (TxGuard tx = pingTx.acquireTx(PAYLOAD_BYTES)) {
					tx.commit(PAYLOAD_BYTES, 1);
				}
				RxGuard rx = pingRx.acquireRx(BENCH_SPIN);
				if (rx != null) rx.close();
			}
		}

		{
			AtomicReference<TachyonBus> ref = new AtomicReference<>();
			CountDownLatch latch = new CountDownLatch(1);
			spawnListener(SOCK_BATCH, ref, latch);
			batchConsumer = connectWithRetry(SOCK_BATCH);
			latch.await(2, TimeUnit.SECONDS);
			batchProducer = ref.get();
			batchProducer.setPollingMode(1);
			batchConsumer.setPollingMode(1);

			batchRunning = new AtomicBoolean(true);
			batchProducerThread = Thread.ofVirtual().start(() -> {
				while (batchRunning.get()) {
					try {
						for (int i = 0; i < 64; i++) {
							try (TxGuard tx = batchProducer.acquireTx(PAYLOAD_BYTES)) {
								tx.commitUnflushed(PAYLOAD_BYTES, 1);
							}
						}
						batchProducer.flush();
					} catch (BufferFullException ignored) {
						Thread.yield();
					} catch (Exception ignored) {
					}
				}
			});
		}
	}

	@TearDown(Level.Trial)
	public void teardown() {
		throughputRunning.set(false);
		pongRunning.set(false);
		batchRunning.set(false);
	}

	@Benchmark
	public void throughput() {
		while (true) {
			try (TxGuard tx = throughputProducer.acquireTx(PAYLOAD_BYTES)) {
				tx.commit(PAYLOAD_BYTES, 1);
				return;
			} catch (BufferFullException ignored) {
				Thread.yield();
			}
		}
	}

	@Benchmark
	public void pingPong(Blackhole bh) {
		try (TxGuard tx = pingTx.acquireTx(PAYLOAD_BYTES)) {
			tx.commit(PAYLOAD_BYTES, 1);
		}
		RxGuard rx = pingRx.acquireRx(BENCH_SPIN);
		if (rx != null) {
			bh.consume(rx.getTypeId());
			rx.close();
		}
	}

	@Benchmark
	public long drainBatch(Blackhole bh) {
		try (RxBatchGuard batch = batchConsumer.drainBatch(64, BENCH_SPIN)) {
			long n = batch.getCount();
			bh.consume(n);
			batch.commit();
			return n;
		}
	}

	public static void main(String[] args) throws RunnerException {
		Options opt = new OptionsBuilder()
				.include(BenchIpc.class.getSimpleName())
				.forks(1)
				.warmupIterations(5)
				.measurementIterations(10)
				.resultFormat(org.openjdk.jmh.results.format.ResultFormatType.JSON)
				.result("benchmark/results/jmh_" + System.currentTimeMillis() + ".json")
				.build();
		new Runner(opt).run();
	}
}
