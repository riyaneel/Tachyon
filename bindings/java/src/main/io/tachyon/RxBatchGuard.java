package io.tachyon;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.util.Iterator;
import java.util.NoSuchElementException;
import java.util.Objects;
import java.util.Spliterator;
import java.util.Spliterators;
import java.util.function.Consumer;

public final class RxBatchGuard implements AutoCloseable, Iterable<RxMsgView> {

	private final MemorySegment busHandle;
	private final Arena arena;
	private final long count;
	private final RxMsgView[] views;
	private final MemorySegment viewsArray;
	private boolean consumed;

	RxBatchGuard(MemorySegment busHandle, int maxMsgs, int spinThreshold) {
		this.busHandle = busHandle;
		this.consumed = false;
		this.arena = Arena.ofConfined();

		try {
			this.viewsArray = arena.allocate(MsgViewLayout.sizeBytes * maxMsgs);
			this.count = TachyonABI.drainBatch(busHandle, viewsArray, maxMsgs, spinThreshold);

			if (this.count < 0 || this.count > maxMsgs) {
				throw new IllegalStateException("Invalid batch count returned from C ABI: " + this.count);
			}

			if (this.count == 0) {
				this.views = new RxMsgView[0];
			} else {
				this.views = new RxMsgView[(int) this.count];
				for (int i = 0; i < this.count; i++) {
					long offset = i * MsgViewLayout.sizeBytes;
					MemorySegment ptr = (MemorySegment) MsgViewLayout.ptrHandle.get(viewsArray, offset);
					long actualSize = (long) MsgViewLayout.sizeHandle.get(viewsArray, offset);
					int typeId = (int) MsgViewLayout.typeHandle.get(viewsArray, offset);

					this.views[i] = new RxMsgView(ptr, typeId, actualSize);
				}
			}
		} catch (Throwable throwable) {
			arena.close();
			throw throwable;
		}
	}

	public long getCount() {
		return count;
	}

	public RxMsgView get(int index) {
		checkState();
		return views[index];
	}

	public void commit() {
		checkState();
		this.consumed = true;

		try {
			if (count > 0) {
				TachyonABI.commitRxBatch(busHandle, viewsArray, count);
			}
		} finally {
			for (RxMsgView view : views) {
				view.invalidate();
			}

			arena.close();
		}
	}

	@Override
	public void close() {
		if (!consumed) {
			commit();
		}
	}

	@Override
	public Iterator<RxMsgView> iterator() {
		checkState();
		return new Iterator<>() {
			private int currentIndex = 0;

			@Override
			public boolean hasNext() {
				checkState();
				return currentIndex < views.length;
			}

			@Override
			public RxMsgView next() {
				if (!hasNext()) {
					throw new NoSuchElementException();
				}

				return views[currentIndex++];
			}
		};
	}

	@Override
	public void forEach(Consumer<? super RxMsgView> action) {
		Objects.requireNonNull(action);
		for (RxMsgView view : views) {
			checkState();
			action.accept(view);
		}
	}

	@Override
	public Spliterator<RxMsgView> spliterator() {
		checkState();
		return Spliterators.spliterator(
				views,
				Spliterator.ORDERED | Spliterator.SIZED | Spliterator.SUBSIZED | Spliterator.NONNULL | Spliterator.IMMUTABLE
		);
	}

	private void checkState() {
		if (consumed || !arena.scope().isAlive()) {
			throw new IllegalStateException("RxBatchGuard has already been committed or closed");
		}
	}
}
