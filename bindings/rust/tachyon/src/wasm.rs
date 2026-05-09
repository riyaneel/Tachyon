use wasm_bindgen::prelude::*;

use crate::type_id::{make_type_id, msg_type, route_id};

const MSG_ALIGNMENT: usize = 64;
const HEADER_SIZE: usize = MSG_ALIGNMENT;
const ALIGN_MASK: usize = MSG_ALIGNMENT - 1;
const SKIP_MARKER: u32 = 0xFFFF_FFFF;
const BATCH_SIZE: u32 = 32;

#[inline]
fn align_message_size(payload_size: usize) -> usize {
    (HEADER_SIZE + payload_size + ALIGN_MASK) & !ALIGN_MASK
}

#[inline]
fn js_error(message: &str) -> JsValue {
    JsValue::from_str(message)
}

/// Browser-local Tachyon SPSC ring backed by WebAssembly linear memory.
///
/// This keeps the Tachyon message wire shape (`size`, `type_id`,
/// `reserved_size`, 64-byte alignment, and skip marker) while replacing the
/// native POSIX shared-memory transport with a WASM memory arena that page
/// JavaScript can access through `WebAssembly.Memory`.
#[wasm_bindgen]
pub struct WasmBus {
    arena: Box<[u8]>,
    capacity: usize,
    mask: usize,
    head: usize,
    published_head: usize,
    tail: usize,
    pending_tx: u32,
    tx_reserved_size: usize,
    pre_acquire_head: usize,
    rx_payload_offset: usize,
    rx_reserved_size: usize,
    rx_actual_size: usize,
    rx_type_id: u32,
    fatal: bool,
}

#[wasm_bindgen]
impl WasmBus {
    #[wasm_bindgen(constructor)]
    pub fn new(capacity: u32) -> Result<WasmBus, JsValue> {
        let capacity = capacity as usize;
        if capacity < MSG_ALIGNMENT || !capacity.is_power_of_two() {
            return Err(js_error(
                "WasmBus capacity must be a power of two and at least 64 bytes",
            ));
        }

        Ok(WasmBus {
            arena: vec![0; capacity].into_boxed_slice(),
            capacity,
            mask: capacity - 1,
            head: 0,
            published_head: 0,
            tail: 0,
            pending_tx: 0,
            tx_reserved_size: 0,
            pre_acquire_head: 0,
            rx_payload_offset: 0,
            rx_reserved_size: 0,
            rx_actual_size: 0,
            rx_type_id: 0,
            fatal: false,
        })
    }

    pub fn capacity(&self) -> u32 {
        self.capacity as u32
    }

    #[wasm_bindgen(js_name = dataPtr)]
    pub fn data_ptr(&self) -> u32 {
        self.arena.as_ptr() as u32
    }

    #[wasm_bindgen(js_name = availableBytes)]
    pub fn available_bytes(&self) -> u32 {
        self.published_head.saturating_sub(self.tail) as u32
    }

    #[wasm_bindgen(js_name = freeBytes)]
    pub fn free_bytes(&self) -> u32 {
        self.capacity
            .saturating_sub(self.head.saturating_sub(self.tail)) as u32
    }

    #[wasm_bindgen(js_name = isFatal)]
    pub fn is_fatal(&self) -> bool {
        self.fatal
    }

    /// Copy-based send. Prefer `acquireTx` + direct JS view writes for hot paths.
    pub fn send(&mut self, data: &[u8], type_id: u32) -> Result<(), JsValue> {
        let payload_offset = self.acquire_tx_offset(data.len())?;
        self.arena[payload_offset..payload_offset + data.len()].copy_from_slice(data);
        self.commit_tx(data.len() as u32, type_id)
    }

    #[wasm_bindgen(js_name = sendU32)]
    pub fn send_u32(&mut self, value: u32, type_id: u32) -> Result<(), JsValue> {
        self.send_u32_inner(value, type_id, true)
    }

    #[wasm_bindgen(js_name = sendU32Unflushed)]
    pub fn send_u32_unflushed(&mut self, value: u32, type_id: u32) -> Result<(), JsValue> {
        self.send_u32_inner(value, type_id, false)
    }

    /// Reserve a TX slot and return a pointer to its payload bytes in WASM memory.
    ///
    /// JavaScript can create a zero-copy view with:
    /// `new Uint8Array(wasm.memory.buffer, ptr, maxSize)`.
    #[wasm_bindgen(js_name = acquireTx)]
    pub fn acquire_tx(&mut self, max_size: u32) -> Result<u32, JsValue> {
        let payload_offset = self.acquire_tx_offset(max_size as usize)?;
        Ok((self.arena.as_ptr() as usize + payload_offset) as u32)
    }

