import { PeerDeadError } from './error.ts';

/** A detached ArrayBuffer (WASM) reports zero byteLength; a live slot never does. */
function assertAttached(buffer: Uint8Array, what: string): void {
	if (buffer.byteLength === 0) {
		throw new Error(`${what}: the underlying buffer has been detached.`);
	}
}

// Branded slot types, nominal subtypes of the underlying byte buffer.
// Brand symbols are not accessible outside this module, so only
// TxGuard and RxGuard can produce these types.
declare const txSlotBrand: unique symbol;
declare const rxSlotBrand: unique symbol;

/**
 * Zero-copy write window into the ring buffer. Valid only until commit or rollback.
 *
 * The backing type is platform-specific: native Node exposes a `Buffer`, while the
 * browser WASM transport exposes a plain `Uint8Array`. The window is never cast across
 * those shapes, so browser code can never call a Node-only `Buffer` method on it.
 */
export type TxSlot<S extends Uint8Array = Uint8Array> = S & { readonly [txSlotBrand]: true };

/** Zero-copy read window into the ring buffer. Valid only until commit. */
export type RxSlot<S extends Uint8Array = Uint8Array> = S & { readonly [rxSlotBrand]: true };

/** @internal */
export interface TxController {
	assertOpen?(): void;
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
export class TxGuard<S extends Uint8Array = Uint8Array> {
	#ctrl: TxController;
	#buffer: TxSlot<S> | null;
	#done = false;

	/** @internal */
	public constructor(ctrl: TxController, buffer: S) {
		this.#ctrl = ctrl;
		this.#buffer = buffer as TxSlot<S>;
	}

	/**
	 * Returns the writable zero-copy window into shared memory.
	 * The reference is invalidated on commit or rollback, cached browser views must not be used afterward. WASM memory cannot
	 * detach individual slots, and memory growth can detach earlier views.
	 *
	 * @throws {Error} If the slot has already been finalized.
	 */
	public bytes(): TxSlot<S> {
		if (this.#done || this.#buffer === null) {
			throw new Error('TxGuard: slot has already been committed or rolled back.');
		}

		this.#ctrl.assertOpen?.();
		assertAttached(this.#buffer, 'TxGuard');
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
		this.#ctrl.commitTx(actualSize, typeId);
		this.#invalidate();
	}

	/**
	 * Publishes without flushing. Call {@link Bus.flush} after the last message in the batch.
	 *
	 * @throws {Error} If the slot has already been finalized.
	 */
	public commitUnflushed(actualSize: number, typeId: number): void {
		this.#assertOpen();
		this.#ctrl.commitTxUnflushed(actualSize, typeId);
		this.#invalidate();
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
export class RxGuard<S extends Uint8Array = Uint8Array> {
	#ctrl: RxController;
	#buffer: RxSlot<S> | null;
	#done = false;

	/** Message type discriminator set by the producer. */
	public readonly typeId: number;

	/** Exact payload size in bytes. */
	public readonly actualSize: number;

	/** @internal */
	public constructor(ctrl: RxController, buffer: S, typeId: number, actualSize: number) {
		this.#ctrl = ctrl;
		this.#buffer = buffer as RxSlot<S>;
		this.typeId = typeId;
		this.actualSize = actualSize;
	}

	/**
	 * Returns the read-only zero-copy window into shared memory.
	 * The reference is invalidated on commit, cached browser views must not be used afterward.
	 *
	 * @throws {Error} If the slot has already been committed.
	 * @throws {PeerDeadError} If the bus has transitioned to TACHYON_STATE_FATAL_ERROR.
	 */
	public data(): RxSlot<S> {
		this.#assertOpen();
		if (this.#ctrl.getState() === 4 /* TACHYON_STATE_FATAL_ERROR */) throw new PeerDeadError();
		// eslint-disable-next-line @typescript-eslint/no-non-null-assertion
		assertAttached(this.#buffer!, 'RxGuard');
		// eslint-disable-next-line @typescript-eslint/no-non-null-assertion
		return this.#buffer!;
	}

	/** Releases the slot and advances the consumer head.
	 *
	 * @throws {Error} If the slot has already been committed.
	 */
	public commit(): void {
		this.#assertOpen();
		this.#invalidate();
		this.#ctrl.commitRx();
	}

	/** Called automatically by the `using` keyword. Commits if not already released. */
	public [Symbol.dispose](): void {
		if (!this.#done) this.commit();
	}

	#assertOpen(): void {
		if (this.#done) throw new Error('RxGuard: slot has already been committed.');
	}

	#invalidate(): void {
		this.#done = true;
		this.#buffer = null;
	}
}
