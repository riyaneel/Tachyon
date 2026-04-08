package dev.tachyon_ipc;

/**
 * Thrown when the SPSC ring buffer has no available capacity.
 *
 * @implNote Deliberate: Treated as a control-flow signal in non-blocking paths.
 * The calling thread or coroutine is expected to back off (yield/spin) and retry.
 */
public final class BufferFullException extends TachyonException {

	/**
	 * Constructs a BufferFullException mapping to TACHYON_ERR_FULL (code 9).
	 */
	public BufferFullException() {
		super(9, "The bus buffer is full. No transaction could be acquired.");
	}
}
