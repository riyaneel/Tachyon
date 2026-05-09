/* tslint:disable */
/* eslint-disable */

/**
 * Browser-local Tachyon SPSC ring backed by WebAssembly linear memory.
 *
 * This keeps the Tachyon message wire shape (`size`, `type_id`,
 * `reserved_size`, 64-byte alignment, and skip marker) while replacing the
 * native POSIX shared-memory transport with a WASM memory arena that page
 * JavaScript can access through `WebAssembly.Memory`.
 */
export class WasmBus {
    free(): void;
    [Symbol.dispose](): void;
    /**
     * Acquire the next RX message, if one is visible.
     *
     * When this returns `true`, read `rxPtr`, `rxSize`, and `rxTypeId`, then
     * call `commitRx` when done.
     */
    acquireRx(): boolean;
    /**
     * Reserve a TX slot and return a pointer to its payload bytes in WASM memory.
     *
     * JavaScript can create a zero-copy view with:
     * `new Uint8Array(wasm.memory.buffer, ptr, maxSize)`.
     */
    acquireTx(max_size: number): number;
    availableBytes(): number;
    capacity(): number;
    commitRx(): void;
    commitTx(actual_size: number, type_id: number): void;
    commitTxUnflushed(actual_size: number, type_id: number): void;
    dataPtr(): number;
    /**
     * Publish pending unflushed TX messages.
     */
    flush(): void;
    freeBytes(): number;
    isFatal(): boolean;
    constructor(capacity: number);
    rollbackTx(): void;
    rxPtr(): number;
    rxSize(): number;
    rxTypeId(): number;
    /**
     * Copy-based send. Prefer `acquireTx` + direct JS view writes for hot paths.
     */
    send(data: Uint8Array, type_id: number): void;
}

export function makeTypeId(route: number, ty: number): number;

export function msgType(type_id: number): number;

export function routeId(type_id: number): number;

export type InitInput = RequestInfo | URL | Response | BufferSource | WebAssembly.Module;

export interface InitOutput {
    readonly memory: WebAssembly.Memory;
    readonly __wbg_wasmbus_free: (a: number, b: number) => void;
    readonly makeTypeId: (a: number, b: number) => number;
    readonly msgType: (a: number) => number;
    readonly routeId: (a: number) => number;
    readonly wasmbus_acquireRx: (a: number) => [number, number, number];
    readonly wasmbus_acquireTx: (a: number, b: number) => [number, number, number];
    readonly wasmbus_availableBytes: (a: number) => number;
    readonly wasmbus_capacity: (a: number) => number;
    readonly wasmbus_commitRx: (a: number) => [number, number];
    readonly wasmbus_commitTx: (a: number, b: number, c: number) => [number, number];
    readonly wasmbus_commitTxUnflushed: (a: number, b: number, c: number) => [number, number];
    readonly wasmbus_dataPtr: (a: number) => number;
    readonly wasmbus_flush: (a: number) => void;
    readonly wasmbus_freeBytes: (a: number) => number;
    readonly wasmbus_isFatal: (a: number) => number;
    readonly wasmbus_new: (a: number) => [number, number, number];
    readonly wasmbus_rollbackTx: (a: number) => [number, number];
    readonly wasmbus_rxPtr: (a: number) => number;
    readonly wasmbus_rxSize: (a: number) => number;
    readonly wasmbus_rxTypeId: (a: number) => number;
    readonly wasmbus_send: (a: number, b: number, c: number, d: number) => [number, number];
    readonly __wbindgen_externrefs: WebAssembly.Table;
    readonly __externref_table_dealloc: (a: number) => void;
    readonly __wbindgen_malloc: (a: number, b: number) => number;
    readonly __wbindgen_start: () => void;
}

export type SyncInitInput = BufferSource | WebAssembly.Module;

/**
 * Instantiates the given `module`, which can either be bytes or
 * a precompiled `WebAssembly.Module`.
 *
 * @param {{ module: SyncInitInput }} module - Passing `SyncInitInput` directly is deprecated.
 *
 * @returns {InitOutput}
 */
export function initSync(module: { module: SyncInitInput } | SyncInitInput): InitOutput;

/**
 * If `module_or_path` is {RequestInfo} or {URL}, makes a request and
 * for everything else, calls `WebAssembly.instantiate` directly.
 *
 * @param {{ module_or_path: InitInput | Promise<InitInput> }} module_or_path - Passing `InitInput` directly is deprecated.
 *
 * @returns {Promise<InitOutput>}
 */
export default function __wbg_init (module_or_path?: { module_or_path: InitInput | Promise<InitInput> } | InitInput | Promise<InitInput>): Promise<InitOutput>;
