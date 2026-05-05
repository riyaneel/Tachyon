import { createRequire } from 'node:module';
import { AbiMismatchError, ErrorCode, PeerDeadError, isTachyonError } from './error.ts';

const _require = createRequire(import.meta.url);

interface NativeRxResult {
	data: Buffer;
	msgType: number;
	actualSize: number;
}

interface NativeServeResult extends NativeRxResult {
	correlationId: bigint;
}

interface NativeRpcBinding {
	close(): void;
	call(data: Buffer | Uint8Array, msgType: number): { correlationId: bigint };
	wait(correlationId: bigint, spinThreshold?: number): NativeRxResult | null;
	commitRx(): void;
	serve(spinThreshold?: number): NativeServeResult | null;
	commitServe(): void;
	reply(correlationId: bigint, data: Buffer | Uint8Array, msgType: number): void;
	setPollingMode(spinMode: number): void;
	getState(): number;
}

interface NativeRpcModule {
	TachyonRpcBusNode: {
		listen(path: string, capFwd: number, capRev: number): NativeRpcBinding;
		connect(path: string): NativeRpcBinding;
	};
}

function loadNative(): NativeRpcModule {
	const candidates = [
		new URL('../../build/Release/tachyon_node.node', import.meta.url).pathname,
		new URL('../../build/Debug/tachyon_node.node', import.meta.url).pathname,
	];
	for (const p of candidates) {
		try {
			return _require(p) as NativeRpcModule;
		} catch {
			/* try next */
		}
	}
	throw new Error(
		'tachyon_node.node not found. Run `npm run build:native` first.\n' + `Searched: ${candidates.join(', ')}`,
	);
}

const native = loadNative();

const STATE_FATAL_ERROR = 4;

/**
 * Zero-copy read lease on an RPC ring buffer slot.
 *
 * Obtain via {@link RpcBus.wait} or {@link RpcBus.serve}.
 * Call {@link commit} once the payload is consumed, or use `using`.
 * Failing to commit holds the consumer head indefinitely.
 */
export class RpcRxGuard implements Disposable {
	#handle: NativeRpcBinding;
	#buffer: Buffer | null;
	#done = false;
	#isServe: boolean;

	/** Message type discriminator set by the sender. */
	public readonly msgType: number;

	/** Exact payload size in bytes. */
	public readonly actualSize: number;

	/**
	 * Correlation ID of this message.
	 * On the callee side (from {@link RpcBus.serve}), pass unchanged to {@link RpcBus.reply}.
	 */
	public readonly correlationId: bigint;

	/** @internal */
	public constructor(
		handle: NativeRpcBinding,
		buffer: Buffer,
		msgType: number,
		actualSize: number,
		correlationId: bigint,
		isServe: boolean,
	) {
		this.#handle = handle;
		this.#buffer = buffer;
		this.msgType = msgType;
		this.actualSize = actualSize;
		this.correlationId = correlationId;
		this.#isServe = isServe;
	}