    #[wasm_bindgen(js_name = commitTx)]
    pub fn commit_tx(&mut self, actual_size: u32, type_id: u32) -> Result<(), JsValue> {
        self.commit_tx_inner(actual_size as usize, type_id, true)
    }

    #[wasm_bindgen(js_name = commitTxUnflushed)]
    pub fn commit_tx_unflushed(&mut self, actual_size: u32, type_id: u32) -> Result<(), JsValue> {
        self.commit_tx_inner(actual_size as usize, type_id, false)
    }

    #[wasm_bindgen(js_name = rollbackTx)]
    pub fn rollback_tx(&mut self) -> Result<(), JsValue> {
        if self.tx_reserved_size == 0 {
            return Err(js_error("no pending TX slot to roll back"));
        }
        self.head = self.pre_acquire_head;
        self.tx_reserved_size = 0;
        Ok(())
    }

    /// Publish pending unflushed TX messages.
    pub fn flush(&mut self) {
        self.published_head = self.head;
        self.pending_tx = 0;
    }

    /// Acquire the next RX message, if one is visible.
    ///
    /// When this returns `true`, read `rxPtr`, `rxSize`, and `rxTypeId`, then
    /// call `commitRx` when done.
    #[wasm_bindgen(js_name = acquireRx)]
    pub fn acquire_rx(&mut self) -> Result<bool, JsValue> {
        if self.fatal {
            return Err(js_error("WasmBus is in fatal state"));
        }

        if self.rx_reserved_size != 0 {
            return Ok(true);
        }

        if self.published_head <= self.tail {
            return Ok(false);
        }

        let mut physical_idx = self.tail & self.mask;
        if self.capacity - physical_idx < 12 {
            self.fatal = true;
            return Err(js_error("corrupt Tachyon ring: truncated message header"));
        }

        let mut size = self.read_u32(physical_idx);
        let mut type_id = self.read_u32(physical_idx + 4);
        let mut reserved_size = self.read_u32(physical_idx + 8) as usize;

        if size == SKIP_MARKER {
            let space_until_end = self.capacity - physical_idx;
            self.tail += space_until_end;
            physical_idx = 0;

            if self.published_head <= self.tail {
                return Ok(false);
            }

            size = self.read_u32(0);
            type_id = self.read_u32(4);
            reserved_size = self.read_u32(8) as usize;
        }

        let actual_size = size as usize;
        if reserved_size < HEADER_SIZE
            || reserved_size > self.capacity
            || (reserved_size & ALIGN_MASK) != 0
            || actual_size > reserved_size - HEADER_SIZE
        {
            self.fatal = true;
            return Err(js_error("corrupt Tachyon ring: invalid message metadata"));
        }

        self.rx_payload_offset = physical_idx + HEADER_SIZE;
        self.rx_reserved_size = reserved_size;
        self.rx_actual_size = actual_size;
        self.rx_type_id = type_id;

        Ok(true)
    }

    #[wasm_bindgen(js_name = rxPtr)]
    pub fn rx_ptr(&self) -> u32 {
        if self.rx_reserved_size == 0 {
            return 0;
        }
        (self.arena.as_ptr() as usize + self.rx_payload_offset) as u32
    }

    #[wasm_bindgen(js_name = rxSize)]
    pub fn rx_size(&self) -> u32 {
        self.rx_actual_size as u32
    }

    #[wasm_bindgen(js_name = rxTypeId)]
    pub fn rx_type_id(&self) -> u32 {
        self.rx_type_id
    }

    #[wasm_bindgen(js_name = commitRx)]
    pub fn commit_rx(&mut self) -> Result<(), JsValue> {
        if self.rx_reserved_size == 0 {
            return Err(js_error("no pending RX slot to commit"));
        }

        self.tail += self.rx_reserved_size;
        self.rx_payload_offset = 0;
        self.rx_reserved_size = 0;
        self.rx_actual_size = 0;
        self.rx_type_id = 0;

        Ok(())
    }

    #[wasm_bindgen(js_name = recvU32)]
    pub fn recv_u32(&mut self) -> Result<u32, JsValue> {
        if !self.acquire_rx()? {
            return Err(js_error("no u32 message available"));
        }
        if self.rx_actual_size != 4 {
            return Err(js_error("pending message is not a u32 payload"));
        }

        let mut bytes = [0u8; 4];
        bytes.copy_from_slice(&self.arena[self.rx_payload_offset..self.rx_payload_offset + 4]);
        let value = u32::from_le_bytes(bytes);
        self.commit_rx()?;
        Ok(value)
    }
}

impl WasmBus {
    fn send_u32_inner(&mut self, value: u32, type_id: u32, flush: bool) -> Result<(), JsValue> {
        let payload_offset = self.acquire_tx_offset(4)?;
        self.arena[payload_offset..payload_offset + 4].copy_from_slice(&value.to_le_bytes());
        self.commit_tx_inner(4, type_id, flush)
    }

