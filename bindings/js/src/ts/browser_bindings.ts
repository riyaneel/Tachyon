import type { BusHandle, RawBatchMessage, RawRx } from './bus_core.ts';
import { BusBase } from './bus_core.ts';
import { ErrorCode, TachyonError } from './error.ts';
import type { CwrapFn, TachyonCoreModule } from './wasm/tachyon.js';

const TACHYON_SUCCESS = 0;

/**
 * `size_t` is 32-bit on wasm32, so the core rejects any capacity above `INT32_MAX`. Combined with the power-of-two
 * rule, the largest usable ring is 2^30 (1 GiB). Rejected here too so an oversized request fails as a validation
 * error rather than reaching the core.
 */
const MAX_CAPACITY = 0x7fff_ffff; // INT32_MAX
const MAX_BATCH_MSGS = 65_536;

const MSG_VIEW_SIZE = 32;
const VIEW_PTR = 0;
const VIEW_SIZE = 4;
const VIEW_TYPE_ID = 12;

function validateUint(value: number, max: number, name: string): void {
	if (!Number.isInteger(value) || value < 0 || value > max) {
		throw new TachyonError(`Bus: ${name} must be an integer in [0, ${max}].`, ErrorCode.InvalidSz);
	}
}

/** Maps a `tachyon_error_t` code to the JS error-code surface. */
function mapError(code: number): ErrorCode {
	switch (code) {
		case 1:
			return ErrorCode.NullPtr;
		case 2:
			return ErrorCode.Mem;
		case 3:
			return ErrorCode.Open;
		case 4:
			return ErrorCode.Truncate;
		case 5:
			return ErrorCode.Chmod;
		case 6:
			return ErrorCode.Seal;
		case 7:
			return ErrorCode.Map;
		case 8:
			return ErrorCode.InvalidSz;
		case 9:
			return ErrorCode.Full;
		case 10:
			return ErrorCode.Empty;
		case 11:
			return ErrorCode.Network;
		case 13:
			return ErrorCode.Interrupted;
		case 14:
			return ErrorCode.AbiMismatch;
		default:
			return ErrorCode.System;
	}
}

export interface BrowserBus extends BusBase<Uint8Array> {
	/** Borrowed C ABI pointer, valid until close(), for exports in this WASM module only. */
	readonly wasmPointer: number;
}

export interface BrowserBindings {
	Bus: {
		listen(socketPath: string, capacity: number): BrowserBus;
		connect(socketPath: string): BrowserBus;
	};
}

const bindingsByModule = new WeakMap<TachyonCoreModule, BrowserBindings>();

