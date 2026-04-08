package dev.tachyon_ipc;

import java.lang.foreign.Arena;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.util.Iterator;
import java.util.NoSuchElementException;
import java.util.Objects;
import java.util.Spliterator;
import java.util.Spliterators;
import java.util.function.Consumer;

/**
 * A high-throughput batch lease that drains multiple contiguous messages in a single FFI crossing.
 *
 * @implSpec Backed by a confined FFM Arena holding the native C-struct array. Committing the batch
 * advances the consumer head for all processed messages simultaneously.
 */
public final class RxBatchGuard implements AutoCloseable, Iterable<RxMsgView> {

	/**
	 * The native bus pointer used for FFI operations.
	 */
	private final MemorySegment busHandle;

	/**
	 * The confined memory arena governing the lifecycle of the native struct array and its projections.
	 */
	private final Arena arena;

	/**
	 * The absolute number of messages successfully extracted from the ring buffer.
	 */
	private final long count;

	/**
	 * The array of object-oriented views mapping to the native memory segments.
	 */
	private final RxMsgView[] views;

	/**
	 * The raw FFM memory segment representing the contiguous array of {@code tachyon_msg_view_t} structs.
	 */
	private final MemorySegment viewsArray;

	/**
	 * State flag indicating whether the batch has been acknowledged and the consumer head advanced.
	 */
	private boolean consumed;

	/**
	 * Internal constructor invoked exclusively by the {@link TachyonBus}.
	 * Allocates the layout and invokes the native ABI to populate the structs.
	 *
	 * @param busHandle     The native bus pointer.
	 * @param busArena      The shared arena of the bus used to reinterpret raw pointers.
	 * @param maxMsgs       The upper limit of messages to extract.
	 * @param spinThreshold The CPU yield threshold before blocking.
	 * @throws IllegalStateException If the native ABI returns a count exceeding the requested bounds.
	 */
	RxBatchGuard(MemorySegment busHandle, Arena busArena, int maxMsgs, int spinThreshold) {
		this.busHandle = busHandle;
		this.consumed = false;
		this.arena = Arena.ofConfined();

		try {
			MemoryLayout arrayLayout = MemoryLayout.sequenceLayout(maxMsgs, MsgViewLayout.layout);
			this.viewsArray = arena.allocate(arrayLayout);
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
					MemorySegment viewStruct = viewsArray.asSlice(offset, MsgViewLayout.sizeBytes);
					MemorySegment ptr = (MemorySegment) MsgViewLayout.ptrHandle.get(viewStruct);
					long actualSize = (long) MsgViewLayout.sizeHandle.get(viewStruct);
					int typeId = (int) MsgViewLayout.typeHandle.get(viewStruct);
					MemorySegment safePtr = ptr.reinterpret(actualSize, busArena, null);
					this.views[i] = new RxMsgView(safePtr, typeId, actualSize);
				}
			}
		} catch (Throwable throwable) {
			arena.close();
			throw throwable;
		}
	}

	/**
	 * Retrieves the number of valid messages loaded into this batch.
	 *
	 * @return The total message count.
	 */
	public long getCount() {
		return count;
	}

	/**
	 * Retrieves a specific message view from the batch by its index.
	 *
	 * @param index The zero-based index of the message within the batch.
	 * @return The corresponding {@link RxMsgView}.
	 * @throws IllegalStateException          If the batch has already been committed or closed.
	 * @throws ArrayIndexOutOfBoundsException If the index is out of bounds.
	 */
	public RxMsgView get(int index) {
		checkState();
		return views[index];
	}

	/**
	 * Submits the entire batch of messages to the native arena, advancing the consumer head
	 * in a single atomic operation.
	 *
	 * @implSpec Iterates through all instantiated {@link RxMsgView} references and invalidates
	 * their memory segments to strictly enforce zero-copy lifetime constraints and prevent use-after-free.
	 * Finally, closes the confined struct arena.
	 */
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

	/**
	 * Finalizes the batch lifecycle.
	 *
	 * @implSpec Automatically commits the batch if not explicitly consumed, ensuring the
	 * consumer head does not stall the producer arena.
	 */
	@Override
	public void close() {
		if (!consumed) {
			commit();
		}
	}

	/**
	 * Provides an iterator over the valid messages in this batch.
	 *
	 * @return An {@link Iterator} of {@link RxMsgView}.
	 * @throws IllegalStateException If the batch has already been committed.
	 */
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

	/**
	 * Performs the given action for each element of the batch until all elements have been processed.
	 *
	 * @param action The action to be performed for each message view.
	 * @throws NullPointerException  If the specified action is null.
	 * @throws IllegalStateException If the batch is committed during iteration.
	 */
	@Override
	public void forEach(Consumer<? super RxMsgView> action) {
		Objects.requireNonNull(action);
		for (RxMsgView view : views) {
			checkState();
			action.accept(view);
		}
	}

	/**
	 * Creates a Spliterator over the message views in this batch.
	 *
	 * @return A {@link Spliterator} with ORDERED and IMMUTABLE characteristics.
	 * @throws IllegalStateException If the batch has already been committed.
	 */
	@Override
	public Spliterator<RxMsgView> spliterator() {
		checkState();
		return Spliterators.spliterator(
				views,
				Spliterator.ORDERED | Spliterator.SIZED | Spliterator.SUBSIZED | Spliterator.NONNULL | Spliterator.IMMUTABLE
		);
	}

	/**
	 * Validates the temporal lifecycle of the batch guard.
	 *
	 * @throws IllegalStateException If the guard has already been committed or closed.
	 */
	private void checkState() {
		if (consumed || !arena.scope().isAlive()) {
			throw new IllegalStateException("RxBatchGuard has already been committed or closed");
		}
	}
}
