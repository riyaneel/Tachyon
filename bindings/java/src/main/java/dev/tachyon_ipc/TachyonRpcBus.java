package dev.tachyon_ipc;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.charset.StandardCharsets;

/**
 * Tachyon bidirectional RPC bus backed by two independent SPSC arenas:
 * {@code arena_fwd} (caller to callee) and {@code arena_rev} (callee to caller).
 *
 * <p>Use {@link #rpcListen} on the callee side and {@link #rpcConnect} on the
 * caller side. Always call {@link #close()} when done, or use try-with-resources.
 *
 * <p>Thread safety: one thread per direction. Do not call {@link #call} and
 * {@link #wait} concurrently, and do not call {@link #serve} and {@link #reply}
 * concurrently.
 *
 * <p>Out-params for {@link #call}, {@link #wait}, and {@link #serve} are
 * pre-allocated once at construction and reused on every invocation.
 * No per-call heap allocation occurs on the hot path.
 */
public final class TachyonRpcBus implements AutoCloseable {

	private static final int STATE_FATAL_ERROR = 4;
	private static final int SPIN_DEFAULT = 10_000;

	private MemorySegment raw;

	/**
	 * Persistent arena owning the pre-allocated out-param slots.
	 * Closed in {@link #close()}.
	 */
	private final Arena slots;

	/**
	 * Pre-allocated out-param for {@link #call} (receives assigned correlation ID)
	 * and for {@link #serve} (receives incoming correlation ID).
	 */
	private final MemorySegment slotCid;

	/**
	 * Pre-allocated out-param for {@link #wait} and {@link #serve} (receives payload size).
	 */
	private final MemorySegment slotSize;

	/**
	 * Pre-allocated out-param for {@link #wait} and {@link #serve} (receives message type).
	 */
	private final MemorySegment slotMsgType;

	private TachyonRpcBus(MemorySegment raw) {
		this.raw = raw;
		this.slots = Arena.ofShared();
		this.slotCid = slots.allocate(ValueLayout.JAVA_LONG);
		this.slotSize = slots.allocate(ValueLayout.JAVA_LONG);
		this.slotMsgType = slots.allocate(ValueLayout.JAVA_INT);
	}

	/**
	 * Creates two SHM arenas and blocks until a caller connects via the UNIX socket
	 * at {@code socketPath}. Interrupted syscalls (EINTR) are retried transparently.
	 *
	 * @param socketPath The UNIX socket path. Must not exceed 107 bytes on Linux.
	 * @param capFwd     Arena capacity for incoming requests (caller to callee).
	 *                   Must be a strictly positive power of two.
	 * @param capRev     Arena capacity for outgoing replies (callee to caller).
	 *                   Must be a strictly positive power of two.
	 * @return A fully initialized {@code TachyonRpcBus} ready to serve requests.
	 * @throws TachyonException if SHM creation or socket binding fails.
	 */
	public static TachyonRpcBus rpcListen(String socketPath, long capFwd, long capRev) {
		try (Arena tmp = Arena.ofConfined()) {
			MemorySegment path = tmp.allocateUtf8String(socketPath);
			return new TachyonRpcBus(TachyonABI.rpcListen(path, capFwd, capRev));
		}
	}

	/**
	 * Attaches to existing SHM arenas via the UNIX socket at {@code socketPath}.
	 *
	 * @param socketPath The UNIX socket path used by the callee.
	 * @return A fully initialized {@code TachyonRpcBus} ready to send requests.
	 * @throws TachyonException with code 14 if the callee was compiled with a
	 *                          different Tachyon version or {@code TACHYON_MSG_ALIGNMENT}.
	 * @throws TachyonException on connection or SHM mapping failure.
	 */
	public static TachyonRpcBus rpcConnect(String socketPath) {
		try (Arena tmp = Arena.ofConfined()) {
			MemorySegment path = tmp.allocateUtf8String(socketPath);
			return new TachyonRpcBus(TachyonABI.rpcConnect(path));
		}
	}

