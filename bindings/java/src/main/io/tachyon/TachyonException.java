package io.tachyon;

public class TachyonException extends RuntimeException {

	private final int code;

	public TachyonException(int code, String message) {
		super(message);
		this.code = code;
	}

	public int getCode() {
		return code;
	}

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
