import path from 'node:path';
import {isMainThread} from 'node:worker_threads';

import {RxBatch, type BatchController, type RxMessage} from './batch';
import {AbiMismatchError, ErrorCode, PeerDeadError, isTachyonError} from './error';
import {TxGuard, RxGuard, type TxController, type RxController} from './guards';
import type {RxSlot} from './guards';

// Raw shape of a native instance returned by TachyonBusNode.listen / .connect.
interface NativeBinding {
    close(): void;

    send(data: Buffer | Uint8Array, typeId?: number): void;

    acquireTx(maxSize: number): Buffer;

    commitTx(actualSize: number, typeId: number): void;

    commitTxUnflushed(actualSize: number, typeId: number): void;

    rollbackTx(): void;

    flush(): void;

    acquireRxBlocking(spinThreshold?: number): { data: Buffer; typeId: number; actualSize: number } | null;

    commitRx(): void;

    drainBatch(maxMsgs: number, spinThreshold?: number): Array<{ data: Buffer; typeId: number; size: number }>;

    commitBatch(): void;

    setPollingMode(spinMode: number): void;

    setNumaNode(nodeId: number): void;

    getState(): number;
}

interface NativeModule {
    TachyonBusNode: {
        listen(path: string, capacity: number): NativeBinding;
        connect(path: string): NativeBinding;
    };
}

function loadNative(): NativeModule {
    const candidates = [
        path.join(__dirname, '../build/Release/tachyon_node.node'),
        path.join(__dirname, '../build/Debug/tachyon_node.node'),
    ];
    for (const p of candidates) {
        try {
            // eslint-disable-next-line @typescript-eslint/no-require-imports
            return require(p) as NativeModule;
        } catch {
            // try next candidate
        }
    }

    throw new Error(
        'tachyon_node.node not found. Run `npm run build:native` first.\n' +
        `Searched: ${candidates.join(', ')}`,
    );
}

const native = loadNative();

function warnMainThread(method: string): void {
    if (isMainThread) {
        console.warn(
            `[tachyon] Bus.${method}() called on the main thread. ` +
            'Blocking futex calls will saturate the event loop. ' +
            'Consider moving IPC to a Worker.',
        );
    }
}

/**
 * Tachyon SPSC IPC bus.
 *
 * Start the consumer first — it owns the UNIX socket and the SHM arena.
 * All blocking operations (listen, acquireRx, drainBatch) spin then park on a futex;
 * call them from a Worker thread to avoid saturating the main event loop.
 *
 * `using` closes the bus automatically.
 *
 * @example
 * ```ts
 * // consumer (start first)
 * using bus = Bus.listen('/tmp/demo.sock', 1 << 16);
 * const { data, typeId } = bus.recv();
 *
 * // producer
 * using bus = Bus.connect('/tmp/demo.sock');
 * bus.send(Buffer.from('hello'), 1);
 * ```
 */
export class Bus implements Disposable {
    #handle: NativeBinding;
    #closed: boolean = false;

    private constructor(handle: NativeBinding) {
        this.#handle = handle;
    }

    /**
     * Creates the SHM arena and waits for one producer to connect.
     * Blocks until the producer calls {@link connect} on the same path.
     *
     * @param socketPath Socket path
     * @param capacity Must be a strictly positive power of two.
     */
    static listen(socketPath: string, capacity: number): Bus {
        warnMainThread('listen');
        return new Bus(native.TachyonBusNode.listen(socketPath, capacity));
    }

    /**
     * Connects to an existing SHM arena via the UNIX socket at `socketPath`.
     *
     * @throws {AbiMismatchError} If producer and consumer were compiled with incompatible versions.
     */
    static connect(socketPath: string): Bus {
        warnMainThread('connect');
        try {
            return new Bus(native.TachyonBusNode.connect(socketPath));
        } catch (err) {
            if (isTachyonError(err) && err.code === ErrorCode.AbiMismatch) throw new AbiMismatchError();
            throw err;
        }
    }

    /**
     * Signals that the consumer will never sleep. The producer skips the seq_cst fence
     * and consumer_sleeping check on every flush. Use only on a dedicated SCHED_FIFO thread.
     * Call immediately after listen/connect, before the first message.
     */
    setPollingMode(spinMode: 0 | 1): void {
        this.#assertOpen();
        this.#handle.setPollingMode(spinMode);
    }