	/**
	 * Copies {@code payload} into {@code arena_fwd} and returns the assigned
	 * correlation ID. Blocks if the ring buffer is full.
	 *
	 * <p>No per-call heap allocation. {@code slotCid} is reused across calls.
	 * Not safe to call concurrently with another {@link #call} invocation.
	 *
	 * @param payload The request bytes to send to the callee.
	 * @param msgType The application-level message type (uint16 range).
	 * @return The correlation ID assigned to this request, always greater than zero.
	 * @throws TachyonException      if the commit fails due to an internal error.
	 * @throws IllegalStateException if the bus has been closed.
	 */
	public long call(byte[] payload, int msgType) {
		checkOpen();
		MemorySegment slot;
		do {
			slot = TachyonABI.rpcAcquireTx(raw, payload.length);
			if (slot.address() == 0) checkFatalError();
		} while (slot.address() == 0);

		slot.reinterpret(payload.length).copyFrom(MemorySegment.ofArray(payload));
		TachyonABI.rpcCommitCall(raw, payload.length, msgType, slotCid);
		return slotCid.get(ValueLayout.JAVA_LONG, 0);
	}

	/**
	 * Blocks until the response matching {@code correlationId} arrives in
	 * {@code arena_rev}, spinning then falling back to futex sleep.
	 *
	 * <p>A correlation ID mismatch triggers a fatal error on {@code arena_rev}.
	 * The returned guard must be committed after use; prefer try-with-resources.
	 *
	 * <p>No per-call heap allocation. {@code slotSize} and {@code slotMsgType}
	 * are reused across calls. Not safe to call concurrently with another
	 * {@link #wait} invocation.
	 *
	 * @param correlationId The ID returned by a prior {@link #call}.
	 * @param spinThreshold The number of {@code cpu_relax()} iterations before
	 *                      the thread falls back to a futex wait.
	 * @return An {@link RpcRxGuard} holding the response payload.
	 * @throws TachyonException      if a fatal error or correlation mismatch is detected.
	 * @throws IllegalStateException if the bus has been closed.
	 */
	public RpcRxGuard waitResponse(long correlationId, int spinThreshold) {
		checkOpen();
		MemorySegment ptr;
		do {
			ptr = TachyonABI.rpcWait(raw, correlationId, slotSize, slotMsgType, spinThreshold);
			if (ptr.address() == 0) checkFatalError();
		} while (ptr.address() == 0);

		return new RpcRxGuard(
				raw,
				ptr,
				slotSize.get(ValueLayout.JAVA_LONG, 0),
				slotMsgType.get(ValueLayout.JAVA_INT, 0),
				correlationId,
				false);
	}

	/**
	 * Blocks until the response matching {@code correlationId} arrives, using the
	 * default spin threshold of {@value #SPIN_DEFAULT} iterations.
	 *
	 * @param correlationId The ID returned by a prior {@link #call}.
	 * @return An {@link RpcRxGuard} holding the response payload.
	 * @throws TachyonException      if a fatal error or correlation mismatch is detected.
	 * @throws IllegalStateException if the bus has been closed.
	 */
	public RpcRxGuard waitResponse(long correlationId) {
		return waitResponse(correlationId, SPIN_DEFAULT);
	}

