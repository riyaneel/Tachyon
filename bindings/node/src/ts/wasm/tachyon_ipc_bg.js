/**
 * Browser-local Tachyon SPSC ring backed by WebAssembly linear memory.
 *
 * This keeps the Tachyon message wire shape (`size`, `type_id`,
 * `reserved_size`, 64-byte alignment, and skip marker) while replacing the
 * native POSIX shared-memory transport with a WASM memory arena that page
 * JavaScript can access through `WebAssembly.Memory`.
 */
export class WasmBus {
    __destroy_into_raw() {
        const ptr = this.__wbg_ptr;
        this.__wbg_ptr = 0;
        WasmBusFinalization.unregister(this);
        return ptr;
    }
    free() {
        const ptr = this.__destroy_into_raw();
        wasm.__wbg_wasmbus_free(ptr, 0);
    }
    /**
     * Acquire the next RX message, if one is visible.
     *
     * When this returns `true`, read `rxPtr`, `rxSize`, and `rxTypeId`, then
     * call `commitRx` when done.
     * @returns {boolean}
     */
    acquireRx() {
        const ret = wasm.wasmbus_acquireRx(this.__wbg_ptr);
        if (ret[2]) {
            throw takeFromExternrefTable0(ret[1]);
        }
        return ret[0] !== 0;
    }
    /**
     * Reserve a TX slot and return a pointer to its payload bytes in WASM memory.
     *
     * JavaScript can create a zero-copy view with:
     * `new Uint8Array(wasm.memory.buffer, ptr, maxSize)`.
     * @param {number} max_size
     * @returns {number}
     */
    acquireTx(max_size) {
        const ret = wasm.wasmbus_acquireTx(this.__wbg_ptr, max_size);
        if (ret[2]) {
            throw takeFromExternrefTable0(ret[1]);
        }
        return ret[0] >>> 0;
    }
    /**
     * @returns {number}
     */
    availableBytes() {
        const ret = wasm.wasmbus_availableBytes(this.__wbg_ptr);
        return ret >>> 0;
    }
    /**
     * @returns {number}
     */
    capacity() {
        const ret = wasm.wasmbus_capacity(this.__wbg_ptr);
        return ret >>> 0;
    }
    commitRx() {
        const ret = wasm.wasmbus_commitRx(this.__wbg_ptr);
        if (ret[1]) {
            throw takeFromExternrefTable0(ret[0]);
        }
    }
    /**
     * @param {number} actual_size
     * @param {number} type_id
     */
    commitTx(actual_size, type_id) {
        const ret = wasm.wasmbus_commitTx(this.__wbg_ptr, actual_size, type_id);
        if (ret[1]) {
            throw takeFromExternrefTable0(ret[0]);
        }
    }
    /**
     * @param {number} actual_size
     * @param {number} type_id
     */
    commitTxUnflushed(actual_size, type_id) {
        const ret = wasm.wasmbus_commitTxUnflushed(this.__wbg_ptr, actual_size, type_id);
        if (ret[1]) {
            throw takeFromExternrefTable0(ret[0]);
        }
    }
    /**
     * @returns {number}
     */
    dataPtr() {
        const ret = wasm.wasmbus_dataPtr(this.__wbg_ptr);
        return ret >>> 0;
    }
    /**
     * Publish pending unflushed TX messages.
     */
    flush() {
        wasm.wasmbus_flush(this.__wbg_ptr);
    }
    /**
     * @returns {number}
     */
    freeBytes() {
        const ret = wasm.wasmbus_freeBytes(this.__wbg_ptr);
        return ret >>> 0;
    }
    /**
     * @returns {boolean}
     */
    isFatal() {
        const ret = wasm.wasmbus_isFatal(this.__wbg_ptr);
        return ret !== 0;
    }
    /**
     * @param {number} capacity
     */
    constructor(capacity) {
        const ret = wasm.wasmbus_new(capacity);
        if (ret[2]) {
            throw takeFromExternrefTable0(ret[1]);
        }
        this.__wbg_ptr = ret[0];
        WasmBusFinalization.register(this, this.__wbg_ptr, this);
        return this;
    }
    /**
     * @returns {number}
     */
    recvU32() {
        const ret = wasm.wasmbus_recvU32(this.__wbg_ptr);
        if (ret[2]) {
            throw takeFromExternrefTable0(ret[1]);
        }
        return ret[0] >>> 0;
    }
    rollbackTx() {
        const ret = wasm.wasmbus_rollbackTx(this.__wbg_ptr);
        if (ret[1]) {
            throw takeFromExternrefTable0(ret[0]);
        }
    }
    /**
     * @returns {number}
     */
    rxPtr() {
        const ret = wasm.wasmbus_rxPtr(this.__wbg_ptr);
        return ret >>> 0;
    }
    /**
     * @returns {number}
     */
    rxSize() {
        const ret = wasm.wasmbus_rxSize(this.__wbg_ptr);
        return ret >>> 0;
    }
    /**
     * @returns {number}
     */
    rxTypeId() {
        const ret = wasm.wasmbus_rxTypeId(this.__wbg_ptr);
        return ret >>> 0;
    }
    /**
     * Copy-based send. Prefer `acquireTx` + direct JS view writes for hot paths.
     * @param {Uint8Array} data
     * @param {number} type_id
     */
    send(data, type_id) {
        const ptr0 = passArray8ToWasm0(data, wasm.__wbindgen_malloc);
        const len0 = WASM_VECTOR_LEN;
        const ret = wasm.wasmbus_send(this.__wbg_ptr, ptr0, len0, type_id);
        if (ret[1]) {
            throw takeFromExternrefTable0(ret[0]);
        }
    }
    /**
     * @param {number} value
     * @param {number} type_id
     */
    sendU32(value, type_id) {
        const ret = wasm.wasmbus_sendU32(this.__wbg_ptr, value, type_id);
        if (ret[1]) {
            throw takeFromExternrefTable0(ret[0]);
        }
    }
    /**
     * @param {number} value
     * @param {number} type_id
     */
    sendU32Unflushed(value, type_id) {
        const ret = wasm.wasmbus_sendU32Unflushed(this.__wbg_ptr, value, type_id);
        if (ret[1]) {
            throw takeFromExternrefTable0(ret[0]);
        }
    }
}
if (Symbol.dispose) WasmBus.prototype[Symbol.dispose] = WasmBus.prototype.free;

