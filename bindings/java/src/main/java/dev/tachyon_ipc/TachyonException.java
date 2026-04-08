package dev.tachyon_ipc;

/**
 * Base runtime exception for Tachyon FFI boundary errors.
 * Maps native C++ error codes to JVM exceptions.
 */
public class TachyonException extends RuntimeException {
	private final int code;

	/**
	 * @param code The native C++ error code.
	 * @param message The detail message.
	 */
	public TachyonException(int code, String message) {
		super(message);
		this.code = code;
	}

	/**
	 * @return The underlying native error code.
	 */
	public int getCode() {
		return code;
	}

	/**
	 * Maps a native error code to a specific exception subclass.
	 *
	 * @param code The native error code returned by the C API.
	 * @throws IllegalArgumentException If the provided code is 0.
	 * @return A specific subclass of {@link TachyonException}.
	 * @apiNote Expects a non-zero code. 0 represents success in the native ABI.
	 */
	public static TachyonException fromCode(int code) {
		if (code == 0) {
			throw new IllegalArgumentException("Cannot create an exception for success code (0)");
		}

		return switch (code) {
			case 9  -> new BufferFullException();
			case 12 -> new TachyonException(code, "OS-level system error (errno). Check system resources.");
			case 13 -> new TachyonException(code, "Blocking call interrupted by signal (EINTR). Retry the operation.");
			case 14 -> new AbiMismatchException();
			default -> new TachyonException(code, "Tachyon native error occurred. Error code: " + code);
		};
	}
}
