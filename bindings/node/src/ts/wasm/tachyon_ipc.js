/* @ts-self-types="./tachyon_ipc.d.ts" */
import * as wasm from "./tachyon_ipc_bg.wasm";
import { __wbg_set_wasm } from "./tachyon_ipc_bg.js";

__wbg_set_wasm(wasm);
wasm.__wbindgen_start();
export {
    WasmBus, makeTypeId, msgType, routeId, tachyon_browser_echo_once
} from "./tachyon_ipc_bg.js";
