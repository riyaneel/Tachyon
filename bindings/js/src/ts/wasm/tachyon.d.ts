// Type surface for the Emscripten-generated `tachyon.js` module.
//
// The implementation is produced by Emscripten from the C++ core
// (`npm run build:wasm`). It is MODULARIZE=1 + EXPORT_ES6=1, so the default
// export is an async factory that resolves to the runtime module. Only the
// runtime methods and C ABI accessors the browser bridge needs are declared.

/** Argument / return marshalling types accepted by Emscripten `cwrap`. */
export type CwrapType = 'number' | 'string' | 'boolean' | 'array' | null;

/** A wrapped C function. All Tachyon ABI calls marshal to/from `number` (pointers, sizes, error codes). */
export type CwrapFn = (...args: Array<number | string>) => number;

export interface TachyonCoreModule {
	/** Wraps an exported C function for calling from JS. */
	cwrap(name: string, returnType: CwrapType, argTypes: CwrapType[]): CwrapFn;

	/** Reads a value of the given LLVM type (e.g. `'i32'`) from the heap. */
	getValue(ptr: number, type: string): number;

	/** Writes a value of the given LLVM type to the heap. */
	setValue(ptr: number, value: number, type: string): void;

	/** Allocates `size` bytes on the WASM heap and returns the offset. */
	_malloc(size: number): number;

	/** Frees a pointer previously returned by {@link _malloc}. */
	_free(ptr: number): void;

	/** The WASM linear memory as a byte view. Replaced on memory growth. */
	readonly HEAPU8: Uint8Array;

	/** The WASM linear memory as a 32-bit view. Replaced on memory growth. */
	readonly HEAPU32: Uint32Array;
}

/** Instantiates the Tachyon WASM core. Resolves once the module is ready. */
export default function TachyonCore(moduleArg?: Record<string, unknown>): Promise<TachyonCoreModule>;
