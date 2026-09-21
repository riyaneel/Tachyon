import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { randomUUID } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { spawn, type ChildProcess } from 'node:child_process';
import { describe, it } from 'mocha';

const __dirname = fileURLToPath(new URL('.', import.meta.url));
const WORKER = join(__dirname, 'star_worker.ts');
const TSX_BIN = join(__dirname, '..', 'node_modules', '.bin', 'tsx');

function sockBase(): string {
	return join(tmpdir(), `tstr-${randomUUID()}`);
}

type WorkerSpec = {
	role: 'hub' | 'spoke';
	spokeIdx?: number;
};

function spawnWorker(testCase: string, base: string, spec: WorkerSpec): Promise<void> {
	return new Promise((resolve, reject) => {
		const env: NodeJS.ProcessEnv = {
			...process.env,
			TACHYON_STAR_ROLE: spec.role,
			TACHYON_STAR_TEST_CASE: testCase,
			TACHYON_STAR_SOCK_BASE: base,
			TACHYON_STAR_SPOKE_IDX: String(spec.spokeIdx ?? 0),
		};
		const proc: ChildProcess = spawn(TSX_BIN, [WORKER], { env });
		proc.stderr?.on('data', (d: Buffer) => process.stderr.write(d));
		proc.on('exit', (code) =>
			code === 0
				? resolve()
				: reject(new Error(`[${spec.role}/spoke${spec.spokeIdx ?? ''}] exited with code ${code ?? 'null'}`)),
		);
		proc.on('error', reject);
	});
}

function runStar(testCase: string, nSpokes = 1): Promise<void> {
	const base = sockBase();
	const spokes = Array.from({ length: nSpokes }, (_, i) =>
		spawnWorker(testCase, base, { role: 'spoke', spokeIdx: i }),
	);
	const hub = spawnWorker(testCase, base, { role: 'hub' });
	return Promise.all([hub, ...spokes]).then(() => undefined);
}

describe('Tachyon StarBus - Integration Suite', function () {
	this.timeout(10_000);

	it('nSpokes reflects bus count', () => runStar('n_spokes', 2));

	it('poll drains a single message from a single spoke', () => runStar('poll_single', 1));

	it('poll drains one message per spoke, attributes correct spokeIdx and typeId', () => runStar('poll_multi', 2));

	it('poll returns empty guard when budget expires with no data', () => runStar('poll_empty', 1));

	it('commit releases ring-buffer slots; second poll is empty', () => runStar('commit_releases', 1));

	it('StarPollGuard.commit() is idempotent', () => runStar('guard_idempotent', 1));

	it('StarMsgView.data is detached (TypeError) after commit', () => runStar('data_invalidated_after_commit', 1));

	it('acquireTx rollback, explicit rollback leaves ring usable', () => runStar('tx_rollback', 1));

	it('acquireTx rollback, auto-rollback via using/Symbol.dispose', () => runStar('tx_rollback_via_using', 1));

	it('acquireTx returns null for out-of-range spokeIdx', () => runStar('tx_oob', 1));

	it('getState does not return FATAL_ERROR on a fresh spoke', () => runStar('get_state', 1));

	it('close() is idempotent', () => runStar('close_idempotent', 1));

	it('create() throws when buses array is empty', () => runStar('create_rejects', 0));

	it('flush() does not throw on a live spoke', () => runStar('flush', 1));

	it('multi-message single spoke, 5 messages arrive in send order', () => runStar('multi_message', 1));
});
