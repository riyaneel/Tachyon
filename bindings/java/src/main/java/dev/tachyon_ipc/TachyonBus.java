package dev.tachyon_ipc;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.util.Objects;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Main orchestrator for the Tachyon SPSC (Single Producer Single Consumer) IPC bus.
 * Manages the lifecycle of the native shared memory arena and FFI boundaries.
 *
 * @apiNote Instances of this class map directly to an underlying native file descriptor
 * and memory mapping. The bus must be explicitly closed to prevent resource leaks.
 * @implSpec Operations on the bus handle are inherently thread-safe at the C++ level
 * via lock-free atomics, but typical SPSC usage dictates one dedicated thread for
 * production and one for consumption.
 */
public final class TachyonBus implements AutoCloseable {
	private final MemorySegment busHandle;
	private final AtomicBoolean closed;
	private final Arena busArena;

	private TachyonBus(MemorySegment busHandle) {
		this.busHandle = busHandle;
		this.closed = new AtomicBoolean(false);
		this.busArena = Arena.ofShared();
	}

	/**
	 * Initializes a new shared memory arena and binds it to the specified UNIX Domain Socket path.
	 *
	 * @param path     The absolute filesystem path for the UDS handshake socket.
	 * @param capacity The requested byte capacity of the ring buffer. Must be a power of 2.
	 * @return A new active {@link TachyonBus} instance.
	 * @throws IllegalArgumentException If the capacity is zero or negative.
	 * @implSpec The native bus pointer is strictly bound to a shared FFM {@link Arena}
	 * that dictates the spatial bounds of further memory segments mapped to this bus.
	 */
	public static TachyonBus listen(String path, long capacity) {
		Objects.requireNonNull(path, "Path cannot be null");
		if (capacity <= 0 || (capacity & (capacity - 1)) != 0) {
			throw new IllegalArgumentException("Capacity must be positive.");
		}

		try (Arena arena = Arena.ofConfined()) {
			MemorySegment pathSegment = arena.allocateUtf8String(path);
			MemorySegment handle = TachyonABI.busListen(pathSegment, capacity);
			return new TachyonBus(handle);
		}
	}

	/**
	 * Connects to an existing shared memory arena via UNIX Domain Socket SCM_RIGHTS negotiation.
	 *
	 * @param path The absolute filesystem path of the target UDS socket.
	 * @return A new active {@link TachyonBus} instance mapped to the remote arena.
	 */
	public static TachyonBus connect(String path) {
		Objects.requireNonNull(path, "Path cannot be null");

		try (Arena arena = Arena.ofConfined()) {
			MemorySegment pathSegment = arena.allocateUtf8String(path);
			MemorySegment handle = TachyonABI.busConnect(pathSegment);
			return new TachyonBus(handle);
		}
	}

	/**
	 * Configures the hardware polling strategy for the consumer.
	 *
	 * @param spinMode The integer mapping to the native spin enumeration (e.g., pure spin vs yield).
	 * @apiNote Modifies the atomic watchdog behavior inside the native arena to optimize
	 * either for ultra-low latency (pure spin) or lower CPU power consumption (futex sleep).
	 */
	public void setPollingMode(int spinMode) {
		checkOpen();
		TachyonABI.setPollingMode(busHandle, spinMode);
	}

	/**
	 * Pins the memory policy of the underlying shared memory file to a specific NUMA node.
	 *
	 * @param nodeId The target NUMA node index.
	 * @implSpec Translates to an OS-level mbind/set_mempolicy syscall to prevent cross-die
	 * cache invalidation penalties and optimize memory locality.
	 */
	public void setNumaNode(int nodeId) {
		checkOpen();
		TachyonABI.setNumaNode(busHandle, nodeId);
	}

	/**
	 * Requests an exclusive, zero-copy memory slot from the producer arena.
	 *
	 * @param maxSize The maximum contiguous payload size required.
	 * @return A {@link TxGuard} managing the spatial bounds of the allocated slot.
	 * @throws BufferFullException If the SPSC ring buffer lacks sufficient capacity.
	 * @implSpec The returned guard is mapped to the shared FFM arena and must be committed
	 * or rolled back to advance the producer head.
	 */
	public TxGuard acquireTx(long maxSize) {
		checkOpen();
		MemorySegment ptr = TachyonABI.acquireTx(busHandle, maxSize);
		if (ptr.address() == 0L) {
			throw new BufferFullException();
		}
		MemorySegment safePtr = ptr.reinterpret(maxSize, busArena, null);
		return new TxGuard(busHandle, safePtr);
	}

	/**
	 * Issues a strict hardware memory barrier (release semantics) to wake up sleeping consumers.
	 *
	 * @apiNote Should be invoked explicitly after a batch of unflushed transactions to
	 * ensure memory visibility across cores.
	 */
	public void flush() {
		checkOpen();
		TachyonABI.flush(busHandle);
	}

	/**
	 * Polls the arena for the next available message, potentially blocking the caller.
	 *
	 * @param spinThreshold The number of CPU yields before falling back to an OS-level futex wait.
	 * @return An {@link RxGuard} representing the read-only projection of the message, or null if interrupted.
	 * @implSpec Deserialization must be performed directly on the returned zero-copy memory segment.
	 */
	public RxGuard acquireRx(int spinThreshold) {
		checkOpen();
		TachyonABI.RxResult result = TachyonABI.acquireRxBlocking(busHandle, spinThreshold);
		if (result == null) {
			return null;
		}
		MemorySegment safePtr = result.payload().reinterpret(result.actualSize(), busArena, null);
		return new RxGuard(busHandle, safePtr, result.typeId(), result.actualSize());
	}

	/**
	 * Extracts multiple contiguous messages from the ring buffer in a single FFI crossing.
	 *
	 * @param maxMsgs       The upper bound of messages to pull from the consumer head.
	 * @param spinThreshold The CPU yield threshold before falling back to sleep.
	 * @return An {@link RxBatchGuard} containing the array of message views.
	 * @throws IllegalArgumentException If maxMsgs is strictly negative or zero.
	 * @implSpec Drastically amortizes the cost of JNI/FFM transitions by populating a contiguous
	 * C-struct array bounded within a confined memory arena.
	 */
	public RxBatchGuard drainBatch(int maxMsgs, int spinThreshold) {
		checkOpen();
		if (maxMsgs <= 0) {
			throw new IllegalArgumentException("maxMsgs must be strictly positive");
		}
		return new RxBatchGuard(busHandle, busArena, maxMsgs, spinThreshold);
	}

	/**
	 * Reads the internal atomic lifecycle state of the native arena.
	 *
	 * @return The integer mapping of the BusState native enumeration.
	 */
	public int getState() {
		checkOpen();
		return TachyonABI.getState(busHandle);
	}

	/**
	 * Initiates the teardown of the FFM shared arena and signals the native ABI to destroy the bus.
	 *
	 * @implNote Uses an atomic boolean flag to ensure idempotency. The underlying FFM shared memory
	 * arena is explicitly closed, immediately invalidating all pending guards and segments.
	 */
	@Override
	public void close() {
		if (closed.compareAndSet(false, true)) {
			try {
				TachyonABI.busDestroy(busHandle);
			} finally {
				busArena.close();
			}
		}
	}

	/**
	 * Validates the operational state of the bus.
	 *
	 * @throws IllegalStateException If the bus has been closed and the arena memory unmapped.
	 */
	private void checkOpen() {
		if (closed.get()) {
			throw new IllegalStateException("TachyonBus is closed");
		}
	}
}
