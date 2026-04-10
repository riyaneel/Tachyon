/**
 * Error codes mirroring tachyon_error_code_str() in tachyon_node.cpp.
 */
export const ErrorCode = {
	NullPtr: 'ERR_TACHYON_NULL_PTR',
	Mem: 'ERR_TACHYON_MEM',
	Open: 'ERR_TACHYON_OPEN',
	Truncate: 'ERR_TACHYON_TRUNCATE',
	Chmod: 'ERR_TACHYON_CHMOD',
	Seal: 'ERR_TACHYON_SEAL',
	Map: 'ERR_TACHYON_MAP',
	InvalidSz: 'ERR_TACHYON_INVALID_SZ',
	Full: 'ERR_TACHYON_FULL',
	Empty: 'ERR_TACHYON_EMPTY',
	Network: 'ERR_TACHYON_NETWORK',
	System: 'ERR_TACHYON_SYSTEM',
	Interrupted: 'ERR_TACHYON_INTERRUPTED',
	AbiMismatch: 'ERR_TACHYON_ABI_MISMATCH',
	Unknown: 'ERR_TACHYON_UNKNOWN',
} as const;

export type ErrorCode = (typeof ErrorCode)[keyof typeof ErrorCode];

/**
 * Base class for all errors crossing the N-API boundary.
 */
export class TachyonError extends Error {
	/** Native C-API error code, set as a `.code` property by N-API. */
	public readonly code: ErrorCode;

	public constructor(message: string, code: ErrorCode) {
		super(message);
		this.name = 'TachyonError';
		this.code = code;
		Object.setPrototypeOf(this, new.target.prototype);
	}
}

/**
 * Thrown when the TachyonHandshake fails.
 * Connection is rejected before the first SCM_RIGHTS exchange if the producer
 * and consumer were compiled with differing Tachyon versions or TACHYON_MSG_ALIGNMENT values.
 */
export class AbiMismatchError extends TachyonError {
	public constructor() {
		super(
			'ABI mismatch: incompatible Tachyon versions or TACHYON_MSG_ALIGNMENT. ' +
				'Rebuild producer and consumer from the same tag.',
			ErrorCode.AbiMismatch,
		);
		this.name = 'AbiMismatchError';
		Object.setPrototypeOf(this, new.target.prototype);
	}
}

/**
 * Thrown when the bus transitions to TACHYON_STATE_FATAL_ERROR.
 *
 * Triggered exclusively by a corrupted message header detected in acquire_rx —
 * not by a producer crash. A crashed producer causes the consumer to block
 * indefinitely; use an external supervisor if dead-peer detection is required.
 *
 * This error is raised by Bus after polling tachyon_get_state(), not
 * thrown directly by the native binding — there is no corresponding tachyon_error_t.
 */
export class PeerDeadError extends TachyonError {
	public constructor() {
		super(
			'Bus entered FATAL_ERROR state: corrupted message header detected. ' + 'Close this bus immediately.',
			ErrorCode.Unknown,
		);
		this.name = 'PeerDeadError';
		Object.setPrototypeOf(this, new.target.prototype);
	}
}

/**
 * Returns true for any error originating from the Tachyon C++ binding.
 */
export function isTachyonError(err: unknown): err is TachyonError {
	return (
		err instanceof Error &&
		'code' in err &&
		typeof (err as { code: unknown }).code === 'string' &&
		(err as { code: string }).code.startsWith('ERR_TACHYON_')
	);
}

/**
 * Returns true if the error is an ABI contract violation detected during handshake.
 */
export function isAbiMismatch(err: unknown): err is AbiMismatchError {
	return isTachyonError(err) && err.code === ErrorCode.AbiMismatch;
}

/**
 * Returns true if the bus has entered a fatal error state.
 * Uses instanceof rather than .code — PeerDeadError is raised by Bus, not the native binding.
 */
export function isPeerDead(err: unknown): err is PeerDeadError {
	return err instanceof PeerDeadError;
}

/**
 * Returns true if the SPSC ring buffer is full. Expected on the TX hot path when the producer outpaces the consumer.
 */
export function isFull(err: unknown): err is TachyonError {
	return isTachyonError(err) && err.code === ErrorCode.Full;
}
