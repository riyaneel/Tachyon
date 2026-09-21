import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { mkdtemp, rm } from 'node:fs/promises';
import { existsSync, readdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, extname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const DEMO = process.env.TACHYON_BROWSER_DEMO === '1';
const PACKAGE_ROOT = DEMO
	? resolve(__dirname, '../../../examples/browser_wasm/dist')
	: resolve(process.env.TACHYON_PACKAGE_ROOT ?? resolve(__dirname, '..'));
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
import { Bus, makeTypeId, msgType, routeId, createBrowserBindings } from "@tachyon-ipc/core";
import createTachyonCore from "/dist/wasm/tachyon.js";
const raw = await createTachyonCore();

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
  assert.equal(cached.byteLength, 4);
  assert.throws(() => batch.at(0), /already been committed/);

  producer.close();
  consumer.close();
});

record("drainBatch drains more than one internal batch window", () => {
  const bus = Bus.listen("/test/batch-window", 1 << 16);
  const COUNT = 100;
  for (let i = 0; i < COUNT; i += 1) {
    const tx = bus.acquireTx(4);
    new DataView(tx.bytes().buffer, tx.bytes().byteOffset, 4).setUint32(0, i, true);
    tx.commitUnflushed(4, i);
  }
  bus.flush();

  const batch = bus.drainBatch(COUNT);
  assert.equal(batch.length, COUNT);
  for (let i = 0; i < COUNT; i += 1) {
    assert.equal(batch.at(i).typeId, i);
    assert.equal(new DataView(batch.at(i).data.buffer, batch.at(i).data.byteOffset, 4).getUint32(0, true), i);
  }
  batch.commit();

  const drained = bus.drainBatch(COUNT);
  assert.equal(drained.length, 0);
  drained.commit();
  bus.close();
});

record("empty receive and wasm32 argument validation", () => {
  const bus = Bus.listen("/browser/validation", 256);
  assert.equal(bus.recv(), null);
  for (const value of [-1, 1.5, NaN, Infinity, 2 ** 32, 2 ** 53]) {
    assert.throws(() => bus.acquireTx(value), /integer/);
    assert.throws(() => bus.drainBatch(value), /integer/);
    assert.throws(() => Bus.listen("/browser/invalid", value), /capacity|limit/);
  }
  assert.throws(() => Bus.listen("/browser/invalid", 2 ** 31), /limit/);
  const tx = bus.acquireTx(4);
  assert.throws(() => tx.commit(5, 1), /actualSize/);
  assert.throws(() => tx.commit(4, -1), /typeId/);
  tx.bytes().set([1, 2, 3, 4]);
  tx.commit(4, 0xffffffff);
  assert.equal(bus.recv().typeId, 0xffffffff);
  for (let i = 0; i < 100; i++) {
    bus.send(new Uint8Array([i]), i);
    assert.equal(bus.recv().data[0], i);
  }
  bus.close();
});

record("a batch that crosses the ring wrap keeps its cursor consistent", () => {
  const CAPACITY = 4096;
  const bus = Bus.listen("/test/batch-wrap", CAPACITY);

  const filler = bus.acquireTx(CAPACITY - 160);
  filler.commit(CAPACITY - 160, 1);
  const first = bus.drainBatch(4);
  assert.equal(first.length, 1);
  first.commit();

  for (const typeId of [2, 3]) {
    const tx = bus.acquireTx(64);
    tx.bytes()[0] = typeId;
    tx.commitUnflushed(64, typeId);
  }
  bus.flush();

  const wrapped = bus.drainBatch(4);
  assert.equal(wrapped.length, 2);
  assert.equal(wrapped.at(0).typeId, 2);
  assert.equal(wrapped.at(1).typeId, 3);
  assert.equal(wrapped.at(0).data[0], 2);
  assert.equal(wrapped.at(1).data[0], 3);
  wrapped.commit();

  const drained = bus.drainBatch(4);
  assert.equal(drained.length, 0);
  drained.commit();

  const after = bus.acquireTx(64);
  after.commit(64, 4);
  const tail = bus.drainBatch(4);
  assert.equal(tail.length, 1);
  assert.equal(tail.at(0).typeId, 4);
  tail.commit();
  bus.close();
});

