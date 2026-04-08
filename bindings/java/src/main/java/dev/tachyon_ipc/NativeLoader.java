package dev.tachyon_ipc;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;

/**
 * Dynamically loads the Tachyon native shared library (FFI Bridge).
 *
 * @apiNote Internal utility. Automatically detects the host OS and architecture
 * to extract the correct pre-compiled binary from the classpath.
 * @implNote Uses a double-checked locking mechanism for thread-safe static initialization.
 * The native binary is extracted to a temporary file before being loaded via {@link System#load(String)},
 * falling back to {@link System#loadLibrary(String)} on failure.
 */
final class NativeLoader {

	/**
	 * State flag preventing redundant extraction and linkage attempts across classloaders.
	 */
	private static volatile boolean loaded = false;

	/**
	 * Mutex for synchronizing the native extraction phase.
	 */
	private static final Object lock = new Object();

	private NativeLoader() {
	}

	/**
	 * Ensures the native library is loaded into the JVM context.
	 *
	 * @apiNote Safe to call multiple times concurrently. Only the initial successful call
	 * will map the native symbols.
	 */
	public static void load() {
		if (loaded) {
			return;
		}

		synchronized (lock) {
			if (loaded) {
				return;
			}

			doLoad();
			loaded = true;
		}
	}

	/**
	 * Resolves the platform-specific library, extracts it to the host's temporary directory,
	 * and invokes the native linker.
	 *
	 * @throws UnsupportedOperationException If Tachyon is load on an unsupported OS / architecture.
	 * @throws RuntimeException              If the library stream cannot be read or written to the filesystem.
	 * @throws UnsatisfiedLinkError          If the JVM cannot link the extracted binary.
	 */
	private static void doLoad() {
		final String osName = System.getProperty("os.name").toLowerCase();
		final String osArch = System.getProperty("os.arch").toLowerCase();
		final String prefix = "lib";
		String os;
		String extension;

		if (osName.contains("linux")) {
			os = "linux";
			extension = ".so";
		} else if (osName.contains("mac")) {
			os = "darwin";
			extension = ".dylib";
		} else {
			throw new UnsupportedOperationException("Unsupported OS: " + osName);
		}

		String arch;
		if (osArch.contains("amd64") || osArch.contains("x86_64")) {
			arch = "x86_64";
		} else if (osArch.contains("aarch64") || osArch.contains("arm64")) {
			arch = os.equals("darwin") ? "arm64" : "aarch64";
		} else {
			throw new UnsupportedOperationException("Unsupported architecture: " + osArch);
		}

		String platform = os + "-" + arch;
		String libName = prefix + "tachyon" + extension;
		String resourcePath = "/native/" + platform + "/" + libName;

		try (InputStream is = NativeLoader.class.getResourceAsStream(resourcePath)) {
			if (is == null) {
				loadFromJavaLibPath();
				return;
			}

			Path tempFile = Files.createTempFile("libtachyon_", extension);
			tempFile.toFile().deleteOnExit();
			Files.copy(is, tempFile, StandardCopyOption.REPLACE_EXISTING);

			try {
				System.load(tempFile.toAbsolutePath().toString());
			} catch (UnsatisfiedLinkError e) {
				throw new UnsatisfiedLinkError("Failed to load Tachyon native library from " + resourcePath +
						" (extracted to " + tempFile + "): " + e.getMessage()
				);
			}
		} catch (IOException exception) {
			throw new RuntimeException("I/O error while extracting Tachyon native library: " + resourcePath, exception);
		}
	}

	/**
	 * Fallback linkage strategy resolving against standard system paths (e.g., LD_LIBRARY_PATH).
	 *
	 * @throws UnsatisfiedLinkError If the system library path lacks the tachyon binary.
	 */
	private static void loadFromJavaLibPath() {
		try {
			System.loadLibrary("tachyon");
		} catch (UnsatisfiedLinkError error) {
			throw new UnsatisfiedLinkError("Failed to load Tachyon native library from java.library.path: " + error.getMessage());
		}
	}
}
