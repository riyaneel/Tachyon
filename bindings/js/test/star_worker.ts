import assert from 'node:assert/strict';
import { Bus, StarBus } from '../src/ts/index.ts';

const role = process.env['TACHYON_STAR_ROLE'] as 'hub' | 'spoke';
const testCase = process.env['TACHYON_STAR_TEST_CASE']!;
const sockBase = process.env['TACHYON_STAR_SOCK_BASE']!;
const spokeIdx = parseInt(process.env['TACHYON_STAR_SPOKE_IDX'] ?? '0', 10);

const CAP = 1 << 16;

const connectWithRetry = (path: string): Bus => {
	let retries = 50;
	while (retries-- > 0) {
		try {
			return Bus.connect(path);
		} catch {
			if (retries === 0) throw new Error(`Timed out connecting to ${path}`);
			Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 20);
		}
	}

	throw new Error('unreachable');
};

try {
	if (role === 'spoke') {
		const path = `${sockBase}_${spokeIdx}.sock`;
		const bus = Bus.listen(path, CAP);

		switch (testCase) {
			case 'n_spokes':
			case 'poll_empty': {
				Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 3_000);
				break;
			}

			case 'poll_single': {
				assert.strictEqual(spokeIdx, 0);
				bus.send(Buffer.from('hello_star_node'), 42);
				break;
			}

			case 'poll_multi': {
				bus.send(Buffer.allocUnsafe(8).fill(spokeIdx), spokeIdx + 10);
				break;
			}

			case 'commit_releases': {
				assert.strictEqual(spokeIdx, 0);
				bus.send(Buffer.from([0x01, 0x02, 0x03, 0x04]), 7);
				break;
			}

			case 'data_invalidated_after_commit': {
				assert.strictEqual(spokeIdx, 0);
				bus.send(Buffer.from([0xff]), 1);
				break;
			}

			case 'guard_idempotent': {
				assert.strictEqual(spokeIdx, 0);
				bus.send(Buffer.from([0xff]), 1);
				break;
			}

			case 'multi_message': {
				assert.strictEqual(spokeIdx, 0);
				for (let i = 0; i < 5; i++) {
					const buf = Buffer.alloc(4);
					buf.writeUInt32BE(i, 0);
					bus.send(buf, 100 + i);
				}
				break;
			}

			case 'tx_rollback':
			case 'tx_oob':
			case 'get_state':
			case 'close_idempotent':
			case 'create_rejects': {
				Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 3_000);
				break;
			}
		}

		bus.close();
	} else {
		switch (testCase) {
			case 'n_spokes': {
				const c0 = connectWithRetry(`${sockBase}_0.sock`);
				const c1 = connectWithRetry(`${sockBase}_1.sock`);
				using star = StarBus.create([c0, c1]);
				assert.strictEqual(star.nSpokes, 2);
				c0.close();
				c1.close();
				break;
			}

			case 'poll_single': {
				const c0 = connectWithRetry(`${sockBase}_0.sock`);
				using star = StarBus.create([c0]);
				using guard = star.poll(8, 5_000);
				assert.strictEqual(guard.count(), 1);
				const msg = guard.messages[0]!;
				assert.strictEqual(msg.typeId, 42);
				assert.strictEqual(msg.actualSize, 'hello_star_node'.length);
				assert.strictEqual(msg.spokeIdx, 0);
				assert.deepStrictEqual(msg.data, Buffer.from('hello_star_node'));
				c0.close();
				break;
			}

			case 'poll_multi': {
				const c0 = connectWithRetry(`${sockBase}_0.sock`);
				const c1 = connectWithRetry(`${sockBase}_1.sock`);
				using star = StarBus.create([c0, c1]);
				using guard = star.poll(16, 5_000);
				assert.strictEqual(guard.count(), 2);
				const bySpoke = new Map(guard.messages.map((m) => [m.spokeIdx, m]));
				assert.ok(bySpoke.has(0));
				assert.ok(bySpoke.has(1));
				assert.strictEqual(bySpoke.get(0)!.typeId, 10);
				assert.strictEqual(bySpoke.get(1)!.typeId, 11);
				c0.close();
				c1.close();
				break;
			}

			case 'poll_empty': {
				const c0 = connectWithRetry(`${sockBase}_0.sock`);
				using star = StarBus.create([c0]);
				using guard = star.poll(8, 200);
				assert.ok(guard.isEmpty());
				c0.close();
				break;
			}

			case 'commit_releases': {
				const c0 = connectWithRetry(`${sockBase}_0.sock`);
				using star = StarBus.create([c0]);
				const first = star.poll(8, 5_000);
				assert.strictEqual(first.count(), 1);
				first.commit();
				using second = star.poll(8, 200);
				assert.ok(second.isEmpty());
				c0.close();
				break;
			}

			case 'guard_idempotent': {
				const c0 = connectWithRetry(`${sockBase}_0.sock`);
				using star = StarBus.create([c0]);
				const guard = star.poll(8, 5_000);
				assert.strictEqual(guard.count(), 1);
				guard.commit();
				guard.commit();
				c0.close();
				break;
			}

			case 'data_invalidated_after_commit': {
				const c0 = connectWithRetry(`${sockBase}_0.sock`);
				using star = StarBus.create([c0]);
				const guard = star.poll(8, 5_000);
				assert.strictEqual(guard.count(), 1);
				const { data } = guard.messages[0]!;
				guard.commit();
				assert.throws(() => data.readUInt8(0), { code: 'ERR_BUFFER_OUT_OF_BOUNDS' });
				c0.close();
				break;
			}

			case 'tx_rollback': {
				const c0 = connectWithRetry(`${sockBase}_0.sock`);
				using star = StarBus.create([c0]);
				const tx = star.acquireTx(0, 64);
				assert.ok(tx !== null);
				assert.strictEqual(tx!.slot.byteLength, 64);
				tx!.rollback();
				using guard = star.poll(4, 200);
				assert.ok(guard.isEmpty());
				c0.close();
				break;
			}

			case 'tx_rollback_via_using': {
				const c0 = connectWithRetry(`${sockBase}_0.sock`);
				using star = StarBus.create([c0]);
				{
					using tx = star.acquireTx(0, 64)!;
				}
				using guard = star.poll(4, 200);
				assert.ok(guard.isEmpty());
				c0.close();
				break;
			}

			case 'tx_oob': {
				const c0 = connectWithRetry(`${sockBase}_0.sock`);
				using star = StarBus.create([c0]);
				assert.strictEqual(star.acquireTx(999, 64), null);
				c0.close();
				break;
			}

			case 'get_state': {
				const c0 = connectWithRetry(`${sockBase}_0.sock`);
				using star = StarBus.create([c0]);
				assert.notStrictEqual(star.getState(0), 4 /* TACHYON_STATE_FATAL_ERROR */);
				c0.close();
				break;
			}

			case 'close_idempotent': {
				const c0 = connectWithRetry(`${sockBase}_0.sock`);
				const star = StarBus.create([c0]);
				star.close();
				star.close();
				c0.close();
				break;
			}

			case 'create_rejects': {
				assert.throws(() => StarBus.create([]), /must not be empty/);
				break;
			}

			case 'multi_message': {
				const c0 = connectWithRetry(`${sockBase}_0.sock`);
				using star = StarBus.create([c0]);
				using guard = star.poll(5, 5_000);
				assert.strictEqual(guard.count(), 5);
				for (let i = 0; i < 5; i++) {
					const msg = guard.messages[i]!;
					assert.strictEqual(msg.spokeIdx, 0);
					assert.strictEqual(msg.typeId, 100 + i);
					assert.strictEqual(msg.data.readUInt32BE(0), i);
				}
				c0.close();
				break;
			}

			case 'flush': {
				const c0 = connectWithRetry(`${sockBase}_0.sock`);
				using star = StarBus.create([c0]);
				assert.doesNotThrow(() => star.flush(0));
				c0.close();
				break;
			}
		}
	}

	process.exit(0);
} catch (err) {
	console.error(`StarWorker [${role}/${testCase}/${spokeIdx}] Failure:`, err);
	process.exit(1);
}
