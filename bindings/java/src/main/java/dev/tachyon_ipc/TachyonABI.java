package dev.tachyon_ipc;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;

/**
 * Internal FFM ABI bindings for the Tachyon native shared library.
 *
 * @apiNote Strictly restricted to internal bus operations.
 * @implSpec Method handles are invoked using invokeExact, requiring strict type isomorphism
 * with the declared FunctionDescriptors to avoid auto-boxing overhead.
 */
final class TachyonABI {

	/**
	 * The native linker instance resolving downcalls for the host ABI.
	 */
	private static final Linker linker;

	/**
	 * The symbol table lookup restricted to the loaded Tachyon shared library context.
	 */
	private static final SymbolLookup lookup;

	private TachyonABI() {
	}

	static {
		NativeLoader.load();
		linker = Linker.nativeLinker();
		lookup = SymbolLookup.loaderLookup();
	}

	/**
	 * Resolves a native symbol and constructs a strict downcall method handle.
	 *
	 * @param name       The exact exported C symbol name.
	 * @param descriptor The FFM function descriptor matching the C signature.
	 * @return The executable method handle.
	 * @implNote Throws UnsatisfiedLinkError immediately at initialization if a symbol is missing.
	 */
	private static MethodHandle downcall(String name, FunctionDescriptor descriptor) {
		MemorySegment symbol = lookup.find(name).orElseThrow(() ->
				new UnsatisfiedLinkError("Native symbol not found: " + name)
		);
		return linker.downcallHandle(symbol, descriptor);
	}

