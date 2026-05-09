import { createRequire } from 'node:module';
import { isMainThread } from 'node:worker_threads';

import { AbiMismatchError, ErrorCode, isTachyonError } from './error.ts';
import type { BusHandle, RawRx } from './bus_core.ts';
import { BusBase } from './bus_core.ts';

const _require = createRequire(import.meta.url);

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

	drainBatch(maxMsgs: number, spinThreshold?: number): { data: Buffer; typeId: number; size: number }[];

	commitBatch(): void;

	setPollingMode(spinMode: number): void;

	setNumaNode(nodeId: number): void;

	getState(): number;
}

interface NativeModule {
	TachyonBusNode: {
		listen(socketPath: string, capacity: number): NativeBinding;
		connect(socketPath: string): NativeBinding;
	};
}

function loadNative(): NativeModule {
	const candidates = [
		new URL('../../build/Release/tachyon_node.node', import.meta.url).pathname,
		new URL('../../build/Debug/tachyon_node.node', import.meta.url).pathname,
	];

	for (const p of candidates) {
		try {
			return _require(p) as NativeModule;
		} catch {
			// try next candidate
		}
	}

	throw new Error(
		'tachyon_node.node not found. Run `npm run build:native` first.\n' + `Searched: ${candidates.join(', ')}`,
	);
}

const native = loadNative();

class NativeBusHandle implements BusHandle {
	#handle: NativeBinding;

	public constructor(handle: NativeBinding) {
		this.#handle = handle;
	}

	public close(): void {
		this.#handle.close();
	}

	public send(data: Buffer | Uint8Array, typeId?: number): void {
		this.#handle.send(data, typeId);
	}

	public acquireTx(maxSize: number): Buffer {
		return this.#handle.acquireTx(maxSize);
	}

	public commitTx(actualSize: number, typeId: number): void {
		this.#handle.commitTx(actualSize, typeId);
	}

	public commitTxUnflushed(actualSize: number, typeId: number): void {
		this.#handle.commitTxUnflushed(actualSize, typeId);
	}

	public rollbackTx(): void {
		this.#handle.rollbackTx();
	}

	public flush(): void {
		this.#handle.flush();
	}

	public acquireRx(spinThreshold?: number): RawRx | null {
		return this.#handle.acquireRxBlocking(spinThreshold);
	}

	public drainBatch(maxMsgs: number, spinThreshold?: number): { data: Buffer; typeId: number; size: number }[] {
		return this.#handle.drainBatch(maxMsgs, spinThreshold);
	}

	public commitRx(): void {
		this.#handle.commitRx();
	}

	public commitBatch(): void {
		this.#handle.commitBatch();
	}

	public setPollingMode(spinMode: number): void {
		this.#handle.setPollingMode(spinMode);
	}

	public setNumaNode(nodeId: number): void {
		this.#handle.setNumaNode(nodeId);
	}

	public getState(): number {
		return this.#handle.getState();
	}
}

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
 * Start the consumer first, it owns the UNIX socket and the SHM arena.
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
export class Bus extends BusBase<Buffer> {
	private constructor(handle: NativeBinding) {
		super(new NativeBusHandle(handle), {
			defaultSpinThreshold: 10_000,
			retryNullRecv: true,
			nullRecvMessage: 'Bus.recv: interrupted while waiting for a message.',
			copyData: (data) => Buffer.from(data),
		});
	}

	/**
	 * Creates the SHM arena and waits for one producer to connect.
	 * Blocks until the producer calls {@link connect} on the same path.
	 *
	 * @param socketPath Socket path
	 * @param capacity Must be a strictly positive power of two.
	 */
	public static listen(socketPath: string, capacity: number): Bus {
		warnMainThread('listen');
		return new Bus(native.TachyonBusNode.listen(socketPath, capacity));
	}

	/**
	 * Connects to an existing SHM arena via the UNIX socket at `socketPath`.
	 *
	 * @throws {AbiMismatchError} If producer and consumer were compiled with incompatible versions.
	 */
	public static connect(socketPath: string): Bus {
		warnMainThread('connect');
		try {
			return new Bus(native.TachyonBusNode.connect(socketPath));
		} catch (err) {
			if (isTachyonError(err) && err.code === ErrorCode.AbiMismatch) throw new AbiMismatchError();
			throw err;
		}
	}
}
