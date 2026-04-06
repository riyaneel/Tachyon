package dev.tachyon_ipc;

import java.lang.foreign.MemorySegment;

public final class RxMsgView {
	private MemorySegment data;
	private final int typeId;
	private final long actualSize;

	RxMsgView(MemorySegment rawPointer, int typeId, long actualSize) {
		this.typeId = typeId;
		this.actualSize = actualSize;
		this.data = rawPointer.asReadOnly();
	}

	public MemorySegment getData() {
		if (data == null) {
			throw new IllegalStateException("RxMsgView invalidated after batch commit");
		}
		return data;
	}

	public int getTypeId() {
		return typeId;
	}

	public long getActualSize() {
		return actualSize;
	}

	void invalidate() {
		this.data = null;
	}
}
