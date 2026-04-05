package io.tachyon;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.util.Objects;
import java.util.concurrent.atomic.AtomicBoolean;

public final class TachyonBus implements AutoCloseable {
	private final MemorySegment busHandle;
	private final AtomicBoolean closed;
	private final Arena busArena;

	private TachyonBus(MemorySegment busHandle) {
		this.busHandle = busHandle;
		this.closed = new AtomicBoolean(false);
		this.busArena = Arena.ofShared();
	}

	public static TachyonBus listen(String path, long capacity) {
		Objects.requireNonNull(path, "Path cannot be null");
		if (capacity <= 0) {
			throw new IllegalArgumentException("Capacity must be positive.");
		}

		try (Arena arena = Arena.ofConfined()) {
			MemorySegment pathSegment = arena.allocateUtf8String(path);
			MemorySegment handle = TachyonABI.busListen(pathSegment, capacity);
			return new TachyonBus(handle);
		}
	}

	public static TachyonBus connect(String path) {
		Objects.requireNonNull(path, "Path cannot be null");

		try (Arena arena = Arena.ofConfined()) {
			MemorySegment pathSegment = arena.allocateUtf8String(path);
			MemorySegment handle = TachyonABI.busConnect(pathSegment);
			return new TachyonBus(handle);
		}
	}

	public void setPollingMode(int spinMode) {
		checkOpen();
		TachyonABI.setPollingMode(busHandle, spinMode);
	}

	public void setNumaNode(int nodeId) {
		checkOpen();
		TachyonABI.setNumaNode(busHandle, nodeId);
	}

	public TxGuard acquireTx(long maxSize) {
		checkOpen();
		MemorySegment ptr = TachyonABI.acquireTx(busHandle, maxSize);
		if (ptr.address() == 0L) {
			throw new BufferFullException();
		}
		MemorySegment safePtr = ptr.reinterpret(maxSize, busArena, null);
		return new TxGuard(busHandle, safePtr);
	}

	public void flush() {
		checkOpen();
		TachyonABI.flush(busHandle);
	}

	public RxGuard acquireRx(int spinThreshold) {
		checkOpen();
		TachyonABI.RxResult result = TachyonABI.acquireRxBlocking(busHandle, spinThreshold);
		if (result == null) {
			return null;
		}
		MemorySegment safePtr = result.payload().reinterpret(result.actualSize(), busArena, null);
		return new RxGuard(busHandle, safePtr, result.typeId(), result.actualSize());
	}

	public RxBatchGuard drainBatch(int maxMsgs, int spinThreshold) {
		checkOpen();
		if (maxMsgs <= 0) {
			throw new IllegalArgumentException("maxMsgs must be strictly positive");
		}
		return new RxBatchGuard(busHandle, busArena, maxMsgs, spinThreshold);
	}

	@Override
	public void close() {
		if (closed.compareAndSet(false, true)) {
			busArena.close();
			TachyonABI.busDestroy(busHandle);
		}
	}

	private void checkOpen() {
		if (closed.get()) {
			throw new IllegalStateException("TachyonBus is closed");
		}
	}
}
