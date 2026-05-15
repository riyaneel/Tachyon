import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { randomUUID } from 'node:crypto';
import { fileURLToPath } from 'node:url';

import { describe, it } from 'mocha';

import { Bus, isAbiMismatch, AbiMismatchError, PeerDeadError } from '../src/ts/index.ts';

const __dirname = fileURLToPath(new URL('.', import.meta.url));
const WORKER_PATH = join(__dirname, 'worker.ts');
const TSX_BIN = join(__dirname, '..', 'node_modules', '.bin', 'tsx');

async function runSpsc(testCase: string): Promise<void> {
	const socketPath = join(tmpdir(), `tachyon-${randomUUID()}.sock`);

	const spawnWorker = (role: 'prod' | 'cons'): Promise<void> =>
		new Promise((resolve, reject) => {
			const proc = spawn(TSX_BIN, [WORKER_PATH], {
				env: {
					...process.env,
					TACHYON_ROLE: role,
					TACHYON_TEST_CASE: testCase,
					TACHYON_SOCKET_PATH: socketPath,
				},
			});
			proc.stderr.on('data', (d: Buffer) => process.stderr.write(d));
			proc.on('exit', (code) => {
				if (code === 0) resolve();
				else reject(new Error(`${role} exited with code ${code ?? 'null'}`));
			});
			proc.on('error', reject);
		});

	await Promise.all([spawnWorker('cons'), spawnWorker('prod')]);
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
