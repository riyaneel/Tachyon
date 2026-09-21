import assert from 'node:assert/strict';

import { RxGuard, TxGuard } from '../src/ts/guards.ts';
import type { RxController, TxController } from '../src/ts/guards.ts';

const detached = new Uint8Array(0);

function txController(): TxController & { commits: number; rollbacks: number } {
	const calls = {
		commits: 0,
		rollbacks: 0,
		commitTx(): void {
			calls.commits += 1;
		},
		commitTxUnflushed(): void {
			calls.commits += 1;
		},
		rollbackTx(): void {
			calls.rollbacks += 1;
		},
	};
	return calls;
}

function rxController(state = 2): RxController & { commits: number } {
	const calls = {
		commits: 0,
		commitRx(): void {
			calls.commits += 1;
		},
		getState(): number {
			return state;
		},
	};
	return calls;
}

describe('guards: detached slots', () => {
	it('TxGuard.bytes() refuses a detached slot', () => {
		const guard = new TxGuard(txController(), detached);
		assert.throws(() => guard.bytes(), /detached/);
	});

	it('TxGuard.bytes() returns a live slot untouched', () => {
		const buffer = new Uint8Array(8);
		const guard = new TxGuard(txController(), buffer);
		assert.equal(guard.bytes(), buffer);
	});

	it('RxGuard.data() refuses a detached slot', () => {
		const guard = new RxGuard(rxController(), detached, 7, 0);
		assert.throws(() => guard.data(), /detached/);
	});

	it('RxGuard.data() returns a live slot untouched', () => {
		const buffer = new Uint8Array(4);
		const guard = new RxGuard(rxController(), buffer, 7, 4);
		assert.equal(guard.data(), buffer);
	});

	it('a detached slot can still be finalized', () => {
		const tx = txController();
		const guard = new TxGuard(tx, detached);
		assert.throws(() => guard.bytes(), /detached/);
		guard.rollback();
		assert.equal(tx.rollbacks, 1);

		const rx = rxController();
		const read = new RxGuard(rx, detached, 1, 0);
		assert.throws(() => read.data(), /detached/);
		read.commit();
		assert.equal(rx.commits, 1);
	});

	it('the committed check comes before the detached check', () => {
		const guard = new TxGuard(txController(), detached);
		guard.commit(0, 1);
		assert.throws(() => guard.bytes(), /already been committed/);
	});
});
