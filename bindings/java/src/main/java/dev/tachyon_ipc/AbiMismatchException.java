package dev.tachyon_ipc;

public final class AbiMismatchException extends TachyonException {
	public AbiMismatchException() {
		super(14, "ABI version mismatch between client and Tachyon bus.");
	}
}
