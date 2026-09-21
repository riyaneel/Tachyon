import { copyFile, mkdir } from 'node:fs/promises';
import { resolve } from 'node:path';

// tsc only emits the TypeScript sources; the generated Emscripten module
// (tachyon.js + tachyon.wasm) is a plain asset, so copy it next to the compiled
// browser entry at dist/wasm/ where `dist/browser.js` imports it from.
const SRC_DIR = resolve('src/ts/wasm');
const DIST_DIR = resolve('dist/wasm');

async function main() {
	await mkdir(DIST_DIR, { recursive: true });
	for (const file of ['tachyon.js', 'tachyon.wasm', 'tachyon.d.ts']) {
		await copyFile(resolve(SRC_DIR, file), resolve(DIST_DIR, file));
	}
	console.log('WASM artefacts staged into dist/wasm');
}

main().catch((err) => {
	if (err.code === 'ENOENT') {
		console.error(`Missing WASM artefact: ${err.path}.` + 'Run `npm run build:wasm`\n');
		process.exit(1);
	}
	console.error('Failed to stage WASM artefacts: ', err);
	process.exit(1);
});
