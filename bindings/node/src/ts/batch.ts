import type { RxSlot } from './guards';
import { PeerDeadError } from './error';

/** @internal */
export interface BatchController {
	commitBatch(): void;

	getState(): number;
}

/** A single message inside an {@link RxBatch}. Valid only until the batch is committed. */
export interface RxMessage {
	readonly data: RxSlot;
	readonly typeId: number;
	readonly size: number;
}

/**
 * Zero-copy batch of ring buffer slots drained in a single native call.
 *
 * Obtain via {@link Bus.drainBatch}. Iterate the messages then call {@link commit}.
 * `using` commits automatically.
 *
 * All `RxMessage.data` references are invalidated on commit — any cached reference
 * will throw `TypeError` (underlying ArrayBuffers are detached by the C++ side).
 *
 * @example
 * ```ts
 * using batch = bus.drainBatch(64);
 * for (const msg of batch) {
 *     process(msg.data, msg.typeId);
 * }
 * ```
 */
export class RxBatch {
	#ctrl: BatchController;
	#messages: RxMessage[];
	#done = false;

	/** @internal */
	constructor(ctrl: BatchController, messages: RxMessage[]) {
		this.#ctrl = ctrl;
		this.#messages = messages;
	}

	/** Number of messages in this batch. */
	get length(): number {
		return this.#messages.length;
	}

	/**
	 * Returns the message at index `i`.
	 *
	 * @throws {RangeError} If `i` is out of bounds.
	 * @throws {Error} If the batch has already been committed.
	 * @throws {PeerDeadError} If the bus has transitioned to TACHYON_STATE_FATAL_ERROR.
	 */
	at(i: number): RxMessage {
		this.#assertOpen();
		if (this.#ctrl.getState() === 4 /* TACHYON_STATE_FATAL_ERROR */) throw new PeerDeadError();
		if (i < 0 || i >= this.#messages.length)
			throw new RangeError(`RxBatch: index ${i} out of range [0, ${this.#messages.length}).`);
		return this.#messages[i]!;
	}

	/**
	 * Iterates over all messages. Stops immediately if the batch is committed mid-iteration.
	 *
	 * @throws {PeerDeadError} If the bus has transitioned to TACHYON_STATE_FATAL_ERROR.
	 */
	[Symbol.iterator](): Iterator<RxMessage> {
		let i = 0;
		return {
			next: (): IteratorResult<RxMessage> => {
				if (this.#done || i >= this.#messages.length) return { value: undefined, done: true };
				if (this.#ctrl.getState() === 4) throw new PeerDeadError();
				return { value: this.#messages[i++]!, done: false };
			},
		};
	}

	/** Advances the consumer head and detaches all slot ArrayBuffers. No-op if already committed. */
	commit(): void {
		if (this.#done) return;
		this.#done = true;
		this.#messages = [];
		this.#ctrl.commitBatch();
	}

	/** Called automatically by the `using` keyword. Commits if not already released. */
	[Symbol.dispose](): void {
		this.commit();
	}

	#assertOpen(): void {
		if (this.#done) throw new Error('RxBatch: batch has already been committed.');
	}
}
