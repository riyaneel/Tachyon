import type { BusHandle, RawRx } from './bus_core.ts';
import { BusBase } from './bus_core.ts';
import initWasm, { makeTypeId, msgType, routeId, WasmBus } from './wasm/tachyon_ipc.ts';

interface WasmRuntime {
	readonly memory: {
		readonly buffer: ArrayBuffer;
	};
}

const wasm = (await initWasm()) as unknown as WasmRuntime;

interface BrowserEndpoint {
	handle: WasmBus;
	refs: number;
}

const endpoints = new Map<string, BrowserEndpoint>();

function slot(ptr: number, len: number): Uint8Array {
	return new Uint8Array(wasm.memory.buffer, ptr, len);
}

class BrowserBusHandle implements BusHandle {
	#endpoint: BrowserEndpoint;
	#path: string;

	public constructor(path: string, endpoint: BrowserEndpoint) {
		this.#path = path;
		this.#endpoint = endpoint;
	}

	public close(): void {
		this.#endpoint.refs -= 1;
		if (this.#endpoint.refs <= 0) {
			endpoints.delete(this.#path);
			this.#endpoint.handle.free();
		}
	}

	public send(data: Buffer | Uint8Array, typeId?: number): void {
		this.#endpoint.handle.send(data, typeId ?? 0);
	}

	public acquireTx(maxSize: number): Uint8Array {
		const ptr = this.#endpoint.handle.acquireTx(maxSize);
		return slot(ptr, maxSize);
	}

	public commitTx(actualSize: number, typeId: number): void {
		this.#endpoint.handle.commitTx(actualSize, typeId);
	}

	public commitTxUnflushed(actualSize: number, typeId: number): void {
		this.#endpoint.handle.commitTxUnflushed(actualSize, typeId);
	}

	public rollbackTx(): void {
		this.#endpoint.handle.rollbackTx();
	}

	public flush(): void {
		this.#endpoint.handle.flush();
	}

	public acquireRx(): RawRx | null {
		if (!this.#endpoint.handle.acquireRx()) return null;
		const ptr = this.#endpoint.handle.rxPtr();
		const actualSize = this.#endpoint.handle.rxSize();
		return {
			data: slot(ptr, actualSize),
			typeId: this.#endpoint.handle.rxTypeId(),
			actualSize,
		};
	}

	public commitRx(): void {
		this.#endpoint.handle.commitRx();
	}

	public setPollingMode(_spinMode: number): void {
		// Browser delivery is direct and non-blocking; there is no futex polling mode.
	}

	public setNumaNode(_nodeId: number): void {
		// WASM memory is page-local and cannot be NUMA-bound from browser JS.
	}

	public getState(): number {
		return this.#endpoint.handle.isFatal() ? 4 : 2;
	}
}

/**
 * Browser implementation of the Tachyon SPSC bus.
 *
 * Bundlers resolve `@tachyon-ipc/core` to this entry through the package
 * `browser` export condition. The constructor shape matches Node:
 * `Bus.listen(path, capacity)` creates a page-local ring and
 * `Bus.connect(path)` attaches to it.
 */
export class Bus extends BusBase<Uint8Array> {
	private constructor(path: string, endpoint: BrowserEndpoint) {
		super(new BrowserBusHandle(path, endpoint), {
			defaultSpinThreshold: 0,
			retryNullRecv: false,
			nullRecvMessage: 'Bus.recv: no browser message is available. Use a direct doorbell after send().',
			copyData: (data) => new Uint8Array(data),
		});
	}

	public static listen(socketPath: string, capacity: number): Bus {
		if (endpoints.has(socketPath)) {
			throw new Error(`Bus.listen: browser endpoint already exists for ${socketPath}`);
		}

		const endpoint = { handle: new WasmBus(capacity), refs: 1 };
		endpoints.set(socketPath, endpoint);
		return new Bus(socketPath, endpoint);
	}

	public static connect(socketPath: string): Bus {
		const endpoint = endpoints.get(socketPath);
		if (endpoint === undefined) {
			throw new Error(`Bus.connect: no browser endpoint is listening at ${socketPath}`);
		}

		endpoint.refs += 1;
		return new Bus(socketPath, endpoint);
	}
}

export {
	TachyonError,
	AbiMismatchError,
	PeerDeadError,
	ErrorCode,
	isAbiMismatch,
	isFull,
	isTachyonError,
	isPeerDead,
} from './error.ts';
export type { ErrorCode as ErrorCodeType } from './error.ts';
export { RxBatch } from './batch.ts';
export type { RxMessage } from './batch.ts';
export { TxGuard, RxGuard } from './guards.ts';
export type { TxSlot, RxSlot } from './guards.ts';
export { makeTypeId, msgType, routeId };