	/**
	 * Blocks until a request arrives in {@code arena_fwd}, spinning then falling
	 * back to futex sleep.
	 *
	 * <p>Read the payload and correlation ID from the returned guard. Commit the
	 * guard before calling {@link #reply}; holding both the serve slot and a reply
	 * slot simultaneously must be avoided to prevent arena deadlock.
	 *
	 * <p>No per-call heap allocation. {@code slotCid}, {@code slotSize}, and
	 * {@code slotMsgType} are reused across calls. Not safe to call concurrently
	 * with another {@link #serve} invocation.
	 *
	 * @param spinThreshold The number of {@code cpu_relax()} iterations before
	 *                      the thread falls back to a futex wait.
	 * @return An {@link RpcRxGuard} holding the request payload and correlation ID.
	 * @throws TachyonException      if a fatal error is detected on {@code arena_fwd}.
	 * @throws IllegalStateException if the bus has been closed.
	 */
	public RpcRxGuard serve(int spinThreshold) {
		checkOpen();
		MemorySegment ptr;
		do {
			ptr = TachyonABI.rpcServe(raw, slotCid, slotMsgType, slotSize, spinThreshold);
			if (ptr.address() == 0) checkFatalError();
		} while (ptr.address() == 0);

		return new RpcRxGuard(
				raw,
				ptr,
				slotSize.get(ValueLayout.JAVA_LONG, 0),
				slotMsgType.get(ValueLayout.JAVA_INT, 0),
				slotCid.get(ValueLayout.JAVA_LONG, 0),
				true);
	}

	/**
	 * Blocks until a request arrives, using the default spin threshold of
	 * {@value #SPIN_DEFAULT} iterations.
	 *
	 * @return An {@link RpcRxGuard} holding the request payload and correlation ID.
	 * @throws TachyonException      if a fatal error is detected on {@code arena_fwd}.
	 * @throws IllegalStateException if the bus has been closed.
	 */
	public RpcRxGuard serve() {
		return serve(SPIN_DEFAULT);
	}

	/**
	 * Copies {@code payload} into {@code arena_rev} as a response to the request
	 * identified by {@code correlationId}. Blocks if the ring buffer is full.
	 *
	 * <p>The {@link RpcRxGuard} returned by {@link #serve} must be committed before
	 * calling this method.
	 *
	 * @param correlationId The ID from {@link RpcRxGuard#getCorrelationId()}.
	 *                      Must be greater than zero.
	 * @param payload       The response bytes to send to the caller.
	 * @param msgType       The application-level response type (uint16 range).
	 * @throws TachyonException      if {@code correlationId} is zero, the commit fails,
	 *                               or a fatal error is detected on {@code arena_rev}.
	 * @throws IllegalStateException if the bus has been closed.
	 */
	public void reply(long correlationId, byte[] payload, int msgType) {
		checkOpen();
		if (correlationId == 0)
			throw new TachyonException(8, "correlation_id must be non-zero"); // TACHYON_ERR_INVALID_SZ

		MemorySegment slot;
		do {
			slot = TachyonABI.rpcAcquireReplyTx(raw, payload.length);
			if (slot.address() == 0) checkFatalError();
		} while (slot.address() == 0);

		slot.reinterpret(payload.length).copyFrom(MemorySegment.ofArray(payload));
		TachyonABI.rpcCommitReply(raw, correlationId, payload.length, msgType);
	}

	/**
	 * Configures the consumer polling back-off strategy for both {@code arena_fwd}
	 * and {@code arena_rev}.
	 *
	 * <p>Call immediately after construction, before the first message. Using
	 * pure-spin mode when a consumer thread might park will cause it to spin
	 * indefinitely, as the producer will not issue a futex wake.
	 *
	 * @param pureSpin {@code 1} to enable pure-spin mode; {@code 0} to restore
	 *                 the default hybrid futex-sleep mode.
	 * @throws IllegalStateException if the bus has been closed.
	 */
	public void setPollingMode(int pureSpin) {
		checkOpen();
		TachyonABI.rpcSetPollingMode(raw, pureSpin);
	}

	/**
	 * Unmaps both SHM arenas, releases all resources, and closes the pre-allocated
	 * slot arena. Idempotent: subsequent calls after the first are no-ops.
	 */
	@Override
	public void close() {
		if (raw != null) {
			TachyonABI.rpcDestroy(raw);
			raw = null;
			slots.close();
		}
	}

	private void checkOpen() {
		if (raw == null) throw new IllegalStateException("TachyonRpcBus is closed");
	}

	private void checkFatalError() {
		if (TachyonABI.rpcGetState(raw) == STATE_FATAL_ERROR)
			throw new PeerDeadException();
	}
}
