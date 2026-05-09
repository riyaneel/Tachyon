import { RxBatch, type RxMessage } from './batch.ts';
import { PeerDeadError } from './error.ts';
import type { BatchController } from './batch.ts';
import type { RxController, TxController } from './guards.ts';
import { RxGuard, TxGuard } from './guards.ts';
import { makeTypeId, msgType, routeId, tachyon_browser_echo_once, WasmBus } from './wasm/tachyon_ipc.js';
// @ts-expect-error Bundlers load the generated wasm module through this import.
import * as wasmRaw from './wasm/tachyon_ipc_bg.wasm';

const wasmMemory = (wasmRaw as unknown as { readonly memory: { readonly buffer: ArrayBuffer } }).memory;

interface BrowserEndpoint {
	handle: WasmBus;
	refs: number;
}

const endpoints = new Map<string, BrowserEndpoint>();

function slot(ptr: number, len: number): Uint8Array {
	return new Uint8Array(wasmMemory.buffer, ptr, len);
}

/**
 * Browser implementation of the Tachyon SPSC bus.
 *
 * Bundlers resolve `@tachyon-ipc/core` to this entry through the package
 * `browser` export condition. The constructor shape matches Node:
 * `Bus.listen(path, capacity)` creates a page-local ring and
 * `Bus.connect(path)` attaches to it.
 */
export class Bus implements Disposable {
	#endpoint: BrowserEndpoint;
	#path: string;
	#closed = false;

	private constructor(path: string, endpoint: BrowserEndpoint) {
		this.#path = path;
		this.#endpoint = endpoint;
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

	public setPollingMode(_spinMode: 0 | 1): void {
		this.#assertOpen();
	}

	public setNumaNode(_nodeId: number): void {
		this.#assertOpen();
	}

	public flush(): void {
		this.#assertOpen();
		this.#endpoint.handle.flush();
	}

	public send(data: Uint8Array, typeId = 0): void {
		this.#assertOpen();
		this.#endpoint.handle.send(data, typeId);
	}

	public sendU32(value: number, typeId = 0): void {
		this.#assertOpen();
		this.#endpoint.handle.sendU32(value, typeId);
	}

	public recv(): { data: Uint8Array; typeId: number } {
		this.#assertOpen();
		const guard = this.acquireRx();
		if (guard === null) {
			throw new Error('Bus.recv: no browser message is available. Use a direct doorbell after send().');
		}

		const data = new Uint8Array(guard.data());
		const typeId = guard.typeId;
		guard.commit();
		return { data, typeId };
	}

	public recvU32(): number {
		this.#assertOpen();
		return this.#endpoint.handle.recvU32();
	}

	public acquireTx(maxSize: number): TxGuard {
		this.#assertOpen();
		const ptr = this.#endpoint.handle.acquireTx(maxSize);
		const ctrl: TxController = {
			commitTx: (s, t) => {
				this.#endpoint.handle.commitTx(s, t);
			},
			commitTxUnflushed: (s, t) => {
				this.#endpoint.handle.commitTxUnflushed(s, t);
			},
			rollbackTx: () => {
				this.#endpoint.handle.rollbackTx();
			},
		};
		return new TxGuard(ctrl, slot(ptr, maxSize) as unknown as Buffer);
	}

	public acquireRx(_spinThreshold = 0): RxGuard | null {
		this.#assertOpen();
		if (this.#endpoint.handle.isFatal()) throw new PeerDeadError();
		if (!this.#endpoint.handle.acquireRx()) return null;

		const ptr = this.#endpoint.handle.rxPtr();
		const actualSize = this.#endpoint.handle.rxSize();
		const typeId = this.#endpoint.handle.rxTypeId();
		const ctrl: RxController = {
			commitRx: () => {
				this.#endpoint.handle.commitRx();
			},
			getState: () => (this.#endpoint.handle.isFatal() ? 4 : 2),
		};
		return new RxGuard(ctrl, slot(ptr, actualSize) as unknown as Buffer, typeId, actualSize);
	}

	public drainBatch(maxMsgs: number, _spinThreshold = 0): RxBatch {
		this.#assertOpen();
		if (this.#endpoint.handle.isFatal()) throw new PeerDeadError();

		const messages: RxMessage[] = [];
		for (let i = 0; i < maxMsgs; i += 1) {
			const guard = this.acquireRx();
			if (guard === null) break;
			messages.push({
				data: new Uint8Array(guard.data()) as unknown as RxMessage['data'],
				typeId: guard.typeId,
				size: guard.actualSize,
			});
			guard.commit();
		}

		const ctrl: BatchController = {
			commitBatch: () => {
				// Browser batches are copied and committed while draining.
			},
			getState: () => (this.#endpoint.handle.isFatal() ? 4 : 2),
		};
		return new RxBatch(ctrl, messages);
	}

	public close(): void {
		if (this.#closed) return;
		this.#closed = true;
		this.#endpoint.refs -= 1;
		if (this.#endpoint.refs <= 0) {
			endpoints.delete(this.#path);
			this.#endpoint.handle.free();
		}
	}

	public [Symbol.dispose](): void {
		this.close();
	}

	#assertOpen(): void {
		if (this.#closed) throw new Error('Bus: this bus has been closed.');
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
export { makeTypeId, msgType, routeId, tachyon_browser_echo_once, WasmBus };
