import type { BatchController, RxMessage } from './batch.ts';
import { RxBatch } from './batch.ts';
import { PeerDeadError } from './error.ts';
import type { RxController, RxSlot, TxController } from './guards.ts';
import { RxGuard, TxGuard } from './guards.ts';

const TACHYON_STATE_FATAL_ERROR = 4;

export interface RawRx {
	readonly data: Buffer | Uint8Array;
	readonly typeId: number;
	readonly actualSize: number;
}

export interface RawBatchMessage {
	readonly data: Buffer | Uint8Array;
	readonly typeId: number;
	readonly size: number;
}

export interface BusHandle {
	close(): void;

	send(data: Buffer | Uint8Array, typeId?: number): void;

	acquireTx(maxSize: number): Buffer | Uint8Array;

	commitTx(actualSize: number, typeId: number): void;

	commitTxUnflushed(actualSize: number, typeId: number): void;

	rollbackTx(): void;

	flush(): void;

	acquireRx(spinThreshold?: number): RawRx | null;

	drainBatch?(maxMsgs: number, spinThreshold?: number): RawBatchMessage[];

	commitRx(): void;

	commitBatch?(): void;

	setPollingMode(spinMode: number): void;

	setNumaNode(nodeId: number): void;

	getState(): number;
}

interface BusBaseOptions<TRecv extends Buffer | Uint8Array> {
	readonly defaultSpinThreshold: number;
	readonly retryNullRecv: boolean;
	readonly nullRecvMessage: string;
	readonly copyData: (data: Buffer | Uint8Array) => TRecv;
}

/**
 * Shared JS surface for the native Node addon and the browser WASM transport.
 * Platform entrypoints only adapt their handle shape; guard lifecycle, recv
 * copying, batching, close semantics, and API compatibility stay in one place.
 */
export abstract class BusBase<TRecv extends Buffer | Uint8Array> implements Disposable {
	#handle: BusHandle;
	#closed = false;
	#options: BusBaseOptions<TRecv>;

	protected constructor(handle: BusHandle, options: BusBaseOptions<TRecv>) {
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
	public send(data: Buffer | Uint8Array, typeId = 0): void {
		this.#assertOpen();
		this.#handle.send(data, typeId);
	}

	/**
	 * Copies the next payload and returns it with its type discriminator. Node
	 * blocks and retries EINTR through the native handle; browser WASM is
	 * non-blocking and throws if no message is available.
	 *
	 * @throws {PeerDeadError} If the bus has transitioned to fatal error state.
	 */
	public recv(spinThreshold = this.#options.defaultSpinThreshold): { data: TRecv; typeId: number } {
		this.#assertOpen();
		for (;;) {
			if (this.#isFatal()) throw new PeerDeadError();
			const result = this.#handle.acquireRx(spinThreshold);
			if (result === null) {
				if (this.#options.retryNullRecv) continue;
				throw new Error(this.#options.nullRecvMessage);
			}

			const copy = this.#options.copyData(result.data);
			this.#handle.commitRx();
			return { data: copy, typeId: result.typeId };
		}
	}

	/**
	 * Acquires an exclusive TX slot of `maxSize` bytes.
	 * Write into the slot via {@link TxGuard.bytes}, then commit or rollback.
	 */
	public acquireTx(maxSize: number): TxGuard {
		this.#assertOpen();
		const buf = this.#handle.acquireTx(maxSize);
		const ctrl: TxController = {
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
		return new TxGuard(ctrl, buf as unknown as Buffer);
	}

	/**
	 * Acquires a zero-copy read lease. Node may block according to
	 * `spinThreshold`; browser WASM checks once and returns `null` when empty.
	 *
	 * @throws {PeerDeadError} If the bus has transitioned to fatal error state.
	 */
	public acquireRx(spinThreshold = this.#options.defaultSpinThreshold): RxGuard | null {
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
		return new RxGuard(ctrl, result.data as unknown as Buffer, result.typeId, result.actualSize);
	}

	/**
	 * Drains up to `maxMsgs` messages. Native Node uses one addon call to
	 * amortize FFI cost; browser WASM falls back to the same guard lifecycle
	 * with copied batch entries so all slots are released before returning.
	 */
	public drainBatch(maxMsgs: number, spinThreshold = this.#options.defaultSpinThreshold): RxBatch {
		this.#assertOpen();
		if (this.#isFatal()) throw new PeerDeadError();

		const raw =
			this.#handle.drainBatch?.(maxMsgs, spinThreshold) ?? this.#drainBatchByAcquireRx(maxMsgs, spinThreshold);
		const messages: RxMessage[] = raw.map((m) => ({
			data: m.data as unknown as RxSlot,
			typeId: m.typeId,
			size: m.size,
		}));
		const ctrl: BatchController = {
			commitBatch: () => {
				this.#handle.commitBatch?.();
			},
			getState: () => this.#handle.getState(),
		};
		return new RxBatch(ctrl, messages);
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

	#drainBatchByAcquireRx(maxMsgs: number, spinThreshold: number): RawBatchMessage[] {
		const messages: RawBatchMessage[] = [];
		for (let i = 0; i < maxMsgs; i += 1) {
			if (this.#isFatal()) throw new PeerDeadError();
			const result = this.#handle.acquireRx(spinThreshold);
			if (result === null) break;
			messages.push({
				data: this.#options.copyData(result.data),
				typeId: result.typeId,
				size: result.actualSize,
			});
			this.#handle.commitRx();
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