/**
 * @param {number} route
 * @param {number} ty
 * @returns {number}
 */
export function makeTypeId(route, ty) {
    const ret = wasm.makeTypeId(route, ty);
    return ret >>> 0;
}

/**
 * @param {number} type_id
 * @returns {number}
 */
export function msgType(type_id) {
    const ret = wasm.msgType(type_id);
    return ret;
}

/**
 * @param {number} type_id
 * @returns {number}
 */
export function routeId(type_id) {
    const ret = wasm.routeId(type_id);
    return ret;
}

/**
 * Tiny Rust-side browser program used by the example page.
 *
 * It polls `inbound`, increments a little-endian `u32` payload, and publishes
 * the result to `outbound`. Non-`u32` payloads are echoed unchanged.
 * @param {WasmBus} inbound
 * @param {WasmBus} outbound
 * @returns {boolean}
 */
export function tachyon_browser_echo_once(inbound, outbound) {
    _assertClass(inbound, WasmBus);
    _assertClass(outbound, WasmBus);
    const ret = wasm.tachyon_browser_echo_once(inbound.__wbg_ptr, outbound.__wbg_ptr);
    if (ret[2]) {
        throw takeFromExternrefTable0(ret[1]);
    }
    return ret[0] !== 0;
}
export function __wbg___wbindgen_throw_9c31b086c2b26051(arg0, arg1) {
    throw new Error(getStringFromWasm0(arg0, arg1));
}
export function __wbindgen_cast_0000000000000001(arg0, arg1) {
    // Cast intrinsic for `Ref(String) -> Externref`.
    const ret = getStringFromWasm0(arg0, arg1);
    return ret;
}
export function __wbindgen_init_externref_table() {
    const table = wasm.__wbindgen_externrefs;
    const offset = table.grow(4);
    table.set(0, undefined);
    table.set(offset + 0, undefined);
    table.set(offset + 1, null);
    table.set(offset + 2, true);
    table.set(offset + 3, false);
}
const WasmBusFinalization = (typeof FinalizationRegistry === 'undefined')
    ? { register: () => {}, unregister: () => {} }
    : new FinalizationRegistry(ptr => wasm.__wbg_wasmbus_free(ptr, 1));

function _assertClass(instance, klass) {
    if (!(instance instanceof klass)) {
        throw new Error(`expected instance of ${klass.name}`);
    }
}

function getStringFromWasm0(ptr, len) {
    return decodeText(ptr >>> 0, len);
}

let cachedUint8ArrayMemory0 = null;
function getUint8ArrayMemory0() {
    if (cachedUint8ArrayMemory0 === null || cachedUint8ArrayMemory0.byteLength === 0) {
        cachedUint8ArrayMemory0 = new Uint8Array(wasm.memory.buffer);
    }
    return cachedUint8ArrayMemory0;
}

function passArray8ToWasm0(arg, malloc) {
    const ptr = malloc(arg.length * 1, 1) >>> 0;
    getUint8ArrayMemory0().set(arg, ptr / 1);
    WASM_VECTOR_LEN = arg.length;
    return ptr;
}

function takeFromExternrefTable0(idx) {
    const value = wasm.__wbindgen_externrefs.get(idx);
    wasm.__externref_table_dealloc(idx);
    return value;
}

let cachedTextDecoder = new TextDecoder('utf-8', { ignoreBOM: true, fatal: true });
cachedTextDecoder.decode();
const MAX_SAFARI_DECODE_BYTES = 2146435072;
let numBytesDecoded = 0;
function decodeText(ptr, len) {
    numBytesDecoded += len;
    if (numBytesDecoded >= MAX_SAFARI_DECODE_BYTES) {
        cachedTextDecoder = new TextDecoder('utf-8', { ignoreBOM: true, fatal: true });
        cachedTextDecoder.decode();
        numBytesDecoded = len;
    }
    return cachedTextDecoder.decode(getUint8ArrayMemory0().subarray(ptr, ptr + len));
}

let WASM_VECTOR_LEN = 0;


let wasm;
export function __wbg_set_wasm(val) {
    wasm = val;
}
