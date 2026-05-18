use tachyon_ipc::WasmBus;
use wasm_bindgen::prelude::*;

/// Tiny Rust-side browser program used by this example page.
///
/// It checks `inbound` once, increments a little-endian `u32` payload, and
/// publishes the result to `outbound`. The reusable transport logic stays in
/// the Tachyon bindings; this file owns only the page-specific demo behavior.
#[wasm_bindgen]
pub fn tachyon_browser_echo_once(
    inbound: &mut WasmBus,
    outbound: &mut WasmBus,
) -> Result<bool, JsValue> {
    if !inbound.acquire_rx()? {
        return Ok(false);
    }

    let type_id = inbound.rx_type_id();
    let actual_size = inbound.rx_size();
    if actual_size != 4 {
        inbound.commit_rx()?;
        return Err(JsValue::from_str(
            "example echo expects a 4-byte u32 payload",
        ));
    }

    let inbound_ptr = inbound.rx_ptr() as *const u32;
    let outbound_ptr = outbound.acquire_tx(actual_size)? as *mut u32;

    unsafe {
        let value = inbound_ptr.read_unaligned().wrapping_add(1);
        outbound_ptr.write_unaligned(value);
    }

    outbound.commit_tx(actual_size, type_id.wrapping_add(1 << 16))?;
    inbound.commit_rx()?;

    Ok(true)
}
