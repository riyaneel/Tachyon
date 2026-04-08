package dev.tachyon_ipc;

/**
 * Thrown when the remote process disconnects unexpectedly, crashes, or the arena enters
 * a fatal state.
 *
 * @apiNote The underlying FFM memory segments may be corrupted. The bus instance must be
 * closed immediately upon catching this.
 */
public final class PeerDeadException extends TachyonException {
	public PeerDeadException() {
		super(-1, "The peer process is dead, or the bus has entered a FATAL_ERROR state.");
	}
}
