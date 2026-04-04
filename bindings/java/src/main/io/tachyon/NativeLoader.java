package io.tachyon;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;

final class NativeLoader {
	private static volatile boolean loaded = false;
	private static final Object lock = new Object();

	private NativeLoader() {
	}

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

	private static void loadFromJavaLibPath() {
		try {
			System.loadLibrary("tachyon");
		} catch (UnsatisfiedLinkError error) {
			throw new UnsatisfiedLinkError("Failed to load Tachyon native library.\n" +
					"Embedded resource not found in JAR classpath and Fallback failed. 'tachyon' not found in java.library.path.\n" +
					"Error: " + error.getMessage()
			);
		}
	}
}
