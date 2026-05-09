import { mkdir, writeFile } from "node:fs/promises";
import { homedir } from "node:os";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const exampleRoot = resolve(__dirname, "..");
const hostPath = resolve(
  exampleRoot,
  "native_host/target/release/tachyon-native-messaging-host",
);

const browser = process.argv[2] ?? "chrome";
const extensionId = process.argv[3] ?? "<replace-with-extension-id>";

const locationsByPlatform = {
  darwin: {
    chrome: `${homedir()}/Library/Application Support/Google/Chrome/NativeMessagingHosts`,
    chrome_for_testing: `${homedir()}/Library/Application Support/Google/ChromeForTesting/NativeMessagingHosts`,
    chromium: `${homedir()}/Library/Application Support/Chromium/NativeMessagingHosts`,
  },
  linux: {
    chrome: `${homedir()}/.config/google-chrome/NativeMessagingHosts`,
    chrome_for_testing: `${homedir()}/.config/google-chrome-for-testing/NativeMessagingHosts`,
    chromium: `${homedir()}/.config/chromium/NativeMessagingHosts`,
  },
};

const dir = locationsByPlatform[process.platform]?.[browser];
if (!dir) {
  throw new Error(
    `unknown platform/browser '${process.platform}/${browser}', expected chrome, chrome_for_testing, or chromium on macOS/Linux`,
  );
}

const manifestPath = `${dir}/tachyon.native_messaging_host.json`;
const manifest = {
  name: "tachyon.native_messaging_host",
  description: "Tachyon native messaging example host",
  path: hostPath,
  type: "stdio",
  allowed_origins: [`chrome-extension://${extensionId}/`],
};

await mkdir(dir, { recursive: true });
await writeFile(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`);
console.log(manifestPath);
