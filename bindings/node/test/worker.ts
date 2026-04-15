import assert from 'node:assert/strict';
import { Bus, isAbiMismatch } from '../src/ts/index.ts';

const role = process.env['TACHYON_ROLE'] as 'prod' | 'cons';
const testCase = process.env['TACHYON_TEST_CASE']!;
const socketPath = process.env['TACHYON_SOCKET_PATH']!;

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
		const bus = connectProducer(socketPath);
		switch (testCase) {
			case 'lifecycle':
				break;

			case 'send_recv':
			case 'rx_guard':
				bus.send(Buffer.from('hello'), 42);
				break;

			case 'tx_guard':
				{
					const tx1 = bus.acquireTx(128);
					tx1.bytes().write('tx-payload', 0);
					tx1.commit(10, 7);
				}
				{
					const tx2 = bus.acquireTx(128);
					tx2.bytes().write('should-be-rolled-back', 0);
					tx2[Symbol.dispose]();
				}
				{
					const tx3 = bus.acquireTx(128);
					tx3.bytes().write('final', 0);
					tx3.commit(5, 8);
				}
				break;

			case 'batch':
				for (let i = 0; i < 3; i++) {
					const tx = bus.acquireTx(32);
					tx.bytes().write(`msg-${i}`, 0);
					tx.commitUnflushed(5, 100 + i);
				}
				bus.flush();
				break;
		}
		bus.close();
	} else {
		const bus = Bus.listen(socketPath, 1024);
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
				const rx = bus.acquireRx();
				if (!rx) throw new Error('Rx acquisition failed');
				assert.strictEqual(rx.typeId, 42);
				rx[Symbol.dispose]();
				assert.throws(() => rx.data(), /already been committed/);
				break;
			}

			case 'batch': {
				Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 100);
				const batch = bus.drainBatch(10);
				assert.strictEqual(batch.length, 3);
				let i = 0;
				for (const msg of batch) {
					assert.strictEqual(msg.typeId, 100 + i);
					i++;
				}
				batch.commit();
				assert.throws(() => batch.at(0), /already been committed/);
				break;
			}
		}
		bus.close();
	}
	process.exit(0);
} catch (err) {
	console.error(`Worker [${role}/${testCase}] Failure:`, err);
	process.exit(1);
}
