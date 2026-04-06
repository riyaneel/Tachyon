package dev.tachyon_ipc;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;

final class TachyonABI {
	private static final Linker linker;
	private static final SymbolLookup lookup;

	private TachyonABI() {
	}

	static {
		NativeLoader.load();
		linker = Linker.nativeLinker();
		lookup = SymbolLookup.loaderLookup();
	}

	private static MethodHandle downcall(String name, FunctionDescriptor descriptor) {
		MemorySegment symbol = lookup.find(name).orElseThrow(() ->
				new UnsatisfiedLinkError("Native symbol not found: " + name)
		);
		return linker.downcallHandle(symbol, descriptor);
	}

	private static final MethodHandle MH_BUS_LISTEN = downcall("tachyon_bus_listen",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG, ValueLayout.ADDRESS));

	private static final MethodHandle MH_BUS_CONNECT = downcall("tachyon_bus_connect",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS));

	private static final MethodHandle MH_BUS_DESTROY = downcall("tachyon_bus_destroy",
			FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));

	private static final MethodHandle MH_FLUSH = downcall("tachyon_flush",
			FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));

	private static final MethodHandle MH_SET_POLLING_MODE = downcall("tachyon_bus_set_polling_mode",
			FunctionDescriptor.ofVoid(ValueLayout.ADDRESS, ValueLayout.JAVA_INT));

	private static final MethodHandle MH_SET_NUMA_NODE = downcall("tachyon_bus_set_numa_node",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));

	private static final MethodHandle MH_ACQUIRE_TX = downcall("tachyon_acquire_tx",
			FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));

	private static final MethodHandle MH_COMMIT_TX = downcall("tachyon_commit_tx",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG, ValueLayout.JAVA_INT));

	private static final MethodHandle MH_ROLLBACK_TX = downcall("tachyon_rollback_tx",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));

	private static final MethodHandle MH_ACQUIRE_RX_BLOCKING = downcall("tachyon_acquire_rx_blocking",
			FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));

	private static final MethodHandle MH_COMMIT_RX = downcall("tachyon_commit_rx",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));

	private static final MethodHandle MH_DRAIN_BATCH = downcall("tachyon_drain_batch",
			FunctionDescriptor.of(ValueLayout.JAVA_LONG, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG, ValueLayout.JAVA_INT));

	private static final MethodHandle MH_COMMIT_RX_BATCH = downcall("tachyon_commit_rx_batch",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));

	private static final MethodHandle MH_GET_STATE = downcall("tachyon_get_state",
			FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));

	record RxResult(MemorySegment payload, int typeId, long actualSize) {
	}

	private static void checkError(int code) {
		if (code != 0) {
			throw TachyonException.fromCode(code);
		}
	}

	private static RuntimeException handleException(Throwable t, String methodName) {
		if (t instanceof RuntimeException re) throw re;
		if (t instanceof Error e) throw e;
		return new RuntimeException("Fatal error invoking " + methodName, t);
	}

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

	static void busDestroy(MemorySegment handle) {
		try {
			MH_BUS_DESTROY.invokeExact(handle);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_bus_destroy");
		}
	}

	static void flush(MemorySegment handle) {
		try {
			MH_FLUSH.invokeExact(handle);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_flush");
		}
	}

	static void setNumaNode(MemorySegment handle, int nodeId) {
		try {
			checkError((int) MH_SET_NUMA_NODE.invokeExact(handle, nodeId));
		} catch (Throwable t) {
			throw handleException(t, "tachyon_bus_set_numa_node");
		}
	}

	static void setPollingMode(MemorySegment handle, int spinMode) {
		try {
			MH_SET_POLLING_MODE.invokeExact(handle, spinMode);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_bus_set_polling_mode");
		}
	}

	static MemorySegment acquireTx(MemorySegment handle, long maxSize) {
		try {
			return (MemorySegment) MH_ACQUIRE_TX.invokeExact(handle, maxSize);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_acquire_tx");
		}
	}

	static void commitTxUnflushed(MemorySegment handle, long actualSize, int typeId) {
		try {
			checkError((int) MH_COMMIT_TX.invokeExact(handle, actualSize, typeId));
		} catch (Throwable t) {
			throw handleException(t, "tachyon_commit_tx");
		}
	}

	static void commitTx(MemorySegment handle, long actualSize, int typeId) {
		commitTxUnflushed(handle, actualSize, typeId);
		flush(handle);
	}

	static void rollbackTx(MemorySegment handle) {
		try {
			checkError((int) MH_ROLLBACK_TX.invokeExact(handle));
		} catch (Throwable t) {
			throw handleException(t, "tachyon_rollback_tx");
		}
	}

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

	static void commitRx(MemorySegment handle) {
		try {
			checkError((int) MH_COMMIT_RX.invokeExact(handle));
		} catch (Throwable t) {
			throw handleException(t, "tachyon_commit_rx");
		}
	}

	static long drainBatch(MemorySegment handle, MemorySegment viewsSegment, int maxMsgs, int spinThreshold) {
		try {
			return (long) MH_DRAIN_BATCH.invokeExact(handle, viewsSegment, (long) maxMsgs, spinThreshold);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_drain_batch");
		}
	}

	static void commitRxBatch(MemorySegment handle, MemorySegment viewsSegment, long count) {
		try {
			checkError((int) MH_COMMIT_RX_BATCH.invokeExact(handle, viewsSegment, count));
		} catch (Throwable t) {
			throw handleException(t, "tachyon_commit_rx_batch");
		}
	}

	static int getState(MemorySegment handle) {
		try {
			return (int) MH_GET_STATE.invokeExact(handle);
		} catch (Throwable t) {
			throw handleException(t, "tachyon_get_state");
		}
	}
}
