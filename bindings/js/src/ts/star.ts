import { createRequire } from 'node:module';
import { isMainThread } from 'node:worker_threads';

const _require = createRequire(import.meta.url);

interface NativeStarPollItem {
	data: Buffer;
	typeId: number;
	actualSize: number;
	spokeIdx: number;
}

interface NativeStarBinding {
	close(): void;
	poll(maxTotal: number, budgetUs: number): NativeStarPollItem[];
	commit(): void;
	acquireTx(spokeIdx: number, maxSize: number): Buffer | null;
	commitTx(spokeIdx: number, actualSize: number, typeId: number): void;
	rollbackTx(spokeIdx: number): void;
	flush(spokeIdx: number): void;
	getState(spokeIdx: number): number;
	nSpokes(): number;
}

interface NativeStarModule {
	TachyonStarBusNode: {
		create(buses: object[], nodeIds?: number[] | null): NativeStarBinding;
	};
}

function loadNative(): NativeStarModule {
	const candidates = [
		new URL('../build/Release/tachyon_node.node', import.meta.url).pathname,
		new URL('../build/Debug/tachyon_node.node', import.meta.url).pathname,
		new URL('../../build/Release/tachyon_node.node', import.meta.url).pathname,
		new URL('../../build/Debug/tachyon_node.node', import.meta.url).pathname,
	];

	for (const p of candidates) {
		try {
			return _require(p) as NativeStarModule;
		} catch {}
	}

	throw new Error(
		'tachyon_node.node not found. Run `npm run build:native` first.\n' + `Searched: ${candidates.join(', ')}`,
	);
}

const native = loadNative();

function warnMainThread(method: string): void {
	if (isMainThread) {
		console.warn(
			`[tachyon] StarBus.${method}() called on the main thread. ` +
				'Blocking poll calls will saturate the event loop. ' +
				'Consider moving IPC to a Worker.',
		);
	}
}

/**
 * One message received from a specific spoke during a {@link StarPollGuard} poll.
 */
export interface StarMsgView {
	readonly data: Buffer;
	readonly typeId: number;
	readonly actualSize: number;
	readonly spokeIdx: number;
}

/**
 * Holds the zero-copy batch returned by {@link StarBus.poll}.
 *
 * All {@link StarMsgView.data} buffers point directly into shared memory and are
 * invalidated on {@link commit}. Commit releases the ring-buffer slots across
 * all polled spokes.
 *
 * Use `using` or call {@link commit} explicitly before the next {@link StarBus.poll}.
 *
 * @example
 * ```ts
 * using guard = star.poll(64, 5_000n);
 * for (const msg of guard.messages) {
 *   process(msg.data, msg.spokeIdx);
 * }
 * // guard auto-commits on scope exit
 * ```
 */
export class StarPollGuard implements Disposable {
	readonly #handle: NativeStarBinding;
	readonly #messages: StarMsgView[];
	#committed = false;

	/** @internal */
	public constructor(handle: NativeStarBinding, messages: StarMsgView[]) {
		this.#handle = handle;
		this.#messages = messages;
	}

	/** All messages in this poll batch. */
	public get messages(): readonly StarMsgView[] {
		return this.#messages;
	}

	/** Number of messages in this batch. */
	public count(): number {
		return this.#messages.length;
	}

	/** `true` if no messages were drained. */
	public isEmpty(): boolean {
		return this.#messages.length === 0;
	}

	/**
	 * Advances consumer tails for all polled spokes and invalidates all
	 * {@link StarMsgView.data} buffers (ArrayBuffers are detached).
	 */
	public commit(): void {
		if (this.#committed) return;
		this.#committed = true;
		if (this.#messages.length > 0) {
			this.#handle.commit();
		}
	}

	/** Called automatically by the `using` keyword. */
	public [Symbol.dispose](): void {
		this.commit();
	}
}

/**
 * Holds an exclusive zero-copy TX slot in the producer arena of a specific spoke.
 * Write the payload directly into {@link slot}, then call {@link commit} or
 * {@link rollback}. `using` rolls back automatically if not yet committed.
 * {@link commit} calls `tachyon_star_commit_tx`, which flushes internally.
 *
 * @example
 * ```ts
 * using tx = star.acquireTx(0, 8);
 * if (tx) {
 *   tx.slot.writeBigUInt64BE(value, 0);
 *   tx.commit(8, MSG_TYPE);
 * }
 * ```
 */
export class StarTxGuard implements Disposable {
	readonly #handle: NativeStarBinding;
	readonly #spokeIdx: number;
	readonly #slot: Buffer;
	#consumed = false;

	/** @internal */
	public constructor(handle: NativeStarBinding, spokeIdx: number, slot: Buffer) {
		this.#handle = handle;
		this.#spokeIdx = spokeIdx;
		this.#slot = slot;
	}