    fn acquire_tx_offset(&mut self, max_size: usize) -> Result<usize, JsValue> {
        if self.fatal {
            return Err(js_error("WasmBus is in fatal state"));
        }
        if self.tx_reserved_size != 0 {
            return Err(js_error("a TX slot is already pending"));
        }

        let aligned_size = align_message_size(max_size);
        if aligned_size > self.capacity || max_size > (SKIP_MARKER as usize) - HEADER_SIZE {
            return Err(js_error("message is larger than the Tachyon ring capacity"));
        }

        let mut physical_idx = self.head & self.mask;
        let space_until_end = self.capacity - physical_idx;
        let need_skip = space_until_end < aligned_size;
        let required_space = aligned_size + if need_skip { space_until_end } else { 0 };

        if self.head - self.tail + required_space > self.capacity {
            return Err(js_error("Tachyon ring buffer is full"));
        }

        if need_skip {
            self.write_u32(physical_idx, SKIP_MARKER);
            self.write_u32(physical_idx + 4, 0);
            self.write_u32(physical_idx + 8, 0);
            self.head += space_until_end;
            physical_idx = 0;
        }

        self.pre_acquire_head = self.head;
        self.tx_reserved_size = aligned_size;

        Ok(physical_idx + HEADER_SIZE)
    }

    fn commit_tx_inner(
        &mut self,
        actual_size: usize,
        type_id: u32,
        flush: bool,
    ) -> Result<(), JsValue> {
        if self.tx_reserved_size == 0 {
            return Err(js_error("no pending TX slot to commit"));
        }
        if actual_size > self.tx_reserved_size - HEADER_SIZE {
            self.tx_reserved_size = 0;
            return Err(js_error("actual TX size exceeds reserved slot size"));
        }

        let physical_idx = self.head & self.mask;
        self.write_u32(physical_idx, actual_size as u32);
        self.write_u32(physical_idx + 4, type_id);
        self.write_u32(physical_idx + 8, self.tx_reserved_size as u32);
        self.write_u32(physical_idx + 12, 0);
        self.write_u32(physical_idx + 16, 0);
        self.write_u32(physical_idx + 20, 0);

        self.head += self.tx_reserved_size;
        self.tx_reserved_size = 0;
        self.pending_tx += 1;

        if flush || self.pending_tx >= BATCH_SIZE {
            self.flush();
        }

        Ok(())
    }

    fn read_u32(&self, offset: usize) -> u32 {
        let mut bytes = [0u8; 4];
        bytes.copy_from_slice(&self.arena[offset..offset + 4]);
        u32::from_le_bytes(bytes)
    }

    fn write_u32(&mut self, offset: usize, value: u32) {
        self.arena[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
    }
}

/// Tiny Rust-side browser program used by the example page.
///
/// It polls `inbound`, increments a little-endian `u32` payload, and publishes
/// the result to `outbound`. Non-`u32` payloads are echoed unchanged.
#[wasm_bindgen]
pub fn tachyon_browser_echo_once(
    inbound: &mut WasmBus,
    outbound: &mut WasmBus,
) -> Result<bool, JsValue> {
    if !inbound.acquire_rx()? {
        return Ok(false);
    }

    let type_id = inbound.rx_type_id;
    let actual_size = inbound.rx_actual_size;
    let inbound_offset = inbound.rx_payload_offset;
    let outbound_offset = outbound.acquire_tx_offset(actual_size)?;

    if actual_size == 4 {
        let mut bytes = [0u8; 4];
        bytes.copy_from_slice(&inbound.arena[inbound_offset..inbound_offset + 4]);
        let value = u32::from_le_bytes(bytes).wrapping_add(1);
        outbound.arena[outbound_offset..outbound_offset + 4].copy_from_slice(&value.to_le_bytes());
    } else {
        outbound.arena[outbound_offset..outbound_offset + actual_size]
            .copy_from_slice(&inbound.arena[inbound_offset..inbound_offset + actual_size]);
    }

    outbound.commit_tx_inner(
        actual_size,
        make_type_id(route_id(type_id).wrapping_add(1), msg_type(type_id)),
        true,
    )?;
    inbound.commit_rx()?;

    Ok(true)
}

#[wasm_bindgen(js_name = makeTypeId)]
pub fn wasm_make_type_id(route: u16, ty: u16) -> u32 {
    make_type_id(route, ty)
}

#[wasm_bindgen(js_name = routeId)]
pub fn wasm_route_id(type_id: u32) -> u16 {
    route_id(type_id)
}

#[wasm_bindgen(js_name = msgType)]
pub fn wasm_msg_type(type_id: u32) -> u16 {
    msg_type(type_id)
}