/** Bind the JS API to an application's Emscripten module linked with the Tachyon core. */
export function createBrowserBindings(core: TachyonCoreModule): BrowserBindings {
	const existing = bindingsByModule.get(core);
	if (existing) return existing;
	const abi: {
		busListen: CwrapFn;
		busDestroy: CwrapFn;
		getShmPtr: CwrapFn;
		acquireTx: CwrapFn;
		commitTx: CwrapFn;
		acquireRxBatch: CwrapFn;
		commitRxBatch: CwrapFn;
		rollbackTx: CwrapFn;
		acquireRx: CwrapFn;
		commitRx: CwrapFn;
		flush: CwrapFn;
		getState: CwrapFn;
		setPollingMode: CwrapFn;
	} = {
		busListen: core.cwrap('tachyon_bus_listen', 'number', ['string', 'number', 'number']),
		busDestroy: core.cwrap('tachyon_bus_destroy', null, ['number']),
		getShmPtr: core.cwrap('tachyon_bus_get_shm_ptr', 'number', ['number']),
		acquireTx: core.cwrap('tachyon_acquire_tx', 'number', ['number', 'number']),
		commitTx: core.cwrap('tachyon_commit_tx', 'number', ['number', 'number', 'number']),
		rollbackTx: core.cwrap('tachyon_rollback_tx', 'number', ['number']),
		acquireRx: core.cwrap('tachyon_acquire_rx', 'number', ['number', 'number', 'number']),
		commitRx: core.cwrap('tachyon_commit_rx', 'number', ['number']),
		acquireRxBatch: core.cwrap('tachyon_acquire_rx_batch', 'number', ['number', 'number', 'number']),
		commitRxBatch: core.cwrap('tachyon_commit_rx_batch', 'number', ['number', 'number', 'number']),
		flush: core.cwrap('tachyon_flush', null, ['number']),
		getState: core.cwrap('tachyon_get_state', 'number', ['number']),
		setPollingMode: core.cwrap('tachyon_bus_set_polling_mode', null, ['number', 'number']),
	};

	// Scratch heap cells for C out-parameters. acquire_rx writes the type_id and the
	// actual size here; listen writes the out bus pointer. Reused across calls.
	const scratch = core._malloc(16);
	if (!scratch) throw new TachyonError('Cannot allocate WASM out-parameters.', ErrorCode.Mem);
	const OUT_TYPE_ID = scratch; // uint32_t
	const OUT_SIZE = scratch + 4; // size_t (32-bit on wasm32)
	const OUT_BUS = scratch + 8; // tachyon_bus_t*

	/** Maps a heap offset + length onto the live WASM memory as a zero-copy view. */
	function slot(ptr: number, len: number): Uint8Array {
		// HEAPU8.buffer is re-read every call because memory growth swaps the buffer.
		return new Uint8Array(core.HEAPU8.buffer, ptr, len);
	}

	// Scratch array of tachyon_msg_view_t for drainBatch. Grown on demand, never
	// shrunk (allocating can grow the heap and detach every live view), so it must
	// happen before any view is taken rather than between calls
	let batchScratch = 0;
	let batchScratchMsgs = 0;

	function batchViews(maxMsgs: number): number {
		if (maxMsgs <= batchScratchMsgs) {
			return batchScratch;
		}
		const next = core._malloc(maxMsgs * MSG_VIEW_SIZE);
		if (!next) {
			throw new TachyonError('Cannot allocate the batch scratch.', ErrorCode.Mem);
		}

		if (batchScratch) {
			core._free(batchScratch);
		}

		batchScratch = next;
		batchScratchMsgs = maxMsgs;
		return batchScratch;
	}

	interface BrowserEndpoint {
		/** Raw `tachyon_bus_t*` shared by the listener and every connected peer. */
		busPtr: number;
		refs: number;
		txOwner?: BrowserBusHandle;
		rxOwner?: BrowserBusHandle;
	}

	const endpoints = new Map<string, BrowserEndpoint>();

	class BrowserBusHandle implements BusHandle {
		#endpoint: BrowserEndpoint;
		#path: string;
		#batchOpen = false;
		#batchCount = 0;
		#closed = false;
		#txSize = 0;

		public constructor(path: string, endpoint: BrowserEndpoint) {
			this.#path = path;
			this.#endpoint = endpoint;
		}

		get #bus(): number {
			if (this.#closed) throw new Error('Bus: this bus has been closed.');
			if (this.#endpoint.busPtr === 0) {
				throw new Error('Bus: the underlying endpoint has been destroyed.');
			}
			return this.#endpoint.busPtr;
		}

		public get pointer(): number {
			return this.#bus;
		}

		public close(): void {
			if (this.#closed) return;
			if (this.#endpoint.txOwner === this) this.rollbackTx();
			if (this.#endpoint.rxOwner === this) this.commitRx();
			this.commitBatch();
			this.#closed = true;
			this.#endpoint.refs -= 1;
			if (this.#endpoint.refs <= 0) {
				endpoints.delete(this.#path);
				abi.busDestroy(this.#endpoint.busPtr);
				this.#endpoint.busPtr = 0;
			}
		}

		public send(data: Uint8Array, typeId = 0): void {
			validateUint(typeId, 0xffff_ffff, 'typeId');
			const target = this.acquireTx(data.length);
			try {
				target.set(data);
				this.commitTx(data.length, typeId);
			} catch (error) {
				this.rollbackTx();
				throw error;
			}
		}

		public acquireTx(maxSize: number): Uint8Array {
			validateUint(maxSize, MAX_CAPACITY, 'maxSize');
			if (this.#endpoint.txOwner) throw new Error('Bus: a TX guard is already active.');
			const ptr = abi.acquireTx(this.#bus, maxSize);
			if (ptr === 0) throw new TachyonError('Bus.acquireTx: the ring buffer is full.', ErrorCode.Full);
			this.#endpoint.txOwner = this;
			this.#txSize = maxSize;
			return slot(ptr, maxSize);
		}

		public commitTx(actualSize: number, typeId: number): void {
			this.commitTxUnflushed(actualSize, typeId);
			abi.flush(this.#bus);
		}

		public commitTxUnflushed(actualSize: number, typeId: number): void {
			if (this.#endpoint.txOwner !== this) throw new Error('Bus: no active TX guard.');
			validateUint(actualSize, this.#txSize, 'actualSize');
			validateUint(typeId, 0xffff_ffff, 'typeId');
			const rc = abi.commitTx(this.#bus, actualSize, typeId);
			delete this.#endpoint.txOwner;
			this.#txSize = 0;
			if (rc !== TACHYON_SUCCESS) throw new TachyonError('Bus: TX commit failed.', mapError(rc));
		}

		public rollbackTx(): void {
			if (this.#endpoint.txOwner !== this) return;
			abi.rollbackTx(this.#bus);
			delete this.#endpoint.txOwner;
			this.#txSize = 0;
		}

		public flush(): void {
			abi.flush(this.#bus);
		}

		public acquireRx(): RawRx | null {
			if (this.#endpoint.rxOwner) throw new Error('Bus: an RX guard is already active.');
			const ptr = abi.acquireRx(this.#bus, OUT_TYPE_ID, OUT_SIZE);
			if (ptr === 0) return null;
			this.#endpoint.rxOwner = this;
			const typeId = core.getValue(OUT_TYPE_ID, 'i32') >>> 0;
			const actualSize = core.getValue(OUT_SIZE, 'i32') >>> 0;
			return { data: slot(ptr, actualSize), typeId, actualSize };
		}

		public drainBatch(maxMsgs: number): RawBatchMessage[] {
			validateUint(maxMsgs, MAX_BATCH_MSGS, 'maxMsgs');
			if (this.#batchOpen) throw new Error('Bus: an RX batch is already active.');
			if (this.#endpoint.rxOwner) throw new Error('Bus: an RX guard is already active.');

			const views = batchViews(maxMsgs);
			const count = abi.acquireRxBatch(this.#bus, views, maxMsgs);
			this.#batchOpen = true;
			this.#batchCount = count;

			const messages: RawBatchMessage[] = [];
			for (let i = 0; i < count; i += 1) {
				const view = views + i * MSG_VIEW_SIZE;
				const size = core.getValue(view + VIEW_SIZE, 'i32') >>> 0;
				messages.push({
					data: slot(core.getValue(view + VIEW_PTR, 'i32'), size),
					typeId: core.getValue(view + VIEW_TYPE_ID, 'i32') >>> 0,
					size,
				});
			}

			return messages;
		}

		public commitRx(): void {
			if (this.#endpoint.rxOwner !== this) throw new Error('Bus: no active RX guard.');
			abi.commitRx(this.#bus);
			delete this.#endpoint.rxOwner;
		}

		public commitBatch(): void {
			if (!this.#batchOpen) return;
			this.#batchOpen = false;
			const count = this.#batchCount;
			this.#batchCount = 0;
			abi.commitRxBatch(this.#bus, batchScratch, count);
		}

		public setPollingMode(spinMode: number): void {
			// Browser delivery is direct and non-blocking, but the core still tracks
			// the pure-spin hint, so forward it to keep parity with the native path.
			abi.setPollingMode(this.#bus, spinMode);
		}

		public setNumaNode(_nodeId: number): void {
			// WASM memory is page-local and cannot be NUMA-bound from browser JS.
		}

		public getState(): number {
			return abi.getState(this.#bus);
		}
	}

	/**
	 * Creates a page-local ring through the C core and returns the raw bus pointer.
	 *
	 * @throws {TachyonError} If the core rejects the capacity or allocation fails.
	 */
	function listenBus(socketPath: string, capacity: number): number {
		core.setValue(OUT_BUS, 0, 'i32');
		const rc = abi.busListen(socketPath, capacity, OUT_BUS);
		if (rc !== TACHYON_SUCCESS) {
			throw new TachyonError(`Bus.listen: the core rejected the request (error ${rc}).`, mapError(rc));
		}

		const busPtr = core.getValue(OUT_BUS, 'i32');
		// tachyon_bus_get_shm_ptr exposes the arena base; a null base means the
		// allocation never mapped, so refuse to hand back an unusable bus.
		if (busPtr === 0 || abi.getShmPtr(busPtr) === 0) {
			if (busPtr !== 0) abi.busDestroy(busPtr);
			throw new TachyonError('Bus.listen: the core returned an unmapped bus.', ErrorCode.Mem);
		}
		return busPtr;
	}

	// GC safety net: if a Bus is dropped without close(), free the underlying core
	// bus. The held value is the handle (which never references the Bus), and the
	// unregister token is also the handle, so there is no strong cycle back to the
	// Bus instance and the registry can never pin it in memory.
	const busRegistry = new FinalizationRegistry<BrowserBusHandle>((handle) => {
		handle.close();
	});

	/**
	 * Browser implementation of the Tachyon SPSC bus.
	 *
	 * The ring is the fuzzed C++ engine, but the attach path is not: a page has no fds, so `tachyon_bus_connect`,
	 * `Arena::attach` and the handshake checks never run here. An ABI mismatch is undetectable on this transport.
	 */
	class Bus extends BusBase<Uint8Array> {
		readonly #handle: BrowserBusHandle;

		private constructor(path: string, endpoint: BrowserEndpoint) {
			const handle = new BrowserBusHandle(path, endpoint);
			super(handle, {
				defaultSpinThreshold: 0,
				retryNullRecv: false,
				copyData: (data) => new Uint8Array(data),
			});
			this.#handle = handle;
			busRegistry.register(this, handle, handle);
		}

		public get wasmPointer(): number {
			return this.#handle.pointer;
		}

		public override close(): void {
			busRegistry.unregister(this.#handle);
			super.close();
		}

		public static listen(socketPath: string, capacity: number): Bus {
			if (!Number.isInteger(capacity) || capacity <= 0) {
				throw new TachyonError('Bus.listen: capacity must be a positive integer.', ErrorCode.InvalidSz);
			}
			if (capacity > MAX_CAPACITY) {
				throw new TachyonError(
					`Bus.listen: capacity ${capacity} exceeds the wasm32 limit of ${MAX_CAPACITY}. `,
					ErrorCode.InvalidSz,
				);
			}
			if ((capacity & (capacity - 1)) !== 0) {
				throw new TachyonError('Bus.listen: capacity must be a power of two.', ErrorCode.InvalidSz);
			}
			if (endpoints.has(socketPath)) {
				throw new Error(`Bus.listen: browser endpoint already exists for ${socketPath}`);
			}

			if ([...endpoints.values()].some((e) => e.txOwner !== undefined || e.rxOwner !== undefined)) {
				throw new Error(
					'Bus.listen: a TX or RX guard is active. Allocating a ring can grow WASM memory and detach ' +
						'every existing view, release all guards first.',
				);
			}

			const busPtr = listenBus(socketPath, capacity);
			const endpoint: BrowserEndpoint = { busPtr, refs: 1 };
			endpoints.set(socketPath, endpoint);
			return new Bus(socketPath, endpoint);
		}

		/**
		 * Second handle onto the ring `Bus.listen` created at this path. No socket, no handshake, just a refcount.
		 * SPSC is upheld by the guard exclusion in this file, not by process separation.
		 *
		 * @throws {Error} If no bus is listening at `socketPath`.
		 */
		public static connect(socketPath: string): Bus {
			const endpoint = endpoints.get(socketPath);
			if (endpoint === undefined) {
				throw new Error(`Bus.connect: no browser endpoint is listening at ${socketPath}`);
			}

			endpoint.refs += 1;
			return new Bus(socketPath, endpoint);
		}
	}

	const bindings = { Bus };
	bindingsByModule.set(core, bindings);
	return bindings;
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
export { makeTypeId, msgType, routeId } from './type_id.ts';
