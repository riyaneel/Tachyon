package dev.tachyon_ipc;

import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;

/**
 * Zero-copy view into an RPC ring buffer slot, covering either {@code arena_fwd}
 * (callee side, produced by {@link TachyonRpcBus#serve}) or {@code arena_rev}
 * (caller side, produced by {@link TachyonRpcBus#wait}).
 *
 * <p>The slot is valid until {@link #commit()} is called. Implements
 * {@link AutoCloseable} so it can be used in a try-with-resources block, which
 * guarantees the slot is released even when an exception is thrown.
 *
 * <p>{@link #getData()} copies the payload bytes and is safe to retain after
 * {@link #commit()}. {@link #segment()} exposes the raw zero-copy view and is
 * valid only until {@link #commit()} is called.
 *
 * @see TachyonRpcBus#waitResponse(long, int)
 * @see TachyonRpcBus#serve(int)
 */
public final class RpcRxGuard implements AutoCloseable {

	private final MemorySegment raw;
	private final MemorySegment ptr;
	private final long          actualSize;
	private final int           msgType;
	private final long          correlationId;
	private final boolean       isServe;
	private       boolean       committed;

	RpcRxGuard(
			MemorySegment raw,
			MemorySegment ptr,
			long          actualSize,
			int           msgType,
			long          correlationId,
			boolean       isServe) {
		this.raw           = raw;
		this.ptr           = ptr.reinterpret(actualSize);
		this.actualSize    = actualSize;
		this.msgType       = msgType;
		this.correlationId = correlationId;
		this.isServe       = isServe;
		this.committed     = false;
	}

	/**
	 * Copies the payload bytes into a new array.
	 *
	 * <p>The returned array is owned by the caller and remains valid after
	 * {@link #commit()} is called.
	 *
	 * @return A fresh {@code byte[]} containing the full message payload.
	 * @throws IllegalStateException if the slot has already been committed.
	 */
	public byte[] getData() {
		if (committed) throw new IllegalStateException("RpcRxGuard already committed");
		return ptr.toArray(ValueLayout.JAVA_BYTE);
	}

	/**
	 * Returns a zero-copy {@link MemorySegment} view of the payload.
	 *
	 * <p>The segment points directly into shared memory and is valid only until
	 * {@link #commit()} is called. Do not retain references across commit.
	 *
	 * @return The bounded memory segment backed by the ring buffer slot.
	 * @throws IllegalStateException if the slot has already been committed.
	 */
	public MemorySegment segment() {
		if (committed) throw new IllegalStateException("RpcRxGuard already committed");
		return ptr;
	}

	/**
	 * Returns the actual payload size in bytes.
	 *
	 * @return The number of payload bytes written by the sender.
	 */
	public long getActualSize() {
		return actualSize;
	}

	/**
	 * Returns the application-level message type set by the sender.
	 *
	 * @return The message type discriminator (uint16 range).
	 */
	public int getMsgType() {
		return msgType;
	}

	/**
	 * Returns the correlation ID of this message.
	 *
	 * <p>On the callee side (produced by {@link TachyonRpcBus#serve}), this value
	 * must be passed unchanged to {@link TachyonRpcBus#reply} to associate the
	 * response with the correct pending caller.
	 *
	 * @return The correlation ID assigned by the caller, always greater than zero.
	 */
	public long getCorrelationId() {
		return correlationId;
	}

	/**
	 * Releases the ring buffer slot back to the arena and advances the consumer tail.
	 *
	 * <p>Idempotent: subsequent calls after the first are no-ops.
	 *
	 * @throws TachyonException if the underlying C commit fails.
	 */
	public void commit() {
		if (committed) return;
		committed = true;
		if (isServe) {
			TachyonABI.rpcCommitServe(raw);
		} else {
			TachyonABI.rpcCommitRx(raw);
		}
	}

	/**
	 * Implements {@link AutoCloseable} by delegating to {@link #commit()}.
	 *
	 * <p>Using this guard in a try-with-resources block guarantees the slot is
	 * released regardless of whether an exception is thrown.
	 */
	@Override
	public void close() {
		commit();
	}
}
