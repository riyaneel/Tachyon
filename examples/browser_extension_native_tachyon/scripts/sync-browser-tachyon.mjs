import { cp, mkdir, rm } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(__dirname, "../../..");
const source = resolve(repoRoot, "bindings/node/dist/wasm");
const destination = resolve(
  __dirname,
  "../extension/vendor/@tachyon-ipc/core/wasm",
);

await rm(destination, { recursive: true, force: true });
await mkdir(destination, { recursive: true });
for (const file of ["tachyon_ipc.js", "tachyon_ipc_bg.js", "tachyon_ipc_bg.wasm"]) {
  await cp(resolve(source, file), resolve(destination, file));
}
console.log(destination);
