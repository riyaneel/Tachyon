import type { BatchController, RxMessage } from './batch.ts';
import { RxBatch } from './batch.ts';
import { PeerDeadError } from './error.ts';
import type { RxController, TxController } from './guards.ts';
import { RxGuard, TxGuard } from './guards.ts';

const TACHYON_STATE_FATAL_ERROR = 4;

export interface RawRx<T extends Uint8Array = Uint8Array> {
	readonly data: T;
	readonly typeId: number;
	readonly actualSize: number;
}

export interface RawBatchMessage<T extends Uint8Array = Uint8Array> {
	readonly data: T;
	readonly typeId: number;
	readonly size: number;
}

export interface BusHandle<T extends Uint8Array = Uint8Array> {
	close(): void;

	send(data: Uint8Array, typeId?: number): void;

	acquireTx(maxSize: number): T;

	commitTx(actualSize: number, typeId: number): void;

	commitTxUnflushed(actualSize: number, typeId: number): void;

	rollbackTx(): void;

	flush(): void;

	acquireRx(spinThreshold?: number): RawRx<T> | null;

	drainBatch?(maxMsgs: number, spinThreshold?: number): RawBatchMessage<T>[];

	commitRx(): void;

	commitBatch?(): void;

	setPollingMode(spinMode: number): void;

	setNumaNode(nodeId: number): void;

	getState(): number;
}

interface BusBaseOptions<T extends Uint8Array> {
	readonly defaultSpinThreshold: number;
	readonly retryNullRecv: boolean;
	readonly copyData: (data: T) => T;
}

/**
 * Shared JS surface for the native Node addon and the browser WASM transport.
 * Platform entrypoints only adapt their handle shape; guard lifecycle, recv
 * copying, batching, close semantics, and API compatibility stay in one place.
 *
 * `T` is the platform byte-buffer type: `Buffer` for native Node, `Uint8Array`
 * for browser WASM. It is threaded through the guards so a browser slot is never
 * surfaced as a `Buffer`.
 */
export abstract class BusBase<T extends Uint8Array> implements Disposable {
	#handle: BusHandle<T>;
	#closed = false;
	#options: BusBaseOptions<T>;

	protected constructor(handle: BusHandle<T>, options: BusBaseOptions<T>) {
		this.#handle = handle;
		this.#options = options;
	}

	/**
	 * Signals that the consumer will never sleep. Native Node can use this to
	 * skip the seq_cst fence and consumer_sleeping check on flush; browser WASM
	 * has no futex sleep path, so the browser handle accepts this as a no-op.
	 */
	public setPollingMode(spinMode: 0 | 1): void {
		this.#assertOpen();
		this.#handle.setPollingMode(spinMode);
	}

	/**
	 * Binds native SHM pages to a specific NUMA node where supported. Browser
	 * WASM memory is page-local and cannot be NUMA-bound, so it is a no-op there.
	 */
	public setNumaNode(nodeId: number): void {
		this.#assertOpen();
		this.#handle.setNumaNode(nodeId);
	}

	/** Publishes all pending unflushed TX messages to the consumer. */
	public flush(): void {
		this.#assertOpen();
		this.#handle.flush();
	}

	/** Copies `data` into the ring buffer, commits, and flushes. */
	public send(data: Uint8Array, typeId = 0): void {
		this.#assertOpen();
		this.#handle.send(data, typeId);
	}

