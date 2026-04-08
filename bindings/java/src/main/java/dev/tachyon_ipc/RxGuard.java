package dev.tachyon_ipc;

import java.lang.foreign.MemorySegment;

/**
 * A zero-copy read projection of a single message from the consumer's SPSC arena.
 *
 * @apiNote The memory segment is strictly read-only and bounds-checked. It must not be
 * accessed after the guard is closed or committed.
 */
public final class RxGuard implements AutoCloseable {

	/**
	 * The native bus pointer used for FFI operations.
	 */
	private final MemorySegment busHandle;

	/**
	 * The immutable memory segment pointing to the payload.
	 */
	private MemorySegment data;

	/**
	 * The user-defined protocol identifier for this message payload.
	 */
	private final int typeId;

	/**
	 * The exact number of valid payload bytes.
	 */
	private final long actualSize;

	/**
	 * State flag indicating whether the message has been acknowledged.
	 */
	private boolean consumed;

	/**
	 * Internal constructor invoked exclusively by the {@link TachyonBus}.
	 *
	 * @param busHandle  The native bus pointer.
	 * @param safePtr    The bounded, read-only memory segment containing the message.
	 * @param typeId     The extracted protocol identifier.
	 * @param actualSize The exact payload size.
	 */
	RxGuard(MemorySegment busHandle, MemorySegment safePtr, int typeId, long actualSize) {
		this.busHandle = busHandle;
		this.typeId = typeId;
		this.actualSize = actualSize;
		this.data = safePtr.asReadOnly();
		this.consumed = false;
	}

	/**
	 * Retrieves the read-only memory segment for the payload.
	 *
	 * @return The immutable memory segment pointing to the payload in the shared memory file.
	 * @throws IllegalStateException If the guard has already been committed or closed.
	 */
	public MemorySegment getData() {
		checkState();
		return data;
	}

	/**
	 * Retrieves the protocol identifier associated with this message.
	 *
	 * @return The integer type ID.
	 */
	public int getTypeId() {
		return typeId;
	}

	/**
	 * Retrieves the actual size of the payload.
	 *
	 * @return The size in bytes.
	 */
	public long getActualSize() {
		return actualSize;
	}

	/**
	 * Acknowledges the processing of the message and advances the ring buffer's consumer head.
	 */
	public void commit() {
		checkState();
		this.consumed = true;
		TachyonABI.commitRx(busHandle);
	}

	/**
	 * Finalizes the read lifecycle.
	 *
	 * @implSpec Automatically commits the read transaction if not explicitly consumed,
	 * preventing consumer-side stalls and freeing cache lines for the producer.
	 * The internal memory segment reference is nullified to strictly prevent use-after-free.
	 */
	@Override
	public void close() {
		try {
			if (!consumed) {
				commit();
			}
		} finally {
			this.data = null;
		}
	}

	/**
	 * Validates the temporal lifecycle of the guard.
	 *
	 * @throws IllegalStateException If the guard has already been committed or closed.
	 */
	private void checkState() {
		if (consumed || data == null) {
			throw new IllegalStateException("RxGuard has already been committed or closed");
		}
	}
}