record("guards are exclusive across peers and close releases reservations", () => {
  const bus = Bus.listen("/browser/exclusive", 1024);
  const peer = Bus.connect("/browser/exclusive");
  const tx = peer.acquireTx(4);
  assert.throws(() => bus.acquireTx(4), /already active/);
  peer.close();
  assert.throws(() => tx.bytes(), /closed/);
  assert.throws(() => tx.commit(4, 1), /active|closed/);
  bus.send(new Uint8Array([7]), 1);
  const rx = bus.acquireRx();
  assert.throws(() => bus.recv(), /already active/);
  rx.commit();
  rx[Symbol.dispose]();
  bus.close();
});

record("overlapping batches cannot invalidate each other's buffers", () => {
  const bus = Bus.listen("/browser/batch-lifetime", 1024);
  bus.send(new Uint8Array([1]), 1);
  const first = bus.drainBatch(1);
  const saved = first.at(0).data;
  assert.throws(() => bus.drainBatch(1), /already active/);
  first.commit();
  assert.equal(saved.byteLength, 1);
  assert.throws(() => first.at(0), /already been committed/);
  bus.send(new Uint8Array([2]), 2);
  const second = bus.drainBatch(1);
  assert.equal(second.at(0).data[0], 2);
  second.commit();
  assert.throws(() => second.at(0), /already been committed/);
  bus.close();
});

record("empty batches retain ownership until committed", () => {
  const bus = Bus.listen("/browser/empty-batch-lifetime", 1024);
  const empty = bus.drainBatch(1);
  assert.equal(empty.length, 0);
  bus.send(new Uint8Array([42]), 7);
  assert.throws(() => bus.drainBatch(1), /already active/);
  empty.commit();

  const live = bus.drainBatch(1);
  const saved = live.at(0).data;
  empty.commit();
  assert.equal(saved.byteLength, 1);
  assert.equal(saved[0], 42);
  live.commit();
  assert.equal(saved.byteLength, 1);
  assert.throws(() => live.at(0), /already been committed/);

  const zeroLimit = bus.drainBatch(0);
  assert.throws(() => bus.drainBatch(1), /already active/);
  zeroLimit.commit();
  bus.drainBatch(1).commit();
  bus.close();
});

record("ring counters survive more than 4 GiB of cumulative wasm32 traffic", () => {
  const bus = Bus.listen("/browser/wrap", 1 << 20);
  for (let i = 0; i < 32770; i++) {
    const tx = bus.acquireTx(131008);
    tx.bytes()[0] = i & 255;
    tx.commit(1, i);
    const rx = bus.acquireRx();
    assert.notEqual(rx, null);
    assert.equal(rx.typeId, i);
    assert.equal(rx.data()[0], i & 255);
    rx.commit();
  }
  bus.close();
});

