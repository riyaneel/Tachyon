package io.tachyon;

import java.lang.foreign.MemorySegment;

public final class RxGuard implements AutoCloseable {
	private final MemorySegment busHandle;
	private MemorySegment data;
	private final int typeId;
	private final long actualSize;
	private boolean consumed;

	RxGuard(MemorySegment busHandle, MemorySegment safePtr, int typeId, long actualSize) {
		this.busHandle = busHandle;
		this.typeId = typeId;
		this.actualSize = actualSize;
		this.data = safePtr.asReadOnly();
		this.consumed = false;
	}

	public MemorySegment getData() {
		checkState();
		return data;
	}

	public int getTypeId() {
		return typeId;
	}

	public long getActualSize() {
		return actualSize;
	}

	public void commit() {
		checkState();
		this.consumed = true;
		TachyonABI.commitRx(busHandle);
	}

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

	private void checkState() {
		if (consumed || data == null) {
			throw new IllegalStateException("RxGuard has already been committed or closed");
		}
	}
}
