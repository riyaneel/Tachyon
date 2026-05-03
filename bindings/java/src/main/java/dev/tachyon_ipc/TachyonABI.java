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
	 * Maps to: {@code TACHYON_ABI tachyon_error_t tachyon_rpc_listen(const char*, size_t, size_t, tachyon_rpc_bus_t**)}
	 */
	private static final MethodHandle MH_RPC_LISTEN = downcall("tachyon_rpc_listen",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG, ValueLayout.JAVA_LONG, ValueLayout.ADDRESS));

	/**
	 * Maps to: {@code TACHYON_ABI tachyon_error_t tachyon_rpc_connect(const char*, tachyon_rpc_bus_t**)}
	 */
	private static final MethodHandle MH_RPC_CONNECT = downcall("tachyon_rpc_connect",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS));

	/**
	 * Maps to: {@code TACHYON_ABI void tachyon_rpc_destroy(tachyon_rpc_bus_t*)}
	 */
	private static final MethodHandle MH_RPC_DESTROY = downcall("tachyon_rpc_destroy",
			FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));

	/**
	 * Maps to: {@code TACHYON_ABI void* tachyon_rpc_acquire_tx(tachyon_rpc_bus_t*, size_t)}
	 */
	private static final MethodHandle MH_RPC_ACQUIRE_TX = downcall("tachyon_rpc_acquire_tx",
			FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));

	/**
	 * Maps to: {@code TACHYON_ABI tachyon_error_t tachyon_rpc_commit_call(tachyon_rpc_bus_t*, size_t, uint32_t, uint64_t*)}
	 */
	private static final MethodHandle MH_RPC_COMMIT_CALL = downcall("tachyon_rpc_commit_call",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG, ValueLayout.JAVA_INT, ValueLayout.ADDRESS));

	/**
	 * Maps to: {@code TACHYON_ABI void tachyon_rpc_rollback_call(tachyon_rpc_bus_t*)}
	 */
	private static final MethodHandle MH_RPC_ROLLBACK_CALL = downcall("tachyon_rpc_rollback_call",
			FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));

	/**
	 * Maps to: {@code TACHYON_ABI const void* tachyon_rpc_wait(tachyon_rpc_bus_t*, uint64_t, size_t*, uint32_t*, uint32_t)}
	 */
	private static final MethodHandle MH_RPC_WAIT = downcall("tachyon_rpc_wait",
			FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));

	/**
	 * Maps to: {@code TACHYON_ABI tachyon_error_t tachyon_rpc_commit_rx(tachyon_rpc_bus_t*)}
	 */
	private static final MethodHandle MH_RPC_COMMIT_RX = downcall("tachyon_rpc_commit_rx",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));

	/**
	 * Maps to: {@code TACHYON_ABI const void* tachyon_rpc_serve(tachyon_rpc_bus_t*, uint64_t*, uint32_t*, size_t*, uint32_t)}
	 */
	private static final MethodHandle MH_RPC_SERVE = downcall("tachyon_rpc_serve",
			FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));

	/**
	 * Maps to: {@code TACHYON_ABI tachyon_error_t tachyon_rpc_commit_serve(tachyon_rpc_bus_t*)}
	 */
	private static final MethodHandle MH_RPC_COMMIT_SERVE = downcall("tachyon_rpc_commit_serve",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));

	/**
	 * Maps to: {@code TACHYON_ABI void* tachyon_rpc_acquire_reply_tx(tachyon_rpc_bus_t*, size_t)}
	 */
	private static final MethodHandle MH_RPC_ACQUIRE_REPLY_TX = downcall("tachyon_rpc_acquire_reply_tx",
			FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));

	/**
	 * Maps to: {@code TACHYON_ABI tachyon_error_t tachyon_rpc_commit_reply(tachyon_rpc_bus_t*, uint64_t, size_t, uint32_t)}
	 */
	private static final MethodHandle MH_RPC_COMMIT_REPLY = downcall("tachyon_rpc_commit_reply",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG, ValueLayout.JAVA_LONG, ValueLayout.JAVA_INT));

	/**
	 * Maps to: {@code TACHYON_ABI void tachyon_rpc_rollback_reply(tachyon_rpc_bus_t*)}
	 */
	private static final MethodHandle MH_RPC_ROLLBACK_REPLY = downcall("tachyon_rpc_rollback_reply",
			FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));

	/**
	 * Maps to: {@code TACHYON_ABI void tachyon_rpc_set_polling_mode(const tachyon_rpc_bus_t*, int)}
	 */
	private static final MethodHandle MH_RPC_SET_POLLING_MODE = downcall("tachyon_rpc_set_polling_mode",
			FunctionDescriptor.ofVoid(ValueLayout.ADDRESS, ValueLayout.JAVA_INT));

	/**
	 * Maps to: {@code TACHYON_ABI tachyon_state_t tachyon_rpc_get_state(const tachyon_rpc_bus_t*)}
	 */
	private static final MethodHandle MH_RPC_GET_STATE = downcall("tachyon_rpc_get_state",
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
	@SuppressWarnings("removal")
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
	@SuppressWarnings("removal")
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
	 * or {@code null} if the futex wait was interrupted by a signal (EINTR).
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
	 * if interrupted or if the arena has entered a fatal error state.
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

	/**
	 * Value record holding the result of a blocking RPC receive operation (wait or serve).
	 *
	 * @param payload       The bounded memory segment pointing to the message payload.
	 * @param msgType       The application-level message type set by the sender.
	 * @param actualSize    The real payload size in bytes.
	 * @param correlationId The correlation ID of this message. On the callee side ({@code serve}),
	 *                      use this value when calling {@link #rpcCommitReply}.
	 */
	record RpcRxResult(MemorySegment payload, int msgType, long actualSize, long correlationId) {
	}

	/**
	 * Creates two SHM arenas and blocks until a caller connects via the UNIX socket at {@code path}.
	 * Interrupted syscalls ({@code EINTR}) are retried transparently.
	 *
	 * @param path   The memory segment containing the null-terminated UDS socket path.
	 * @param capFwd Arena capacity for incoming requests (caller→callee). Must be a power of two.
	 * @param capRev Arena capacity for outgoing replies (callee→caller). Must be a power of two.
	 * @return The memory segment pointing to the opaque native {@code tachyon_rpc_bus_t} handle.
	 * @throws TachyonException if SHM creation or socket binding fails.
	 */
	static MemorySegment rpcListen(MemorySegment path, long capFwd, long capRev) {
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment outPtr = arena.allocate(ValueLayout.ADDRESS);
			int result;
			do {
				result = (int) MH_RPC_LISTEN.invokeExact(path, capFwd, capRev, outPtr);
			} while (result == 13);
			checkError(result);
			return outPtr.get(ValueLayout.ADDRESS, 0);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_rpc_listen");
		}
	}

	/**
	 * Attaches to existing SHM arenas via the UNIX socket at {@code path}.
	 *
	 * @param path The memory segment containing the null-terminated UDS socket path.
	 * @return The memory segment pointing to the opaque native {@code tachyon_rpc_bus_t} handle.
	 * @throws TachyonException with {@code TACHYON_ERR_ABI_MISMATCH} if the callee was compiled
	 *                          with a different Tachyon version or {@code TACHYON_MSG_ALIGNMENT}.
	 */
	static MemorySegment rpcConnect(MemorySegment path) {
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment outPtr = arena.allocate(ValueLayout.ADDRESS);
			checkError((int) MH_RPC_CONNECT.invokeExact(path, outPtr));
			return outPtr.get(ValueLayout.ADDRESS, 0);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_rpc_connect");
		}
	}

	/**
	 * Tears down the native RPC bus instance and unmaps both SHM arenas.
	 *
	 * @param handle The native {@code tachyon_rpc_bus_t} pointer.
	 */
	static void rpcDestroy(MemorySegment handle) {
		try {
			MH_RPC_DESTROY.invokeExact(handle);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_rpc_destroy");
		}
	}

	/**
	 * Acquires a zero-copy TX slot in {@code arena_fwd} (caller→callee).
	 * Returns a null-address segment if the ring buffer is full or in fatal error state.
	 *
	 * @param handle  The native {@code tachyon_rpc_bus_t} pointer.
	 * @param maxSize The required contiguous byte capacity.
	 * @return A memory segment for zero-copy writes, or a zero-address segment if unavailable.
	 */
	static MemorySegment rpcAcquireTx(MemorySegment handle, long maxSize) {
		try {
			return (MemorySegment) MH_RPC_ACQUIRE_TX.invokeExact(handle, maxSize);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_rpc_acquire_tx");
		}
	}

	/**
	 * Commits the TX slot acquired via {@link #rpcAcquireTx}, assigns a {@code correlation_id},
	 * and flushes {@code arena_fwd}.
	 *
	 * @param handle     The native {@code tachyon_rpc_bus_t} pointer.
	 * @param actualSize The number of bytes written to the slot.
	 * @param msgType    The application-level message type (uint16).
	 * @param outCid     Pre-allocated {@code JAVA_LONG} segment to receive the assigned correlation ID.
	 * @throws TachyonException if no slot was previously acquired or {@code actualSize} exceeds the reservation.
	 */
	static void rpcCommitCall(MemorySegment handle, long actualSize, int msgType, MemorySegment outCid) {
		try {
			checkError((int) MH_RPC_COMMIT_CALL.invokeExact(handle, actualSize, msgType, outCid));
		} catch (Throwable t) {
			throw handleException(t, "tachyon_rpc_commit_call");
		}
	}

	/**
	 * Rolls back the TX slot acquired via {@link #rpcAcquireTx} without publishing.
	 * Sets {@code arena_fwd} to fatal error state if no slot was acquired.
	 *
	 * @param handle The native {@code tachyon_rpc_bus_t} pointer.
	 */
	static void rpcRollbackCall(MemorySegment handle) {
		try {
			MH_RPC_ROLLBACK_CALL.invokeExact(handle);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_rpc_rollback_call");
		}
	}

	/**
	 * Thin downcall to {@code tachyon_rpc_wait}. Writes payload size and message type
	 * into the caller-supplied pre-allocated segments. Returns a null-address segment
	 * if a correlation ID mismatch is detected (fatal error set on {@code arena_rev})
	 * or if the futex wait is interrupted by a signal (EINTR).
	 *
	 * <p>Callers are responsible for checking the returned address before reading
	 * {@code outSize} and {@code outMsgType}.
	 *
	 * @param handle        The native {@code tachyon_rpc_bus_t} pointer.
	 * @param correlationId The ID returned by a prior {@link #rpcCommitCall}.
	 * @param outSize       Pre-allocated {@code JAVA_LONG} segment to receive the payload size.
	 * @param outMsgType    Pre-allocated {@code JAVA_INT} segment to receive the message type.
	 * @param spinThreshold The number of {@code cpu_relax()} iterations before futex sleep.
	 * @return A raw pointer into {@code arena_rev} at the start of the payload,
	 * or a null-address segment on EINTR or fatal error.
	 */
	static MemorySegment rpcWait(MemorySegment handle, long correlationId,
	                             MemorySegment outSize, MemorySegment outMsgType, int spinThreshold) {
		try {
			return (MemorySegment) MH_RPC_WAIT.invokeExact(
					handle, correlationId, outSize, outMsgType, spinThreshold);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_rpc_wait");
		}
	}

	/**
	 * Releases the {@code arena_rev} read slot after processing a response received via {@link #rpcWait}.
	 *
	 * @param handle The native {@code tachyon_rpc_bus_t} pointer.
	 * @throws TachyonException if no slot was previously acquired.
	 */
	static void rpcCommitRx(MemorySegment handle) {
		try {
			checkError((int) MH_RPC_COMMIT_RX.invokeExact(handle));
		} catch (Throwable t) {
			throw handleException(t, "tachyon_rpc_commit_rx");
		}
	}

	/**
	 * Thin downcall to {@code tachyon_rpc_serve}. Writes correlation ID, message type,
	 * and payload size into the caller-supplied pre-allocated segments. Returns a
	 * null-address segment if the futex wait is interrupted by a signal (EINTR) or
	 * if a fatal error is detected on {@code arena_fwd}.
	 *
	 * <p>Callers must commit the returned slot via {@link #rpcCommitServe} before
	 * acquiring a reply TX slot via {@link #rpcAcquireReplyTx}, to avoid holding
	 * both arena slots simultaneously.
	 *
	 * @param handle        The native {@code tachyon_rpc_bus_t} pointer.
	 * @param outCid        Pre-allocated {@code JAVA_LONG} segment to receive the correlation ID.
	 * @param outMsgType    Pre-allocated {@code JAVA_INT} segment to receive the message type.
	 * @param outSize       Pre-allocated {@code JAVA_LONG} segment to receive the payload size.
	 * @param spinThreshold The number of {@code cpu_relax()} iterations before futex sleep.
	 * @return A raw pointer into {@code arena_fwd} at the start of the payload,
	 * or a null-address segment on EINTR or fatal error.
	 */
	static MemorySegment rpcServe(MemorySegment handle, MemorySegment outCid,
	                              MemorySegment outMsgType, MemorySegment outSize, int spinThreshold) {
		try {
			return (MemorySegment) MH_RPC_SERVE.invokeExact(
					handle, outCid, outMsgType, outSize, spinThreshold);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_rpc_serve");
		}
	}

	/**
	 * Releases the {@code arena_fwd} read slot after processing a request received via {@link #rpcServe}.
	 * Must be called before {@link #rpcCommitReply} to avoid holding both arena slots simultaneously.
	 *
	 * @param handle The native {@code tachyon_rpc_bus_t} pointer.
	 * @throws TachyonException if no slot was previously acquired.
	 */
	static void rpcCommitServe(MemorySegment handle) {
		try {
			checkError((int) MH_RPC_COMMIT_SERVE.invokeExact(handle));
		} catch (Throwable t) {
			throw handleException(t, "tachyon_rpc_commit_serve");
		}
	}

	/**
	 * Acquires a zero-copy TX slot in {@code arena_rev} (callee→caller) for the given {@code correlationId}.
	 * Returns a null-address segment if the ring buffer is full or in fatal error state.
	 *
	 * @param handle  The native {@code tachyon_rpc_bus_t} pointer.
	 * @param maxSize The required contiguous byte capacity.
	 * @return A memory segment for zero-copy writes, or a zero-address segment if unavailable.
	 */
	static MemorySegment rpcAcquireReplyTx(MemorySegment handle, long maxSize) {
		try {
			return (MemorySegment) MH_RPC_ACQUIRE_REPLY_TX.invokeExact(handle, maxSize);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_rpc_acquire_reply_tx");
		}
	}

	/**
	 * Commits the reply TX slot acquired via {@link #rpcAcquireReplyTx} and flushes {@code arena_rev}.
	 * {@code correlationId} must be non-zero and must match the value from the served request.
	 *
	 * @param handle        The native {@code tachyon_rpc_bus_t} pointer.
	 * @param correlationId The ID from the {@link RpcRxResult} returned by {@link #rpcServe}.
	 * @param actualSize    The number of bytes written to the slot.
	 * @param msgType       The application-level response type (uint16).
	 * @throws TachyonException if {@code correlationId == 0}, no slot was acquired, or
	 *                          {@code actualSize} exceeds the reservation.
	 */
	static void rpcCommitReply(MemorySegment handle, long correlationId, long actualSize, int msgType) {
		try {
			checkError((int) MH_RPC_COMMIT_REPLY.invokeExact(handle, correlationId, actualSize, msgType));
		} catch (Throwable t) {
			throw handleException(t, "tachyon_rpc_commit_reply");
		}
	}

	/**
	 * Rolls back the reply TX slot acquired via {@link #rpcAcquireReplyTx} without publishing.
	 * Sets {@code arena_rev} to fatal error state if no slot was acquired.
	 *
	 * @param handle The native {@code tachyon_rpc_bus_t} pointer.
	 */
	static void rpcRollbackReply(MemorySegment handle) {
		try {
			MH_RPC_ROLLBACK_REPLY.invokeExact(handle);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_rpc_rollback_reply");
		}
	}

	/**
	 * Configures the consumer polling back-off strategy for both {@code arena_fwd} and {@code arena_rev}.
	 *
	 * @param handle   The native {@code tachyon_rpc_bus_t} pointer.
	 * @param spinMode {@code 0} = hybrid futex-sleep; non-zero = pure spin via {@code cpu_relax()}.
	 *                 Use non-zero only when both consumer threads are dedicated and will never park.
	 */
	static void rpcSetPollingMode(MemorySegment handle, int spinMode) {
		try {
			MH_RPC_SET_POLLING_MODE.invokeExact(handle, spinMode);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_rpc_set_polling_mode");
		}
	}

	/**
	 * Reads the composite state of the native RPC bus.
	 * Returns {@code TACHYON_STATE_FATAL_ERROR} if either {@code arena_fwd} or {@code arena_rev}
	 * has entered a fatal error state.
	 *
	 * @param handle The native {@code tachyon_rpc_bus_t} pointer.
	 * @return The integer value of the {@code tachyon_state_t} enumeration.
	 */
	static int rpcGetState(MemorySegment handle) {
		try {
			return (int) MH_RPC_GET_STATE.invokeExact(handle);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_rpc_get_state");
		}
	}
}
