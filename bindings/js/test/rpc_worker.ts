import assert from 'node:assert/strict';
import { RpcBus, isAbiMismatch } from '../src/ts/index.ts';

const role = process.env['TACHYON_RPC_ROLE'] as 'caller' | 'callee';
const testCase = process.env['TACHYON_RPC_TEST_CASE']!;
const socketPath = process.env['TACHYON_RPC_SOCKET_PATH']!;

const CAP = 1 << 16;
const CAP_LARGE = 1 << 18;

const connectCaller = (path: string): RpcBus => {
	let retries = 50;
	while (retries-- > 0) {
		try {
			return RpcBus.connect(path);
		} catch (err) {
			if (isAbiMismatch(err) || retries === 0) throw err;
			Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 20);
		}
	}
	throw new Error('Caller timed out waiting for socket.');
};

try {
	if (role === 'callee') {
		const bus = RpcBus.listen(socketPath, CAP, CAP);

		switch (testCase) {
			case 'roundtrip': {
				const req = bus.serve();
				if (!req) throw new Error('serve() returned null');
				const echo = Buffer.from(req.data());
				const cid = req.correlationId;
				req.commit();
				bus.reply(cid, echo, 2);
				break;
			}

			case 'msg_type': {
				const req = bus.serve();
				if (!req) throw new Error('serve() returned null');
				const cid = req.correlationId;
				const receivedMsgType = req.msgType;
				req.commit();
				assert.strictEqual(receivedMsgType, 42);
				bus.reply(cid, Buffer.from('ok'), 99);
				break;
			}

			case 'cid_monotonic': {
				for (let i = 0; i < 4; i++) {
					const req = bus.serve();
					if (!req) throw new Error('serve() returned null');
					const cid = req.correlationId;
					req.commit();
					bus.reply(cid, Buffer.from('ok'), 0);
				}
				break;
			}

			case 'n_inflight': {
				for (let i = 0; i < 8; i++) {
					const req = bus.serve();
					if (!req) throw new Error('serve() returned null');
					const cid = req.correlationId;
					const data = Buffer.from(req.data());
					req.commit();
					bus.reply(cid, data, 0);
				}
				break;
			}

			case 'guard_idempotent': {
				const req = bus.serve();
				if (!req) throw new Error('serve() returned null');
				const cid = req.correlationId;
				req.commit();
				bus.reply(cid, Buffer.from('ok'), 0);
				break;
			}

			case 'close_idempotent':
				bus.close();
				bus.close();
				break;
		}

		bus.close();
	} else {
		switch (testCase) {
			case 'roundtrip': {
				const bus = connectCaller(socketPath);
				const payload = Buffer.from('hello_rpc_node');
				const cid = bus.call(payload, 1);
				for (;;) {
					const resp = bus.wait(cid);
					if (resp === null) continue;
					assert.strictEqual(resp.msgType, 2);
					assert.deepStrictEqual(resp.data(), payload);
					resp.commit();
					break;
				}
				bus.close();
				break;
			}

			case 'msg_type': {
				const bus = connectCaller(socketPath);
				const cid = bus.call(Buffer.from('x'), 42);
				for (;;) {
					const resp = bus.wait(cid);
					if (resp === null) continue;
					assert.strictEqual(resp.msgType, 99);
					resp.commit();
					break;
				}
				bus.close();
				break;
			}

			case 'cid_monotonic': {
				const bus = connectCaller(socketPath);
				const cids: bigint[] = [];
				for (let i = 0; i < 4; i++) {
					const cid = bus.call(Buffer.from('x'), 0);
					cids.push(cid);
					for (;;) {
						const resp = bus.wait(cid);
						if (resp === null) continue;
						resp.commit();
						break;
					}
				}
				for (let i = 0; i < cids.length - 1; i++) {
					assert.strictEqual(cids[i + 1]! - cids[i]!, 1n);
				}
				assert.ok(cids.every((c) => c > 0n));
				bus.close();
				break;
			}

			case 'n_inflight': {
				Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 50);
				const bus = connectCaller(socketPath);
				const sent = Array.from({ length: 8 }, (_, i) => i * 100);
				const cids = sent.map((v) => {
					const buf = Buffer.alloc(4);
					buf.writeInt32LE(v, 0);
					return bus.call(buf, 0);
				});
				for (let i = 0; i < cids.length; i++) {
					for (;;) {
						const resp = bus.wait(cids[i]!);
						if (resp === null) continue;
						assert.strictEqual(resp.data().readInt32LE(0), sent[i]);
						resp.commit();
						break;
					}
				}
				bus.close();
				break;
			}

			case 'guard_idempotent': {
				const bus = connectCaller(socketPath);
				const cid = bus.call(Buffer.from('x'), 0);
				for (;;) {
					const resp = bus.wait(cid);
					if (resp === null) continue;
					resp.commit();
					resp.commit();
					assert.throws(() => resp.data(), /already been committed/);
					break;
				}
				bus.close();
				break;
			}

			case 'close_idempotent': {
				Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 50);
				const bus = connectCaller(socketPath);
				bus.close();
				bus.close();
				break;
			}
		}
	}

	process.exit(0);
} catch (err) {
	console.error(`RpcWorker [${role}/${testCase}] Failure:`, err);
	process.exit(1);
}
