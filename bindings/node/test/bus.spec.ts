import assert from 'node:assert/strict';
import { Worker, isMainThread, workerData } from 'node:worker_threads';
import { createRequire } from 'node:module';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { randomUUID } from 'node:crypto';

import { describe, it } from 'mocha';

import { Bus, isAbiMismatch, AbiMismatchError, PeerDeadError } from '../src/ts/index.ts';

const _require = createRequire(import.meta.url);
const TSX_ESM = _require.resolve('tsx/esm');

if (!isMainThread) {
	const { role, testCase, socketPath } = workerData as {
		role: 'prod' | 'cons';
		testCase: string;
		socketPath: string;
	};

	const connectProducer = (path: string): Bus => {
		let retries = 50;
		while (retries-- > 0) {
			try {
				return Bus.connect(path);
			} catch (err) {
				if (isAbiMismatch(err) || retries === 0) throw err;
				Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 20);
			}
		}

		throw new Error('Producer timed out waiting for socket.');
	};

	try {
		if (role === 'prod') {
			using bus = connectProducer(socketPath);
			switch (testCase) {
				case 'lifecycle':
					break;

				case 'send_recv':
				case 'rx_guard':
					bus.send(Buffer.from('hello'), 42);
					break;

				case 'tx_guard':
					{
						using tx = bus.acquireTx(128);
						tx.bytes().write('tx-payload', 0);
						tx.commit(10, 7);
					}
					{
						using tx = bus.acquireTx(128);
						tx.bytes().write('should-be-rolled-back', 0);
					}
					{
						using tx = bus.acquireTx(128);
						tx.bytes().write('final', 0);
						tx.commit(5, 8);
					}
					break;

				case 'batch':
					for (let i = 0; i < 3; i++) {
						using tx = bus.acquireTx(32);
						tx.bytes().write(`msg-${i}`, 0);
						tx.commitUnflushed(5, 100 + i);
					}
					bus.flush();
					break;
			}
		} else {
			using bus = Bus.listen(socketPath, 1024);
			switch (testCase) {
				case 'lifecycle':
					bus.close();
					bus.close();
					break;

				case 'send_recv': {
					const msg = bus.recv();
					assert.strictEqual(msg.typeId, 42);
					assert.strictEqual(msg.data.subarray(0, 5).toString(), 'hello');
					break;
				}

				case 'tx_guard': {
					const msg1 = bus.recv();
					assert.strictEqual(msg1.typeId, 7);
					assert.strictEqual(msg1.data.subarray(0, 10).toString(), 'tx-payload');

					const msg2 = bus.recv();
					assert.strictEqual(msg2.typeId, 8);
					assert.strictEqual(msg2.data.subarray(0, 5).toString(), 'final');
					break;
				}

				case 'rx_guard': {
					let guardRef: any;
					{
						using rx = bus.acquireRx();
						if (!rx) throw new Error('Rx acquisition failed');
						assert.strictEqual(rx.typeId, 42);
						guardRef = rx;
					}
					assert.throws(() => guardRef.data(), /already been committed/);
					break;
				}

				case 'batch': {
					Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 100);

					using batch = bus.drainBatch(10);
					assert.strictEqual(batch.length, 3);

					let i = 0;
					for (const msg of batch) {
						assert.strictEqual(msg.typeId, 100 + i);
						i++;
					}

					const firstMsg = batch.at(0);
					batch.commit();
					assert.throws(() => batch.at(0), /already been committed/);
					break;
				}
			}
		}
		process.exit(0);
	} catch (err) {
		console.error(`Worker [${role}/${testCase}] Failure:`, err);
		process.exit(1);
	}
}

async function runSpsc(testCase: string) {
	const socketPath = join(tmpdir(), `tachyon-${randomUUID()}.sock`);

	const startWorker = (role: 'prod' | 'cons') =>
		new Worker(new URL(import.meta.url), {
			workerData: { role, testCase, socketPath },
			execArgv: ['--import', TSX_ESM],
		});

	const consumer = startWorker('cons');
	const producer = startWorker('prod');

	const toPromise = (w: Worker, name: string) =>
		new Promise<void>((resolve, reject) => {
			w.on('exit', (code) => {
				if (code === 0) resolve();
				else reject(new Error(`${name} failed with code ${code}`));
			});
			w.on('error', reject);
		});

	try {
		await Promise.all([toPromise(consumer, 'Consumer'), toPromise(producer, 'Producer')]);
	} finally {
		await Promise.allSettled([consumer.terminate(), producer.terminate()]);
	}
}

describe('Tachyon Bus - SPSC Integration Suite', function () {
	this.timeout(10000);

	it('should verify lifecycle: listen/connect and idempotent close', () => runSpsc('lifecycle'));

	it('should complete basic send/recv via blocking I/O', () => runSpsc('send_recv'));

	it('should enforce TxGuard lifecycle (commit, auto-rollback, buffer detachment)', () => runSpsc('tx_guard'));

	it('should enforce RxGuard lifecycle (auto-commit, buffer detachment)', () => runSpsc('rx_guard'));

	it('should support batch draining with iterator and indexed access', () => runSpsc('batch'));

	it('should correctly identify internal errors via type guards', () => {
		const abiErr = new AbiMismatchError();
		assert.ok(isAbiMismatch(abiErr));

		const deadErr = new PeerDeadError();
		assert.ok(deadErr instanceof PeerDeadError);
	});
});