	/**
	 * Copies the next payload and returns it with its type discriminator. Native
	 * Node blocks and retries EINTR through the native handle; browser WASM is
	 * non-blocking and returns `null` when the ring is empty (an empty ring is a
	 * normal poll outcome, not an error).
	 *
	 * @returns The next message, or `null` when no message is available (browser only).
	 * @throws {PeerDeadError} If the bus has transitioned to fatal error state.
	 * @remarks The slot is released even if the copy throws.
	 */
	public recv(spinThreshold = this.#options.defaultSpinThreshold): { data: T; typeId: number } | null {
		this.#assertOpen();
		for (;;) {
			if (this.#isFatal()) throw new PeerDeadError();
			const result = this.#handle.acquireRx(spinThreshold);
			if (result === null) {
				if (this.#options.retryNullRecv) continue;
				return null;
			}

			try {
				const copy = this.#options.copyData(result.data);
				return { data: copy, typeId: result.typeId };
			} finally {
				this.#handle.commitRx();
			}
		}
	}

	/**
	 * Acquires an exclusive TX slot of `maxSize` bytes.
	 * Write into the slot via {@link TxGuard.bytes}, then commit or rollback.
	 */
	public acquireTx(maxSize: number): TxGuard<T> {
		this.#assertOpen();
		const buf = this.#handle.acquireTx(maxSize);
		const ctrl: TxController = {
			assertOpen: () => {
				this.#assertOpen();
			},
			commitTx: (s, t) => {
				this.#handle.commitTx(s, t);
			},
			commitTxUnflushed: (s, t) => {
				this.#handle.commitTxUnflushed(s, t);
			},
			rollbackTx: () => {
				this.#handle.rollbackTx();
			},
		};
		return new TxGuard<T>(ctrl, buf);
	}

	/**
	 * Acquires a zero-copy read lease. Node may block according to
	 * `spinThreshold`; browser WASM checks once and returns `null` when empty.
	 *
	 * @throws {PeerDeadError} If the bus has transitioned to fatal error state.
	 */
	public acquireRx(spinThreshold = this.#options.defaultSpinThreshold): RxGuard<T> | null {
		this.#assertOpen();
		if (this.#isFatal()) throw new PeerDeadError();
		const result = this.#handle.acquireRx(spinThreshold);
		if (result === null) return null;
		const ctrl: RxController = {
			commitRx: () => {
				this.#handle.commitRx();
			},
			getState: () => this.#handle.getState(),
		};
		return new RxGuard<T>(ctrl, result.data, result.typeId, result.actualSize);
	}

	/**
	 * Drains up to `maxMsgs` messages in one call across the boundary. Both
	 * transports are zero-copy; the slots are released on commit.
	 */
	public drainBatch(maxMsgs: number, spinThreshold = this.#options.defaultSpinThreshold): RxBatch<T> {
		this.#assertOpen();
		if (this.#isFatal()) throw new PeerDeadError();

		const raw =
			this.#handle.drainBatch?.(maxMsgs, spinThreshold) ?? this.#drainBatchByAcquireRx(maxMsgs, spinThreshold);
		const messages: RxMessage<T>[] = raw.map((m) => ({
			data: m.data as RxMessage<T>['data'],
			typeId: m.typeId,
			size: m.size,
		}));
		const ctrl: BatchController = {
			commitBatch: () => {
				this.#handle.commitBatch?.();
			},
			getState: () => this.#handle.getState(),
		};
		return new RxBatch<T>(ctrl, messages);
	}

	/** Closes the bus and releases the underlying platform handle. Safe to call multiple times. */
	public close(): void {
		if (this.#closed) return;
		this.#closed = true;
		this.#handle.close();
	}

	/** Called automatically by the `using` keyword. */
	public [Symbol.dispose](): void {
		this.close();
	}

	#drainBatchByAcquireRx(maxMsgs: number, spinThreshold: number): RawBatchMessage<T>[] {
		const messages: RawBatchMessage<T>[] = [];
		for (let i = 0; i < maxMsgs; i += 1) {
			if (this.#isFatal()) throw new PeerDeadError();
			const result = this.#handle.acquireRx(spinThreshold);
			if (result === null) break;
			try {
				messages.push({
					data: this.#options.copyData(result.data),
					typeId: result.typeId,
					size: result.actualSize,
				});
			} finally {
				this.#handle.commitRx();
			}
		}

		return messages;
	}

	#assertOpen(): void {
		if (this.#closed) throw new Error('Bus: this bus has been closed.');
	}

	#isFatal(): boolean {
		return this.#handle.getState() === TACHYON_STATE_FATAL_ERROR;
	}
}
