package io.tachyon;

public final class PeerDeadException extends TachyonException {
	public PeerDeadException() {
		super(-1, "The peer process is dead, or the bus has entered a FATAL_ERROR state.");
	}
}
