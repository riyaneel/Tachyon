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
    recvU32(): number;
    rollbackTx(): void;
    rxPtr(): number;
    rxSize(): number;
    rxTypeId(): number;
    /**
     * Copy-based send. Prefer `acquireTx` + direct JS view writes for hot paths.
     */
    send(data: Uint8Array, type_id: number): void;
    sendU32(value: number, type_id: number): void;
    sendU32Unflushed(value: number, type_id: number): void;
}

export function makeTypeId(route: number, ty: number): number;

export function msgType(type_id: number): number;

export function routeId(type_id: number): number;

/**
 * Tiny Rust-side browser program used by the example page.
 *
 * It polls `inbound`, increments a little-endian `u32` payload, and publishes
 * the result to `outbound`. Non-`u32` payloads are echoed unchanged.
 */
export function tachyon_browser_echo_once(inbound: WasmBus, outbound: WasmBus): boolean;
