import createTachyonCore from './wasm/tachyon.js';
import type { BrowserBus } from './browser_bindings.ts';
import { createBrowserBindings } from './browser_bindings.ts';
export * from './browser_bindings.ts';

export const { Bus } = createBrowserBindings(await createTachyonCore());
export type Bus = BrowserBus;
