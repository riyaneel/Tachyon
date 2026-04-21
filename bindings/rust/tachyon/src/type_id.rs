/// type_id encoding
///
/// Encodes a `route_id` and `msg_type` into a single `type_id` value.
#[inline]
pub fn make_type_id(route: u16, msg_type: u16) -> u32 {
    ((route as u32) << 16) | msg_type as u32
}

/// Extracts the `route_id` from bits [31:16] of a `type_id`.
#[inline]
pub fn route_id(type_id: u32) -> u16 {
    (type_id >> 16) as u16
}

/// Extracts the `msg_type` from bits [15:0] of a `type_id`.
#[inline]
pub fn msg_type(type_id: u32) -> u16 {
    (type_id & 0xFFFF) as u16
}
