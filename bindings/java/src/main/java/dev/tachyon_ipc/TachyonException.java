package dev.tachyon_ipc;

/**
 * Base runtime exception for Tachyon FFI boundary errors.
 * Maps native C++ error codes to JVM exceptions.
 *
 * @implSpec Exceptions thrown across the FFI boundary must strictly subclass this.
 */
public class TachyonException extends RuntimeException {

	/**
	 * The native C++ error code returned by the ABI.
	 */
	private final int code;

	/**
	 * Constructs a new Tachyon exception with the specified native error code and detail message.
	 *
	 * @param code    The native C++ error code.
	 * @param message The detail message explaining the error.
	 */
	public TachyonException(int code, String message) {
		super(message);
		this.code = code;
	}

	/**
	 * Retrieves the underlying native error code associated with this exception.
	 *
	 * @return The integer error code.
	 */
	public int getCode() {
		return code;
	}

	/**
	 * Maps a native error code to a specific exception subclass.
	 *
	 * @param code The native error code returned by the C API.
	 * @return A specific subclass of {@link TachyonException}.
	 * @throws IllegalArgumentException If the provided code is 0.
	 * @apiNote Expects a non-zero code. 0 represents success in the native ABI.
	 */
	public static TachyonException fromCode(int code) {
		if (code == 0) {
			throw new IllegalArgumentException("Cannot create an exception for success code (0)");
		}

		return switch (code) {
			case 9 -> new BufferFullException();
			case 12 -> new TachyonException(code, "OS-level system error (errno). Check system resources.");
			case 13 -> new TachyonException(code, "Blocking call interrupted by signal (EINTR). Retry the operation.");
			case 14 -> new AbiMismatchException();
			default -> new TachyonException(code, "Tachyon native error occurred. Error code: " + code);
		};
	}
}
