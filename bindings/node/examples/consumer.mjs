import { Bus } from '../dist/index.js';

const SOCKET_PATH = '/tmp/tachyon_node_demo.sock';
const CAPACITY = 1 << 20;

const TYPE_SENTINEL = 0xffff;

console.log(`[consumer] listen ${SOCKET_PATH} (cap=${CAPACITY})`);
const bus = Bus.listen(SOCKET_PATH, CAPACITY);
console.log('[consumer] producer connected');

try {
	let received = 0;
	const start = process.hrtime.bigint();

	for (;;) {
		const { data, typeId } = bus.recv();
		if (typeId === TYPE_SENTINEL) break;
		received++;
		if (received <= 5) {
			console.log(`[consumer] #${received} type=${typeId} "${data.toString()}"`);
		}
	}

	const ms = Number(process.hrtime.bigint() - start) / 1e6;
	const rate = Math.round((received / ms) * 1000);
	console.log(`[consumer] received ${received.toLocaleString()} messages in ${ms.toFixed(2)} ms (${rate.toLocaleString()} msg/s)`);
} finally {
	bus.close();
}