	/**
	 * Writable buffer pointing directly into shared memory.
	 * Invalid after {@link commit} or {@link rollback}.
	 *
	 * @throws {Error} If the guard has already been consumed.
	 */
	public get slot(): Buffer {
		if (this.#consumed) throw new Error('StarTxGuard: slot has already been committed or rolled back.');
		return this.#slot;
	}

	/**
	 * Publishes `actualSize` bytes with `typeId` and flushes the spoke arena.
	 *
	 * @param actualSize Number of bytes written. Must not exceed the reservation.
	 * @param typeId     User-defined protocol identifier.
	 * @throws {Error} If the guard has already been consumed.
	 */
	public commit(actualSize: number, typeId: number): void {
		if (this.#consumed) throw new Error('StarTxGuard: slot has already been committed or rolled back.');
		this.#consumed = true;
		this.#handle.commitTx(this.#spokeIdx, actualSize, typeId);
	}

	/** Aborts the TX slot without publishing. No-op if already consumed. */
	public rollback(): void {
		if (this.#consumed) return;
		this.#consumed = true;
		this.#handle.rollbackTx(this.#spokeIdx);
	}

	/** Calls {@link rollback} if not yet consumed. */
	public [Symbol.dispose](): void {
		this.rollback();
	}
}

/**
 * Aggregates N independent SPSC arenas into a single round-robin polling loop
 * bounded by a TSC-calibrated time budget. One consumer per `StarBus`; one
 * producer per spoke.
 *
 * Each spoke is a {@link Bus} created by the producer (Listener) side. The star
 * holds Connector-side handles and ref-counts them internally; the caller may
 * close its own handles after {@link create} returns.
 *
 * All polling and TX operations must be driven from a single consumer thread.
 * {@link close} / `using` are safe from any thread.
 *
 * @example
 * ```ts
 * // Worker thread
 * using star = StarBus.create([c0, c1, c2]);
 * for (;;) {
 *   using guard = star.poll(64, 5_000n);
 *   if (!guard.isEmpty()) {
 *     for (const msg of guard.messages) process(msg);
 *   }
 * }
 * ```
 */
export class StarBus implements Disposable {
	readonly #handle: NativeStarBinding;
	#closed = false;

	private constructor(handle: NativeStarBinding) {
		this.#handle = handle;
	}

	/**
	 * Creates a {@link StarBus} from the given connector-side buses.
	 *
	 * Each bus is ref-counted internally; the caller may close its own handles
	 * after this returns.
	 *
	 * @param buses   Non-empty array of connector-side {@link Bus} instances.
	 * @param nodeIds Optional NUMA node IDs, same length as `buses`.
	 *                Negative values skip binding for that spoke.
	 *                `null` / omitted disables NUMA binding entirely.
	 * @throws {TypeError}  An element of `buses` is not a valid open bus handle.
	 * @throws {Error}      `buses` is empty or `nodeIds` length mismatches.
	 */
	public static create(buses: { _handle: object }[], nodeIds?: number[] | null): StarBus {
		if (buses.length === 0) {
			throw new Error('StarBus.create: buses must not be empty.');
		}

		if (nodeIds !== null && nodeIds !== undefined && nodeIds.length !== buses.length) {
			throw new Error('StarBus.create: nodeIds.length must equal buses.length.');
		}

		const rawHandles = buses.map((b, _) => {
			return b._handle;
		});

		const handle = native.TachyonStarBusNode.create(rawHandles, nodeIds ?? null);
		return new StarBus(handle);
	}

	/** Number of spokes. */
	public get nSpokes(): number {
		this.#assertOpen();
		return this.#handle.nSpokes();
	}

	/**
	 * Raw `tachyon_state_t` integer for `spokeIdx`.
	 * Returns `TACHYON_STATE_UNKNOWN` (5) if `spokeIdx` is out of range.
	 */
	public getState(spokeIdx: number): number {
		this.#assertOpen();
		return this.#handle.getState(spokeIdx);
	}

	/**
	 * Drains up to `maxTotal` messages across all spokes within `budgetUs`
	 * microseconds. Returns an empty guard if the budget expires before any
	 * message arrives.
	 *
	 * All {@link StarMsgView.data} buffers are zero-copy and SHM-backed.
	 *
	 * The returned guard must be committed or closed before the next `poll`.
	 *
	 * @param maxTotal  Upper bound on messages to drain. Must be > 0.
	 * @param budgetUs  TSC-bounded polling budget in microseconds.
	 */
	public poll(maxTotal: number, budgetUs: number): StarPollGuard {
		this.#assertOpen();
		warnMainThread('poll');
		const raw = this.#handle.poll(maxTotal, budgetUs);
		return new StarPollGuard(this.#handle, raw);
	}

	/**
	 * Acquires an exclusive zero-copy TX slot on `spokeIdx`.
	 * Returns `null` if the ring is full or `spokeIdx` is out of range.
	 *
	 * Prefer the guard pattern; call {@link StarTxGuard.commit} or let `using`
	 * roll back automatically.
	 *
	 * @param spokeIdx Zero-based spoke index.
	 * @param maxSize  Required contiguous byte capacity.
	 */
	public acquireTx(spokeIdx: number, maxSize: number): StarTxGuard | null {
		this.#assertOpen();
		const slot = this.#handle.acquireTx(spokeIdx, maxSize);
		if (slot === null) return null;
		return new StarTxGuard(this.#handle, spokeIdx, slot);
	}

	/**
	 * Notifies sleeping consumers on `spokeIdx` via a futex wake-up signal.
	 * Not needed after {@link StarTxGuard.commit}, which flushes internally.
	 */
	public flush(spokeIdx: number): void {
		this.#assertOpen();
		this.#handle.flush(spokeIdx);
	}

	/** Closes the bus and releases all internal references. Safe to call multiple times. */
	public close(): void {
		if (this.#closed) return;
		this.#closed = true;
		this.#handle.close();
	}

	/** Called automatically by the `using` keyword. */
	public [Symbol.dispose](): void {
		this.close();
	}

	#assertOpen(): void {
		if (this.#closed) throw new Error('StarBus: this bus has been closed.');
	}
}
