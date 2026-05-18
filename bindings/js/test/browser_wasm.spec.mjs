import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { mkdtemp, rm } from 'node:fs/promises';
import { existsSync, readdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, extname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const PACKAGE_ROOT = resolve(__dirname, '..');
const HOME = process.env.HOME ?? '';

const TEST_PAGE = `<!doctype html>
<meta charset="utf-8">
<script type="importmap">
{
  "imports": {
    "@tachyon-ipc/core": "/dist/browser.js"
  }
}
</script>
<script type="module">
import { Bus, makeTypeId, msgType, routeId } from "@tachyon-ipc/core";

const results = [];
const assert = {
  equal(actual, expected) {
    if (actual !== expected) throw new Error(\`expected \${actual} to equal \${expected}\`);
  },
  notEqual(actual, expected) {
    if (actual === expected) throw new Error(\`expected \${actual} not to equal \${expected}\`);
  },
  deepEqual(actual, expected) {
    const actualJson = JSON.stringify(actual);
    const expectedJson = JSON.stringify(expected);
    if (actualJson !== expectedJson) throw new Error(\`expected \${actualJson} to equal \${expectedJson}\`);
  },
  throws(fn, pattern) {
    try {
      fn();
    } catch (error) {
      if (pattern.test(String(error?.message || error))) return;
      throw new Error(\`error did not match \${pattern}: \${error?.message || error}\`);
    }
    throw new Error("expected function to throw");
  },
};
const record = (name, fn) => {
  try {
    fn();
    results.push({ name, ok: true });
  } catch (error) {
    results.push({ name, ok: false, message: error?.stack || String(error) });
  }
};

record("package keeps browser import shape", () => {
  assert.equal(typeof Bus.listen, "function");
  assert.equal(typeof Bus.connect, "function");
  assert.equal(typeof makeTypeId, "function");
});

record("listen/connect lifecycle mirrors Node API", () => {
  const consumer = Bus.listen("/browser/lifecycle", 1 << 16);
  const producer = Bus.connect("/browser/lifecycle");
  producer.close();
  producer.close();
  consumer.close();
  assert.throws(() => Bus.connect("/browser/lifecycle"), /no browser endpoint/);
});

record("copy send/recv preserves payload and type_id", () => {
  const consumer = Bus.listen("/browser/send-recv", 1 << 16);
  const producer = Bus.connect("/browser/send-recv");
  const typeId = makeTypeId(3, 42);

  producer.send(new Uint8Array([1, 2, 3, 4]), typeId);
  const msg = consumer.recv();

  assert.deepEqual([...msg.data], [1, 2, 3, 4]);
  assert.equal(msg.typeId, typeId);
  assert.equal(routeId(msg.typeId), 3);
  assert.equal(msgType(msg.typeId), 42);
  producer.close();
  consumer.close();
});

record("zero-copy tx/rx guards and rollback work", () => {
  const consumer = Bus.listen("/browser/guards", 1 << 16);
  const producer = Bus.connect("/browser/guards");

  const rolledBack = producer.acquireTx(16);
  rolledBack.bytes().set([9, 9, 9, 9]);
  rolledBack.rollback();
  assert.equal(consumer.acquireRx(), null);

  const tx = producer.acquireTx(16);
  tx.bytes().set([5, 6, 7, 8]);
  tx.commit(4, 7);
  assert.throws(() => tx.bytes(), /already been committed/);

  const rx = consumer.acquireRx();
  assert.notEqual(rx, null);
  assert.equal(rx.typeId, 7);
  assert.equal(rx.actualSize, 4);
  assert.deepEqual([...rx.data()], [5, 6, 7, 8]);
  rx.commit();
  assert.throws(() => rx.data(), /already been committed/);

  producer.close();
  consumer.close();
});

record("commitUnflushed stays invisible until flush", () => {
  const consumer = Bus.listen("/browser/flush", 1 << 16);
  const producer = Bus.connect("/browser/flush");

  const tx = producer.acquireTx(8);
  tx.bytes().set([1, 1, 2, 3]);
  tx.commitUnflushed(4, 11);
  assert.equal(consumer.acquireRx(), null);

  producer.flush();
  const rx = consumer.acquireRx();
  assert.notEqual(rx, null);
  assert.equal(rx.typeId, 11);
  assert.deepEqual([...rx.data()], [1, 1, 2, 3]);
  rx.commit();

  producer.close();
  consumer.close();
});

record("drainBatch returns ordered messages", () => {
  const consumer = Bus.listen("/browser/batch", 1 << 16);
  const producer = Bus.connect("/browser/batch");

  for (let i = 0; i < 3; i += 1) {
    const tx = producer.acquireTx(4);
    tx.bytes().set([i, i + 1, i + 2, i + 3]);
    tx.commitUnflushed(4, 100 + i);
  }
  producer.flush();

  const batch = consumer.drainBatch(8);
  assert.equal(batch.length, 3);
  assert.equal(batch.at(0).typeId, 100);
  const cached = batch.at(0).data;
  assert.deepEqual([...batch.at(1).data], [1, 2, 3, 4]);
  assert.deepEqual([...batch].map((msg) => msg.typeId), [100, 101, 102]);
  batch.commit();
  assert.equal(cached.byteLength, 0);
  assert.throws(() => batch.at(0), /already been committed/);

  producer.close();
  consumer.close();
});

window.__tachyonBrowserResults = results;
window.__tachyonBrowserDone = true;
</script>`;

function chromiumPath() {
	const candidates = [
		process.env.CHROMIUM_BIN,
		'/usr/bin/chromium',
		'/usr/bin/chromium-browser',
		'/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
		'/Applications/Google Chrome Canary.app/Contents/MacOS/Google Chrome Canary',
		'/Applications/Chromium.app/Contents/MacOS/Chromium',
		...playwrightChromiumCandidates(),
	].filter(Boolean);

	for (const candidate of candidates) {
		if (candidate !== undefined && existsSync(candidate)) return candidate;
	}
	throw new Error(
		`Chromium not found. Set CHROMIUM_BIN, install /usr/bin/chromium, or install a Chrome/Chromium app.`,
	);
}

function playwrightChromiumCandidates() {
	const roots = [
		HOME === '' ? undefined : join(HOME, 'Library/Caches/ms-playwright'),
		HOME === '' ? undefined : join(HOME, '.cache/ms-playwright'),
		process.env.PLAYWRIGHT_BROWSERS_PATH,
	].filter(Boolean);
	const candidates = [];

	for (const root of roots) {
		if (root === undefined || !existsSync(root)) continue;
		for (const entry of readdirSync(root, { withFileTypes: true })) {
			if (!entry.isDirectory() || !entry.name.startsWith('chromium')) continue;
			const dir = join(root, entry.name);
			candidates.push(
				join(dir, 'chrome-linux/chrome'),
				join(dir, 'chrome-mac/Chromium.app/Contents/MacOS/Chromium'),
				join(dir, 'chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing'),
				join(dir, 'chrome-headless-shell-linux64/chrome-headless-shell'),
				join(dir, 'chrome-headless-shell-mac-arm64/chrome-headless-shell'),
			);
		}
	}

	return candidates;
}

function mimeType(pathname) {
	switch (extname(pathname)) {
		case '.html':
			return 'text/html; charset=utf-8';
		case '.js':
			return 'text/javascript; charset=utf-8';
		case '.wasm':
			return 'application/wasm';
		default:
			return 'application/octet-stream';
	}
}

async function startServer() {
	const { readFile } = await import('node:fs/promises');
	const server = createServer(async (req, res) => {
		try {
			const url = new URL(req.url ?? '/', 'http://127.0.0.1');
			if (url.pathname === '/' || url.pathname === '/index.html') {
				res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
				res.end(TEST_PAGE);
				return;
			}

			const filePath = resolve(PACKAGE_ROOT, `.${url.pathname}`);
			if (!filePath.startsWith(PACKAGE_ROOT)) {
				res.writeHead(403);
				res.end('forbidden');
				return;
			}

			const body = await readFile(filePath);
			res.writeHead(200, { 'content-type': mimeType(filePath) });
			res.end(body);
		} catch (error) {
			res.writeHead(404);
			res.end(String(error));
		}
	});

	await new Promise((resolveListen) => server.listen(0, '127.0.0.1', resolveListen));
	const address = server.address();
	assert.equal(typeof address, 'object');
	return { server, port: address.port };
}

async function waitForJson(url, timeoutMs = 10_000) {
	const started = Date.now();
	for (;;) {
		try {
			const res = await fetch(url);
			if (res.ok) return await res.json();
		} catch {
			// Chromium may still be starting.
		}
		if (Date.now() - started > timeoutMs) throw new Error(`Timed out waiting for ${url}`);
		await new Promise((resolveWait) => setTimeout(resolveWait, 50));
	}
}

async function openPage(debugPort, url) {
	const target = await fetch(`http://127.0.0.1:${debugPort}/json/new?${encodeURIComponent(url)}`, {
		method: 'PUT',
	});
	if (!target.ok) throw new Error(`Failed to open browser page: ${await target.text()}`);
	return target.json();
}

async function runCdp(webSocketDebuggerUrl) {
	const ws = new WebSocket(webSocketDebuggerUrl);
	let nextId = 0;
	const pending = new Map();

	ws.addEventListener('message', (event) => {
		const msg = JSON.parse(event.data);
		if (msg.id === undefined || !pending.has(msg.id)) return;
		const { resolve: resolveMessage, reject } = pending.get(msg.id);
		pending.delete(msg.id);
		if (msg.error !== undefined) reject(new Error(JSON.stringify(msg.error)));
		else resolveMessage(msg.result);
	});

	await new Promise((resolveOpen, rejectOpen) => {
		ws.addEventListener('open', resolveOpen, { once: true });
		ws.addEventListener('error', rejectOpen, { once: true });
	});

	const call = (method, params = {}) => {
		const id = ++nextId;
		ws.send(JSON.stringify({ id, method, params }));
		return new Promise((resolveCall, rejectCall) => pending.set(id, { resolve: resolveCall, reject: rejectCall }));
	};

	const evaluate = async (expression, timeout = 15_000) => {
		const result = await call('Runtime.evaluate', {
			expression,
			awaitPromise: true,
			returnByValue: true,
			timeout,
		});
		if (result.exceptionDetails !== undefined) throw new Error(JSON.stringify(result.exceptionDetails));
		return result.result.value;
	};

	await call('Runtime.enable');
	await evaluate(`new Promise((resolve, reject) => {
  const started = performance.now();
  const tick = () => {
    if (window.__tachyonBrowserDone) resolve(true);
    else if (performance.now() - started > 10000) reject(new Error("browser wasm tests timed out"));
    else setTimeout(tick, 25);
  };
  tick();
})`);
	const results = JSON.parse(await evaluate('JSON.stringify(window.__tachyonBrowserResults)'));
	ws.close();
	return results;
}

const { server, port } = await startServer();
const debugPort = 9333 + Math.floor(Math.random() * 1000);
const userDataDir = await mkdtemp(join(tmpdir(), 'tachyon-browser-wasm-'));
const browser = spawn(chromiumPath(), [
	'--headless=new',
	'--disable-gpu',
	'--no-first-run',
	'--no-default-browser-check',
	'--no-sandbox',
	`--remote-debugging-port=${debugPort}`,
	`--user-data-dir=${userDataDir}`,
	`http://127.0.0.1:${port}/`,
]);

browser.stderr.on('data', (chunk) => {
	if (process.env.TACHYON_BROWSER_TEST_DEBUG === '1') process.stderr.write(chunk);
});

try {
	await waitForJson(`http://127.0.0.1:${debugPort}/json/version`);
	const target = await openPage(debugPort, `http://127.0.0.1:${port}/`);
	const results = await runCdp(target.webSocketDebuggerUrl);
	const failures = results.filter((result) => !result.ok);
	for (const result of results) {
		console.log(`${result.ok ? 'ok' : 'not ok'} - ${result.name}`);
	}
	if (failures.length > 0) {
		throw new Error(failures.map((failure) => `${failure.name}: ${failure.message}`).join('\n\n'));
	}
} finally {
	browser.kill('SIGTERM');
	await new Promise((resolveExit) => {
		browser.once('exit', resolveExit);
		setTimeout(resolveExit, 2_000);
	});
	server.close();
	await rm(userDataDir, { recursive: true, force: true });
}