record("custom modules share the wrapper and raw C ABI", () => {
  const custom = createBrowserBindings(raw);
  assert.equal(createBrowserBindings(raw), custom);
  const bus = custom.Bus.listen("/browser/custom", 1024);
  const cells = raw._malloc(64);
  const shm = raw.cwrap("tachyon_bus_get_shm_ptr", "number", ["number"])(bus.wasmPointer);
  assert.equal(shm % 128, 0);
  const receive = raw.cwrap("tachyon_acquire_rx", "number", ["number", "number", "number"]);
  const drain = raw.cwrap("tachyon_acquire_rx_batch", "number", ["number", "number", "number"]);
  assert.equal(receive(bus.wasmPointer, cells, cells + 4), 0);
  assert.equal(drain(bus.wasmPointer, cells, 1), 0);
  bus.send(new Uint8Array([42]), 123);
  const ptr = receive(bus.wasmPointer, cells, cells + 4);
  assert.equal(raw.HEAPU8[ptr], 42);
  assert.equal(raw.getValue(cells, "i32"), 123);
  raw.cwrap("tachyon_commit_rx", "number", ["number"])(bus.wasmPointer);
  assert.equal(bus.recv(), null);
  bus.close();
  assert.throws(() => bus.wasmPointer, /closed/);
  raw._free(cells);
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
			if (!DEMO && (url.pathname === '/' || url.pathname === '/index.html')) {
				res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
				res.end(TEST_PAGE);
				return;
			}

			const filePath = resolve(PACKAGE_ROOT, `.${url.pathname === '/' ? '/index.html' : url.pathname}`);
			if (!filePath.startsWith(PACKAGE_ROOT + '/')) {
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

async function waitForJson(url, timeoutMs = 30_000) {
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

	// The page opened via /json/new is still navigating when we attach, so the
	// initial about:blank execution context is torn down underneath us. Retry
	// until the document's real context is live before running the suite.
	const evaluateResilient = async (expression, timeout) => {
		const deadline = Date.now() + 15_000;
		for (;;) {
			try {
				return await evaluate(expression, timeout);
			} catch (error) {
				const message = String(error?.message ?? error);
				const transient =
					message.includes('Execution context was destroyed') ||
					message.includes('Cannot find context') ||
					message.includes('uniqueContextId');
				if (!transient || Date.now() > deadline) throw error;
				await new Promise((resolveRetry) => setTimeout(resolveRetry, 50));
			}
		}
	};

	await call('Runtime.enable');
	await evaluateResilient('document.readyState', 5_000);
	if (DEMO) {
		await evaluateResilient(`new Promise((resolve, reject) => {
			const started = performance.now();
			const check = () => {
				if (document.querySelector('#wasm-status')?.textContent === 'ready') resolve(true);
				else if (performance.now() - started > 10000) reject(new Error(document.body.innerText));
				else setTimeout(check, 25);
			}; check();
		})`);
		await evaluateResilient(`(() => {
			document.querySelector('#value').value = '41';
			document.querySelector('#send').click();
			if (document.querySelector('#last-reply').textContent !== '42') throw new Error('C++ echo failed');
			document.querySelector('#bench').click();
		})()`);
		await evaluateResilient(`new Promise((resolve, reject) => {
			const started = performance.now();
			const check = () => {
				const log = document.querySelector('#log').textContent;
				if (log.includes('bench failed')) reject(new Error(log));
				else if (log.includes('bench completed')) resolve(true);
				else if (performance.now() - started > 10000) reject(new Error('Demo benchmark timed out'));
				else setTimeout(check, 25);
			}; check();
		})`);
		ws.close();
		return [{ name: 'built demo wrapper echo and million-RTT C ABI benchmark', ok: true }];
	}
	await evaluateResilient(`new Promise((resolve, reject) => {
  const started = performance.now();
  const tick = () => {
    if (window.__tachyonBrowserDone) resolve(true);
    else if (performance.now() - started > 10000) reject(new Error("browser wasm tests timed out"));
    else setTimeout(tick, 25);
  };
  tick();
})`);
	const results = JSON.parse(await evaluateResilient('JSON.stringify(window.__tachyonBrowserResults)'));
	ws.close();
	return results;
}

const { server, port } = await startServer();
const debugPort = 9333 + Math.floor(Math.random() * 1000);
const userDataDir = await mkdtemp(join(tmpdir(), 'tachyon-browser-wasm-'));
const browser = spawn(chromiumPath(), [
	'--headless=new',
	'--disable-gpu',
	// CI runners give containers a tiny /dev/shm; without this Chromium crashes
	// on startup and never opens the remote-debugging port.
	'--disable-dev-shm-usage',
	'--no-first-run',
	'--no-default-browser-check',
	'--no-sandbox',
	`--remote-debugging-port=${debugPort}`,
	`--user-data-dir=${userDataDir}`,
	`http://127.0.0.1:${port}/`,
]);

let browserStderr = '';
browser.stderr.on('data', (chunk) => {
	browserStderr += chunk;
	if (process.env.TACHYON_BROWSER_TEST_DEBUG === '1') process.stderr.write(chunk);
});
browser.on('error', (err) => {
	console.error(`Failed to spawn Chromium: ${err.message}`);
});

try {
	try {
		await waitForJson(`http://127.0.0.1:${debugPort}/json/version`);
	} catch (error) {
		if (browserStderr.trim() !== '') console.error(`Chromium stderr:\n${browserStderr}`);
		throw error;
	}
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
	// Wait for the browser to actually exit before removing its user-data-dir,
	// otherwise Chromium is still writing into it and rmdir races with ENOTEMPTY.
	const exited = new Promise((resolveExit) => browser.once('exit', resolveExit));
	browser.kill('SIGTERM');
	const killed = await Promise.race([exited, new Promise((r) => setTimeout(() => r('timeout'), 3_000))]);
	if (killed === 'timeout') {
		browser.kill('SIGKILL');
		await Promise.race([exited, new Promise((r) => setTimeout(r, 2_000))]);
	}
	server.close();
	// Temp-dir cleanup is best-effort; a lingering Chromium file must not fail the run.
	try {
		await rm(userDataDir, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 });
	} catch {
		// ignore: the OS reaps the temp dir eventually
	}
}
