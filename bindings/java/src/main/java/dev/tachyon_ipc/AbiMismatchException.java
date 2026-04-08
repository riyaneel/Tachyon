package dev.tachyon_ipc;

/**
 * Thrown during connection handshake if the shared memory layout or protocol version
 * differs between the client and the bus.
 */
public final class AbiMismatchException extends TachyonException {
	public AbiMismatchException() {
		super(14, "ABI version mismatch between client and Tachyon bus.");
	}
}
