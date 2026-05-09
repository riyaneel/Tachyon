/* tslint:disable */
/* eslint-disable */
export const memory: WebAssembly.Memory;
export const __wbg_wasmbus_free: (a: number, b: number) => void;
export const makeTypeId: (a: number, b: number) => number;
export const msgType: (a: number) => number;
export const routeId: (a: number) => number;
export const wasmbus_acquireRx: (a: number) => [number, number, number];
export const wasmbus_acquireTx: (a: number, b: number) => [number, number, number];
export const wasmbus_availableBytes: (a: number) => number;
export const wasmbus_capacity: (a: number) => number;
export const wasmbus_commitRx: (a: number) => [number, number];
export const wasmbus_commitTx: (a: number, b: number, c: number) => [number, number];
export const wasmbus_commitTxUnflushed: (a: number, b: number, c: number) => [number, number];
export const wasmbus_dataPtr: (a: number) => number;
export const wasmbus_flush: (a: number) => void;
export const wasmbus_freeBytes: (a: number) => number;
export const wasmbus_isFatal: (a: number) => number;
export const wasmbus_new: (a: number) => [number, number, number];
export const wasmbus_rollbackTx: (a: number) => [number, number];
export const wasmbus_rxPtr: (a: number) => number;
export const wasmbus_rxSize: (a: number) => number;
export const wasmbus_rxTypeId: (a: number) => number;
export const wasmbus_send: (a: number, b: number, c: number, d: number) => [number, number];
export const __wbindgen_externrefs: WebAssembly.Table;
export const __externref_table_dealloc: (a: number) => void;
export const __wbindgen_malloc: (a: number, b: number) => number;
export const __wbindgen_start: () => void;