	/**
	 * Returns the zero-copy read window into shared memory.
	 * Invalidated on {@link commit}; any cached reference will throw `TypeError`.
	 *
	 * @throws {Error} If the slot has already been committed.
	 * @throws {PeerDeadError} If the bus has entered TACHYON_STATE_FATAL_ERROR.
	 */
	public data(): Buffer {
		this.#assertOpen();
		if (this.#handle.getState() === STATE_FATAL_ERROR) throw new PeerDeadError();
		// eslint-disable-next-line @typescript-eslint/no-non-null-assertion
		return this.#buffer!;
	}

	/**
	 * Releases the slot and advances the consumer head. No-op if already committed.
	 */
	public commit(): void {
		if (this.#done) return;
		this.#done = true;
		this.#buffer = null;
		if (this.#isServe) {
			this.#handle.commitServe();
		} else {
			this.#handle.commitRx();
		}
	}

	/** Called automatically by the `using` keyword. */
	public [Symbol.dispose](): void {
		this.commit();
	}

	#assertOpen(): void {
		if (this.#done) throw new Error('RpcRxGuard: slot has already been committed.');
	}
}

/**
 * Tachyon bidirectional RPC bus backed by two independent SPSC arenas.
 *
 * Start the callee first: it owns the UNIX socket and both SHM arenas.
 * All blocking operations spin then park on a futex; call them from a Worker
 * thread to avoid saturating the main event loop.
 *
 * `using` closes the bus automatically.
 *
 * @example
 * ```ts
 * // callee (start first)
 * using bus = RpcBus.listen('/tmp/rpc.sock', 1 << 16, 1 << 16);
 * const req = bus.serve();
 * if (req) {
 *   const cid = req.correlationId;
 *   req.commit();
 *   bus.reply(cid, Buffer.from('pong'), 2);
 * }
 *
 * // caller
 * using bus = RpcBus.connect('/tmp/rpc.sock');
 * const { correlationId } = bus.call(Buffer.from('ping'), 1);
 * using resp = bus.wait(correlationId);
 * ```
 */
export class RpcBus implements Disposable {
	#handle: NativeRpcBinding;
	#closed = false;

	private constructor(handle: NativeRpcBinding) {
		this.#handle = handle;
	}

	/**
	 * Creates two SHM arenas and blocks until a caller connects.
	 * EINTR is retried transparently inside the native layer.
	 *
	 * @param socketPath UDS socket path. Unlinked automatically after handshake.
	 * @param capFwd arena_fwd capacity in bytes (caller to callee). Must be a power of two.
	 * @param capRev arena_rev capacity in bytes (callee to caller). Must be a power of two.
	 */
	public static listen(socketPath: string, capFwd: number, capRev: number): RpcBus {
		return new RpcBus(native.TachyonRpcBusNode.listen(socketPath, capFwd, capRev));
	}

	/**
	 * Connects to an existing SHM arena via the UNIX socket at `socketPath`.
	 *
	 * @throws {AbiMismatchError} If producer and consumer were compiled with incompatible versions.
	 */
	public static connect(socketPath: string): RpcBus {
		try {
			return new RpcBus(native.TachyonRpcBusNode.connect(socketPath));
		} catch (err) {
			if (isTachyonError(err) && err.code === ErrorCode.AbiMismatch) throw new AbiMismatchError();
			throw err;
		}
	}

	/**
	 * Controls the polling back-off strategy for both arenas.
	 * Call before the first message.
	 * When `spinMode` is `1`, the producer skips the futex wake check on every flush.
	 * Only use this when both consumer threads are dedicated and never park.
	 */
	public setPollingMode(spinMode: 0 | 1): void {
		this.#assertOpen();
		this.#handle.setPollingMode(spinMode);
	}

	/**
	 * Copies `data` into arena_fwd and returns the assigned correlation ID.
	 * Blocks if the ring buffer is full.
	 *
	 * @param data Request payload.
	 * @param msgType Application-level message type.
	 * @returns The correlation ID to pass to {@link wait}.
	 */
	public call(data: Buffer | Uint8Array, msgType: number): bigint {
		this.#assertOpen();
		return this.#handle.call(data, msgType).correlationId;
	}

	/**
	 * Blocks until the response matching `correlationId` arrives in arena_rev.
	 * Returns `null` on EINTR; caller should retry.
	 * A correlation ID mismatch triggers a fatal error on arena_rev.
	 *
	 * @throws {PeerDeadError} If the bus has entered TACHYON_STATE_FATAL_ERROR.
	 */
	public wait(correlationId: bigint, spinThreshold = 10_000): RpcRxGuard | null {
		this.#assertOpen();
		if (this.#handle.getState() === STATE_FATAL_ERROR) throw new PeerDeadError();
		const result = this.#handle.wait(correlationId, spinThreshold);
		if (result === null) return null;
		return new RpcRxGuard(this.#handle, result.data, result.msgType, result.actualSize, correlationId, false);
	}

	/**
	 * Blocks until a request arrives in arena_fwd.
	 * Returns `null` on EINTR; caller should retry.
	 * Commit the returned guard before calling {@link reply} to avoid holding
	 * both arena slots simultaneously.
	 *
	 * @throws {PeerDeadError} If the bus has entered TACHYON_STATE_FATAL_ERROR.
	 */
	public serve(spinThreshold = 10_000): RpcRxGuard | null {
		this.#assertOpen();
		if (this.#handle.getState() === STATE_FATAL_ERROR) throw new PeerDeadError();
		const result = this.#handle.serve(spinThreshold);
		if (result === null) return null;
		return new RpcRxGuard(this.#handle, result.data, result.msgType, result.actualSize, result.correlationId, true);
	}

	/**
	 * Copies `data` into arena_rev as a response to `correlationId`.
	 * The guard from {@link serve} must be committed before calling this method.
	 *
	 * @param correlationId Must match the value from the served {@link RpcRxGuard}. Must be > 0.
	 * @param data Response payload.
	 * @param msgType Application-level response type.
	 */
	public reply(correlationId: bigint, data: Buffer | Uint8Array, msgType: number): void {
		this.#assertOpen();
		this.#handle.reply(correlationId, data, msgType);
	}

	/** Closes the bus and unmaps both SHM arenas. Safe to call multiple times. */
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
		if (this.#closed) throw new Error('RpcBus: this bus has been closed.');
	}
}
