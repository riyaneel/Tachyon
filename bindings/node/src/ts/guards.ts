import { PeerDeadError } from './error.ts';

// Branded slot types, nominal subtypes of Buffer.
// Brand symbols are not accessible outside this module, so only
// TxGuard and RxGuard can produce these types.
declare const txSlotBrand: unique symbol;
declare const rxSlotBrand: unique symbol;

/** Zero-copy write window into the ring buffer. Valid only until commit or rollback. */
export type TxSlot = Buffer & { readonly [txSlotBrand]: true };

/** Zero-copy read window into the ring buffer. Valid only until commit. */
export type RxSlot = Buffer & { readonly [rxSlotBrand]: true };

/** @internal */
export interface TxController {
	commitTx(actualSize: number, typeId: number): void;

	commitTxUnflushed(actualSize: number, typeId: number): void;

	rollbackTx(): void;
}

/** @internal */
export interface RxController {
	commitRx(): void;

	getState(): number;
}

/**
 * Exclusive write lease on a ring buffer slot.
 *
 * Obtain via {@link Bus.acquireTx}. Finalize with {@link commit}, {@link commitUnflushed},
 * or {@link rollback}. Failing to do so holds the producer lock indefinitely.
 * `using` rolls back automatically if the slot was not already committed.
 *
 * @example
 * ```ts
 * using tx = bus.acquireTx(32);
 * tx.bytes().write('hello', 0);
 * tx.commit(5, 1);
 * ```
 */
export class TxGuard {
	#ctrl: TxController;
	#buffer: TxSlot | null;
	#done = false;

	/** @internal */
	public constructor(ctrl: TxController, buffer: Buffer) {
		this.#ctrl = ctrl;
		this.#buffer = buffer as TxSlot;
	}

	/**
	 * Returns the writable zero-copy window into shared memory.
	 * The reference is invalidated on commit or rollback, any cached reference
	 * will throw `TypeError` on subsequent access (underlying ArrayBuffer detached).
	 *
	 * @throws {Error} If the slot has already been finalized.
	 */
	public bytes(): TxSlot {
		if (this.#done || this.#buffer === null) {
			throw new Error('TxGuard: slot has already been committed or rolled back.');
		}

		return this.#buffer;
	}

	/**
	 * Publishes `actualSize` bytes with `typeId` and flushes immediately.
	 * Use for single-message sends; prefer {@link commitUnflushed} + {@link Bus.flush} for batches.
	 *
	 * @throws {Error} If the slot has already been finalized.
	 */
	public commit(actualSize: number, typeId: number): void {
		this.#assertOpen();
		this.#invalidate();
		this.#ctrl.commitTx(actualSize, typeId);
	}

	/**
	 * Publishes without flushing. Call {@link Bus.flush} after the last message in the batch.
	 *
	 * @throws {Error} If the slot has already been finalized.
	 */
	public commitUnflushed(actualSize: number, typeId: number): void {
		this.#assertOpen();
		this.#invalidate();
		this.#ctrl.commitTxUnflushed(actualSize, typeId);
	}

	/** Cancels the transaction without publishing. No-op if already finalized. */
	public rollback(): void {
		if (this.#done) return;
		this.#invalidate();
		this.#ctrl.rollbackTx();
	}

	/** Called automatically by the `using` keyword. Rolls back if not already committed. */
	public [Symbol.dispose](): void {
		this.rollback();
	}

	#assertOpen(): void {
		if (this.#done) throw new Error('TxGuard: slot has already been committed or rolled back.');
	}

	#invalidate(): void {
		this.#done = true;
		this.#buffer = null;
	}
}

/**
 * Zero-copy read lease on a ring buffer slot.
 *
 * Obtain via {@link Bus.acquireRx}. Release with {@link commit} once the payload is consumed.
 * Failing to do so stalls the consumer head indefinitely.
 * `using` commits automatically.
 *
 * @example
 * ```ts
 * using rx = bus.acquireRx();
 * if (rx === null) return; // EINTR
 * process(rx.data());
 * ```
 */
export class RxGuard {
	#ctrl: RxController;
	#buffer: RxSlot | null;
	#done = false;

	/** Message type discriminator set by the producer. */
	public readonly typeId: number;

	/** Exact payload size in bytes. */
	public readonly actualSize: number;

	/** @internal */
	public constructor(ctrl: RxController, buffer: Buffer, typeId: number, actualSize: number) {
		this.#ctrl = ctrl;
		this.#buffer = buffer as RxSlot;
		this.typeId = typeId;
		this.actualSize = actualSize;
	}

	/**
	 * Returns the read-only zero-copy window into shared memory.
	 * The reference is invalidated on commit, any cached reference will throw `TypeError`.
	 *
	 * @throws {Error} If the slot has already been committed.
	 * @throws {PeerDeadError} If the bus has transitioned to TACHYON_STATE_FATAL_ERROR.
	 */
	public data(): RxSlot {
		if (this.#done || this.#buffer === null) {
			throw new Error('RxGuard: slot has already been committed.');
		}

		if (this.#ctrl.getState() === 4 /* TACHYON_STATE_FATAL_ERROR */) throw new PeerDeadError();

		return this.#buffer;
	}

	/** Releases the slot and advances the consumer head. No-op if already committed. */
	public commit(): void {
		if (this.#done) return;
		this.#invalidate();
		this.#ctrl.commitRx();
	}

	/** Called automatically by the `using` keyword. Commits if not already released. */
	public [Symbol.dispose](): void {
		this.commit();
	}

	#assertOpen(): void {
		if (this.#done) throw new Error('RxGuard: slot has already been committed.');
	}

	#invalidate(): void {
		this.#done = true;
		this.#buffer = null;
	}
}
