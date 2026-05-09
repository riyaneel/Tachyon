use tachyon_ipc::{WasmBus, make_type_id, msg_type, route_id};
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

    let inbound_ptr = inbound.rx_ptr() as *const u8;
    let outbound_ptr = outbound.acquire_tx(actual_size)? as *mut u8;

    unsafe {
        let inbound_bytes = std::slice::from_raw_parts(inbound_ptr, actual_size as usize);
        let outbound_bytes = std::slice::from_raw_parts_mut(outbound_ptr, actual_size as usize);
        let value = u32::from_le_bytes(inbound_bytes.try_into().unwrap()).wrapping_add(1);
        outbound_bytes.copy_from_slice(&value.to_le_bytes());
    }

    outbound.commit_tx(
        actual_size,
        make_type_id(route_id(type_id).wrapping_add(1), msg_type(type_id)),
    )?;
    inbound.commit_rx()?;

    Ok(true)
}