    /**
     * Binds the SHM pages to a specific NUMA node (MPOL_PREFERRED + MPOL_MF_MOVE).
     * No-op on non-Linux platforms. Call immediately after listen/connect.
     */
    setNumaNode(nodeId: number): void {
        this.#assertOpen();
        this.#handle.setNumaNode(nodeId);
    }

    /** Publishes all pending unflushed TX messages to the consumer. */
    flush(): void {
        this.#assertOpen();
        this.#handle.flush();
    }

    /** Copies `data` into the ring buffer, commits, and flushes. */
    send(data: Buffer | Uint8Array, typeId = 0): void {
        this.#assertOpen();
        this.#handle.send(data, typeId);
    }

    /**
     * Blocks until a message is available, copies the payload, and returns it.
     * Retries transparently on EINTR.
     *
     * @throws {PeerDeadError} If the bus has transitioned to TACHYON_STATE_FATAL_ERROR.
     */
    recv(spinThreshold = 10_000): { data: Buffer; typeId: number } {
        this.#assertOpen();
        for (; ;) {
            if (this.#handle.getState() === 4) throw new PeerDeadError();
            const result = this.#handle.acquireRxBlocking(spinThreshold);
            if (result === null) continue; // EINTR — retry
            const copy = Buffer.from(result.data);
            this.#handle.commitRx();
            return {data: copy, typeId: result.typeId};
        }
    }

    /**
     * Acquires an exclusive TX slot of `maxSize` bytes.
     * Write into the slot via {@link TxGuard.bytes}, then commit or rollback.
     *
     * @throws {TachyonError} code `ERR_TACHYON_FULL` if the ring buffer is full.
     */
    acquireTx(maxSize: number): TxGuard {
        this.#assertOpen();
        const buf = this.#handle.acquireTx(maxSize);
        const ctrl: TxController = {
            commitTx: (s, t) => this.#handle.commitTx(s, t),
            commitTxUnflushed: (s, t) => this.#handle.commitTxUnflushed(s, t),
            rollbackTx: () => this.#handle.rollbackTx(),
        };
        return new TxGuard(ctrl, buf);
    }

    /**
     * Blocks until a message is available and returns a zero-copy read lease.
     * Retries transparently on EINTR. Returns `null` only on EINTR when the
     * spin threshold is exhausted and no futex wake arrives.
     *
     * @throws {PeerDeadError} If the bus has transitioned to TACHYON_STATE_FATAL_ERROR.
     */
    acquireRx(spinThreshold = 10_000): RxGuard | null {
        this.#assertOpen();
        if (this.#handle.getState() === 4) throw new PeerDeadError();
        const result = this.#handle.acquireRxBlocking(spinThreshold);
        if (result === null) return null;
        const ctrl: RxController = {
            commitRx: () => this.#handle.commitRx(),
            getState: () => this.#handle.getState(),
        };
        return new RxGuard(ctrl, result.data, result.typeId, result.actualSize);
    }

    /**
     * Blocks until at least one message is available, then drains up to `maxMsgs`
     * in a single native call. One native crossing amortizes per-message FFI cost.
     *
     * @throws {PeerDeadError} If the bus has transitioned to TACHYON_STATE_FATAL_ERROR.
     */
    drainBatch(maxMsgs: number, spinThreshold = 10_000): RxBatch {
        this.#assertOpen();
        if (this.#handle.getState() === 4) throw new PeerDeadError();
        const raw = this.#handle.drainBatch(maxMsgs, spinThreshold);
        const messages: RxMessage[] = raw.map(m => ({
            data: m.data as unknown as RxSlot,
            typeId: m.typeId,
            size: m.size,
        }));
        const ctrl: BatchController = {
            commitBatch: () => this.#handle.commitBatch(),
            getState: () => this.#handle.getState(),
        };
        return new RxBatch(ctrl, messages);
    }

    /** Closes the bus and unmaps shared memory. Safe to call multiple times. */
    close(): void {
        if (this.#closed) return;
        this.#closed = true;
        this.#handle.close();
    }

    /** Called automatically by the `using` keyword. */
    [Symbol.dispose](): void {
        this.close();
    }

    #assertOpen(): void {
        if (this.#closed) throw new Error('Bus: this bus has been closed.');
    }
}
