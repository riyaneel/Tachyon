import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { randomUUID } from 'node:crypto';
import { fileURLToPath } from 'node:url';

import { describe, it } from 'mocha';

import { AbiMismatchError, PeerDeadError } from '../src/ts/index.ts';

const __dirname = fileURLToPath(new URL('.', import.meta.url));
const WORKER_PATH = join(__dirname, 'rpc_worker.ts');
const TSX_BIN = join(__dirname, '..', 'node_modules', '.bin', 'tsx');

async function runRpc(testCase: string): Promise<void> {
	const socketPath = join(tmpdir(), `tachyon-rpc-${randomUUID()}.sock`);

	const spawnWorker = (role: 'caller' | 'callee'): Promise<void> =>
		new Promise((resolve, reject) => {
			const proc = spawn(TSX_BIN, [WORKER_PATH], {
				env: {
					...process.env,
					TACHYON_RPC_ROLE: role,
					TACHYON_RPC_TEST_CASE: testCase,
					TACHYON_RPC_SOCKET_PATH: socketPath,
				},
			});
			proc.stderr.on('data', (d: Buffer) => process.stderr.write(d));
			proc.on('exit', (code) => {
				if (code === 0) resolve();
				else reject(new Error(`${role} exited with code ${code ?? 'null'}`));
			});
			proc.on('error', reject);
		});

	await Promise.all([spawnWorker('callee'), spawnWorker('caller')]);
}

describe('Tachyon RpcBus - Integration Suite', function () {
	this.timeout(10_000);

	it('should complete call/wait roundtrip with echoed payload', () => runRpc('roundtrip'));

	it('should preserve msgType across call and serve', () => runRpc('msg_type'));

	it('should assign monotonically increasing correlation IDs', () => runRpc('cid_monotonic'));

	it('should match n in-flight responses to their requests', () => runRpc('n_inflight'));

	it('should commit RpcRxGuard idempotently', () => runRpc('guard_idempotent'));

	it('should close the bus idempotently', () => runRpc('close_idempotent'));

	it('should classify AbiMismatchError and PeerDeadError correctly', () => {
		const abi = new AbiMismatchError();
		assert.ok(abi instanceof AbiMismatchError);
		assert.strictEqual(abi.code, 'ERR_TACHYON_ABI_MISMATCH');

		const dead = new PeerDeadError();
		assert.ok(dead instanceof PeerDeadError);
	});
});
