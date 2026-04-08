package dev.tachyon_ipc;

import java.lang.foreign.MemorySegment;

/**
 * An exclusive, zero-copy lease on a contiguous memory slot within the producer's SPSC arena.
 *
 * @apiNote This guard must be either explicitly committed or closed. Leaking an uncommitted
 * guard will stall the ring buffer as the producer head cannot safely advance.
 */
public final class TxGuard implements AutoCloseable {

	/**
	 * The native bus pointer used for FFI operations.
	 */
	private final MemorySegment busHandle;

	/**
	 * The spatially bounded memory segment directly mapped to the shared ring buffer.
	 */
	private MemorySegment slot;

	/**
	 * State flag indicating whether the transaction has been finalized (committed or rolled back).
	 */
	private boolean consumed;

	/**
	 * Internal constructor invoked exclusively by the {@link TachyonBus}.
	 *
	 * @param busHandle The native bus pointer.
	 * @param safePtr   The bounded memory segment ready for zero-copy writes.
	 */
	TxGuard(MemorySegment busHandle, MemorySegment safePtr) {
		this.busHandle = busHandle;
		this.slot = safePtr;
		this.consumed = false;
	}

	/**
	 * Retrieves the writable memory segment for the payload.
	 *
	 * @return The spatially bounded memory segment directly mapped to the shared ring buffer.
	 * @throws IllegalStateException If the transaction has already been consumed or rolled back.
	 */
	public MemorySegment getData() {
		checkState();
		return slot;
	}

	/**
	 * Commits the transaction, advances the producer head, and explicitly issues a memory
	 * barrier/flush to wake up any sleeping consumers.
	 *
	 * @param actualSize The exact number of bytes written to the slot.
	 * @param typeId     A user-defined protocol identifier for this message payload.
	 */
	public void commit(long actualSize, int typeId) {
		checkState();
		this.consumed = true;
		TachyonABI.commitTx(busHandle, actualSize, typeId);
	}

	/**
	 * Commits the transaction without issuing a memory barrier or wake-up signal.
	 *
	 * @param actualSize The exact number of bytes written to the slot.
	 * @param typeId     A user-defined protocol identifier for this message payload.
	 * @implNote Designed for burst-throughput scenarios where multiple messages
	 * are pushed in succession, deferring the flush syscall until the final message.
	 */
	public void commitUnflushed(long actualSize, int typeId) {
		checkState();
		this.consumed = true;
		TachyonABI.commitTxUnflushed(busHandle, actualSize, typeId);
	}

	/**
	 * Aborts the transaction and relinquishes the memory slot without advancing the producer head.
	 */
	public void rollback() {
		checkState();
		this.consumed = true;
		TachyonABI.rollbackTx(busHandle);
	}

	/**
	 * Finalizes the transaction lifecycle.
	 *
	 * @implSpec Automatically triggers a rollback if the transaction has not been explicitly
	 * committed, ensuring the arena does not deadlock on early exits or exceptions.
	 * The internal memory segment reference is nullified to strictly prevent use-after-free.
	 */
	@Override
	public void close() {
		try {
			if (!consumed) {
				rollback();
			}
		} finally {
			this.slot = null;
		}
	}

	/**
	 * Validates the temporal lifecycle of the guard.
	 *
	 * @throws IllegalStateException If the guard has already been consumed or closed.
	 */
	private void checkState() {
		if (consumed || slot == null) {
			throw new IllegalStateException("TxGuard has already been consumed or closed");
		}
	}
}
