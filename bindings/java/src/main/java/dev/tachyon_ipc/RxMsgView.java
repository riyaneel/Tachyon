package dev.tachyon_ipc;

import java.lang.foreign.MemorySegment;

/**
 * A lightweight, read-only projection of a single message scoped to the lifecycle of an {@link RxBatchGuard}.
 *
 * @implNote Points directly to the shared memory ring buffer. Access is strictly forbidden
 * once the parent batch is committed.
 */
public final class RxMsgView {

	/**
	 * The immutable memory segment pointing to the payload in the shared arena.
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
	 * Internal constructor invoked exclusively by the parent {@link RxBatchGuard} during batch initialization.
	 *
	 * @param rawPointer The raw memory segment bound to the parent's confined arena.
	 * @param typeId     The extracted protocol identifier.
	 * @param actualSize The exact payload size in bytes.
	 */
	RxMsgView(MemorySegment rawPointer, int typeId, long actualSize) {
		this.typeId = typeId;
		this.actualSize = actualSize;
		this.data = rawPointer.asReadOnly();
	}

	/**
	 * Retrieves the read-only memory segment for the payload.
	 *
	 * @return The bounds-checked immutable memory segment of the payload.
	 * @throws IllegalStateException If the parent batch has already been committed, invalidating the memory mapping.
	 */
	public MemorySegment getData() {
		if (data == null) {
			throw new IllegalStateException("RxMsgView invalidated after batch commit");
		}
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
	 * Nullifies the internal segment pointer to violently fault any subsequent read attempts.
	 * * @implSpec Invoked strictly by the parent {@link RxBatchGuard} during the commit phase
	 * to enforce zero-copy lifetime constraints.
	 */
	void invalidate() {
		this.data = null;
	}
}
