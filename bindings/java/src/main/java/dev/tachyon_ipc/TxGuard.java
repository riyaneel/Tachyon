package dev.tachyon_ipc;

import java.lang.foreign.MemorySegment;

public final class TxGuard implements AutoCloseable {
	private final MemorySegment busHandle;
	private MemorySegment slot;
	private boolean consumed;

	TxGuard(MemorySegment busHandle, MemorySegment safePtr) {
		this.busHandle = busHandle;
		this.slot = safePtr;
		this.consumed = false;
	}

	public MemorySegment getData() {
		checkState();
		return slot;
	}

	public void commit(long actualSize, int typeId) {
		checkState();
		this.consumed = true;
		TachyonABI.commitTx(busHandle, actualSize, typeId);
	}

	public void commitUnflushed(long actualSize, int typeId) {
		checkState();
		this.consumed = true;
		TachyonABI.commitTxUnflushed(busHandle, actualSize, typeId);
	}

	public void rollback() {
		checkState();
		this.consumed = true;
		TachyonABI.rollbackTx(busHandle);
	}

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

	private void checkState() {
		if (consumed || slot == null) {
			throw new IllegalStateException("TxGuard has already been consumed or closed");
		}
	}
}
