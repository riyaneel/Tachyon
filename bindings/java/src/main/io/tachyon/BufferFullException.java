package io.tachyon;

public final class BufferFullException extends TachyonException {
	public BufferFullException() {
		super(9, "The bus buffer is full. No transaction could be acquired.");
	}
}