	/**
	 * Maps to: {@code TACHYON_ABI tachyon_error_t tachyon_bus_listen(const char*, size_t, tachyon_bus_t**)}
	 */
	private static final MethodHandle MH_BUS_LISTEN = downcall("tachyon_bus_listen",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG, ValueLayout.ADDRESS));

	/**
	 * Maps to: {@code TACHYON_ABI tachyon_error_t tachyon_bus_connect(const char*, tachyon_bus_t**)}
	 */
	private static final MethodHandle MH_BUS_CONNECT = downcall("tachyon_bus_connect",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS));

	/**
	 * Maps to: {@code TACHYON_ABI void tachyon_bus_destroy(tachyon_bus_t*)}
	 */
	private static final MethodHandle MH_BUS_DESTROY = downcall("tachyon_bus_destroy",
			FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));

	/**
	 * Maps to: {@code TACHYON_ABI void tachyon_flush(tachyon_bus_t*)}
	 */
	private static final MethodHandle MH_FLUSH = downcall("tachyon_flush",
			FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));

	/**
	 * Maps to: {@code TACHYON_ABI void tachyon_bus_set_polling_mode(tachyon_bus_t*, int)}
	 */
	private static final MethodHandle MH_SET_POLLING_MODE = downcall("tachyon_bus_set_polling_mode",
			FunctionDescriptor.ofVoid(ValueLayout.ADDRESS, ValueLayout.JAVA_INT));

	/**
	 * Maps to: {@code TACHYON_ABI tachyon_error_t tachyon_bus_set_numa_node(const tachyon_bus_t*, int)}
	 */
	private static final MethodHandle MH_SET_NUMA_NODE = downcall("tachyon_bus_set_numa_node",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));

	/**
	 * Maps to: {@code TACHYON_ABI void* tachyon_acquire_tx(tachyon_bus_t*, size_t)}
	 */
	private static final MethodHandle MH_ACQUIRE_TX = downcall("tachyon_acquire_tx",
			FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));

	/**
	 * Maps to: {@code TACHYON_ABI tachyon_error_t tachyon_commit_tx(tachyon_bus_t*, size_t, uint32_t)}
	 */
	private static final MethodHandle MH_COMMIT_TX = downcall("tachyon_commit_tx",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG, ValueLayout.JAVA_INT));

	/**
	 * Maps to: {@code TACHYON_ABI tachyon_error_t tachyon_rollback_tx(tachyon_bus_t*)}
	 */
	private static final MethodHandle MH_ROLLBACK_TX = downcall("tachyon_rollback_tx",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));

	/**
	 * Maps to: {@code TACHYON_ABI const void* tachyon_acquire_rx_blocking(tachyon_bus_t*, uint32_t*, size_t*, uint32_t)}
	 */
	private static final MethodHandle MH_ACQUIRE_RX_BLOCKING = downcall("tachyon_acquire_rx_blocking",
			FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));

	/**
	 * Maps to: {@code TACHYON_ABI tachyon_error_t tachyon_commit_rx(tachyon_bus_t*)}
	 */
	private static final MethodHandle MH_COMMIT_RX = downcall("tachyon_commit_rx",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));

	/**
	 * Maps to: {@code TACHYON_ABI size_t tachyon_drain_batch(tachyon_bus_t*, tachyon_msg_view_t*, size_t, uint32_t)}
	 */
	private static final MethodHandle MH_DRAIN_BATCH = downcall("tachyon_drain_batch",
			FunctionDescriptor.of(ValueLayout.JAVA_LONG, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG, ValueLayout.JAVA_INT));

	/**
	 * Maps to: {@code TACHYON_ABI tachyon_error_t tachyon_commit_rx_batch(tachyon_bus_t*, const tachyon_msg_view_t*, size_t)}
	 */
	private static final MethodHandle MH_COMMIT_RX_BATCH = downcall("tachyon_commit_rx_batch",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));

	/**
	 * Maps to: {@code TACHYON_ABI tachyon_state_t tachyon_get_state(const tachyon_bus_t*)}
	 */
	private static final MethodHandle MH_GET_STATE = downcall("tachyon_get_state",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));

	/**
	 * Value record holding the parameters of a successful blocking receive operation.
	 *
	 * @param payload    The bounded memory segment pointing to the message.
	 * @param typeId     The protocol identifier extracted from the struct.
	 * @param actualSize The real payload size in bytes.
	 */
	record RxResult(MemorySegment payload, int typeId, long actualSize) {
	}

	/**
	 * Validates the native execution status and bridges non-zero returns to Java exceptions.
	 *
	 * @param code The native ABI return code.
	 */
	private static void checkError(int code) {
		if (code != 0) {
			throw TachyonException.fromCode(code);
		}
	}

	/**
	 * Standardizes exception propagation across the FFI boundary.
	 *
	 * @param t          The throwable caught during handle invocation.
	 * @param methodName The native ABI function context.
	 * @return A formatted runtime exception, unless the throwable is already unchecked.
	 * @implNote Preserves raw Error and RuntimeException instances to avoid masking JVM linkage faults.
	 */
	private static RuntimeException handleException(Throwable t, String methodName) {
		if (t instanceof RuntimeException re) throw re;
		if (t instanceof Error e) throw e;
		return new RuntimeException("Fatal error invoking " + methodName, t);
	}

	/**
	 * Invokes the native listen instruction to initialize a shared memory arena.
	 *
	 * @param path     The memory segment containing the UDS socket path.
	 * @param capacity The requested size of the ring buffer.
	 * @return The memory segment pointing to the opaque native bus handle.
	 * @implSpec Uses a confined arena to safely allocate the output pointer during the FFI transition.
	 */
	static MemorySegment busListen(MemorySegment path, long capacity) {
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment outBus = arena.allocate(ValueLayout.ADDRESS);
			int result = (int) MH_BUS_LISTEN.invokeExact(path, capacity, outBus);
			checkError(result);
			return outBus.get(ValueLayout.ADDRESS, 0);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_bus_listen");
		}
	}

	/**
	 * Invokes the native connect instruction to map an existing shared memory arena.
	 *
	 * @param path The memory segment containing the UDS socket path.
	 * @return The memory segment pointing to the opaque native bus handle.
	 */
	static MemorySegment busConnect(MemorySegment path) {
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment outBus = arena.allocate(ValueLayout.ADDRESS);
			int result = (int) MH_BUS_CONNECT.invokeExact(path, outBus);
			checkError(result);
			return outBus.get(ValueLayout.ADDRESS, 0);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_bus_connect");
		}
	}

	/**
	 * Tears down the native bus instance and unmaps the shared memory.
	 *
	 * @param handle The native bus pointer.
	 */
	static void busDestroy(MemorySegment handle) {
		try {
			MH_BUS_DESTROY.invokeExact(handle);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_bus_destroy");
		}
	}

	/**
	 * Notifies sleeping consumers via a futex wake-up signal.
	 *
	 * @param handle The native bus pointer.
	 * @implNote Only has an observable effect when the consumer is in futex-sleep mode.
	 * In pure-spin mode the consumer never calls futex_wait, so no wake-up is needed.
	 */
	static void flush(MemorySegment handle) {
		try {
			MH_FLUSH.invokeExact(handle);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_flush");
		}
	}

	/**
	 * Instructs the native OS to bind the shared memory pages to a specific NUMA node.
	 *
	 * @param handle The native bus pointer.
	 * @param nodeId The target NUMA node index.
	 */
	static void setNumaNode(MemorySegment handle, int nodeId) {
		try {
			checkError((int) MH_SET_NUMA_NODE.invokeExact(handle, nodeId));
		} catch (Throwable t) {
			throw handleException(t, "tachyon_bus_set_numa_node");
		}
	}

	/**
	 * Configures the consumer polling back-off strategy.
	 *
	 * @param handle   The native bus pointer.
	 * @param spinMode {@code 0} = futex-sleep; non-zero = pure spin via {@code cpu_relax()}.
	 */
	static void setPollingMode(MemorySegment handle, int spinMode) {
		try {
			MH_SET_POLLING_MODE.invokeExact(handle, spinMode);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_bus_set_polling_mode");
		}
	}

	/**
	 * Requests an exclusive memory slot from the producer arena.
	 *
	 * @param handle  The native bus pointer.
	 * @param maxSize The required contiguous byte capacity.
	 * @return A bounded memory segment for zero-copy writes, or a zero-address segment if the buffer is full.
	 */
	static MemorySegment acquireTx(MemorySegment handle, long maxSize) {
		try {
			return (MemorySegment) MH_ACQUIRE_TX.invokeExact(handle, maxSize);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_acquire_tx");
		}
	}

	/**
	 * Publishes a transaction without issuing a futex wake-up signal.
	 *
	 * @param handle     The native bus pointer.
	 * @param actualSize The number of bytes successfully written to the segment.
	 * @param typeId     The user-defined protocol identifier.
	 */
	static void commitTxUnflushed(MemorySegment handle, long actualSize, int typeId) {
		try {
			checkError((int) MH_COMMIT_TX.invokeExact(handle, actualSize, typeId));
		} catch (Throwable t) {
			throw handleException(t, "tachyon_commit_tx");
		}
	}

	/**
	 * Publishes a transaction and notifies sleeping consumers via a futex wake-up signal.
	 *
	 * @param handle     The native bus pointer.
	 * @param actualSize The number of bytes successfully written to the segment.
	 * @param typeId     The user-defined protocol identifier.
	 */
	static void commitTx(MemorySegment handle, long actualSize, int typeId) {
		commitTxUnflushed(handle, actualSize, typeId);
		flush(handle);
	}

	/**
	 * Aborts an uncommitted transaction and releases the lock on the memory slot.
	 *
	 * @param handle The native bus pointer.
	 */
	static void rollbackTx(MemorySegment handle) {
		try {
			checkError((int) MH_ROLLBACK_TX.invokeExact(handle));
		} catch (Throwable t) {
			throw handleException(t, "tachyon_rollback_tx");
		}
	}

	/**
	 * Polls the arena for the next available message, spinning then falling back to futex sleep.
	 *
	 * @param handle        The native bus pointer.
	 * @param spinThreshold The number of {@code cpu_relax()} spin iterations before falling
	 *                      back to an OS-level futex wait.
	 * @return An {@link RxResult} containing the mapped memory segment,
	 *         or {@code null} if the futex wait was interrupted by a signal (EINTR).
	 */
	static RxResult acquireRxBlocking(MemorySegment handle, int spinThreshold) {
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment outTypeId = arena.allocate(ValueLayout.JAVA_INT);
			MemorySegment outActualSize = arena.allocate(ValueLayout.JAVA_LONG);

			MemorySegment payload = (MemorySegment) MH_ACQUIRE_RX_BLOCKING.invokeExact(
					handle, outTypeId, outActualSize, spinThreshold);

			if (payload.address() == 0) {
				return null;
			}

			return new RxResult(
					payload,
					outTypeId.get(ValueLayout.JAVA_INT, 0),
					outActualSize.get(ValueLayout.JAVA_LONG, 0)
			);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_acquire_rx_blocking");
		}
	}

	/**
	 * Signals the native arena to advance the consumer head after processing a single message.
	 *
	 * @param handle The native bus pointer.
	 */
	static void commitRx(MemorySegment handle) {
		try {
			checkError((int) MH_COMMIT_RX.invokeExact(handle));
		} catch (Throwable t) {
			throw handleException(t, "tachyon_commit_rx");
		}
	}

	/**
	 * Drains multiple contiguous messages directly into an FFM struct array in a single FFI crossing.
	 *
	 * @param handle        The native bus pointer.
	 * @param viewsSegment  The pre-allocated array of {@code tachyon_msg_view_t}.
	 * @param maxMsgs       The upper limit of messages to extract in one pass.
	 * @param spinThreshold The number of {@code cpu_relax()} spin iterations before falling
	 *                      back to an OS-level futex wait.
	 * @return The number of valid messages loaded into the struct array. Returns {@code 0}
	 *         if interrupted or if the arena has entered a fatal error state.
	 */
	static long drainBatch(MemorySegment handle, MemorySegment viewsSegment, int maxMsgs, int spinThreshold) {
		try {
			return (long) MH_DRAIN_BATCH.invokeExact(handle, viewsSegment, (long) maxMsgs, spinThreshold);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_drain_batch");
		}
	}

	/**
	 * Acknowledges the processing of an entire message batch, advancing the consumer head
	 * in a single FFI crossing.
	 *
	 * @param handle       The native bus pointer.
	 * @param viewsSegment The populated array of {@code tachyon_msg_view_t}.
	 * @param count        The number of messages successfully processed.
	 */
	static void commitRxBatch(MemorySegment handle, MemorySegment viewsSegment, long count) {
		try {
			checkError((int) MH_COMMIT_RX_BATCH.invokeExact(handle, viewsSegment, count));
		} catch (Throwable t) {
			throw handleException(t, "tachyon_commit_rx_batch");
		}
	}

	/**
	 * Reads the internal atomic state of the C++ arena structure.
	 *
	 * @param handle The native bus pointer.
	 * @return The integer value of the {@code tachyon_state_t} enumeration.
	 */
	static int getState(MemorySegment handle) {
		try {
			return (int) MH_GET_STATE.invokeExact(handle);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_get_state");
		}
	}
}
