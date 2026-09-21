import { Bus } from '../dist/index.js';

const SOCKET_PATH = '/tmp/tachyon_node_demo.sock';
const ITERATIONS = 100_000;

function sendBackpressured(bus, payload, typeId) {
	for (;;) {
		try {
			bus.send(payload, typeId);
			return;
		} catch (err) {
			if (err.code !== 'ERR_TACHYON_FULL') throw err;
			// Ring is full — yield briefly so the consumer can drain.
			Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 1);
		}
	}
}

const TYPE_TICK = 1;
const TYPE_SENTINEL = 0xffff;

function connectWithRetry(path, maxRetries = 100, delayMs = 20) {
	for (let i = 0; i < maxRetries; i++) {
		try {
			return Bus.connect(path);
		} catch (err) {
			if (i === maxRetries - 1) throw err;
			Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, delayMs);
		}
	}
	throw new Error('unreachable');
}

console.log(`[producer] connecting to ${SOCKET_PATH}`);
const bus = connectWithRetry(SOCKET_PATH);
console.log(`[producer] connected; sending ${ITERATIONS.toLocaleString()} messages`);

try {
	const start = process.hrtime.bigint();
	for (let i = 0; i < ITERATIONS; i++) {
		sendBackpressured(bus, Buffer.from(`tick ${i}`), TYPE_TICK);
	}
	sendBackpressured(bus, Buffer.from(''), TYPE_SENTINEL);
	const ms = Number(process.hrtime.bigint() - start) / 1e6;
	const rate = Math.round((ITERATIONS / ms) * 1000);
	console.log(`[producer] done in ${ms.toFixed(2)} ms (${rate.toLocaleString()} msg/s)`);
} finally {
	bus.close();
}
