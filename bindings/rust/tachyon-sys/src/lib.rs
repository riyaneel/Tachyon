#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

#[cfg(test)]
mod layout_tests {
    use super::*;
    use std::mem;

    #[test]
    fn tachyon_msg_view_layout() {
        assert_eq!(mem::size_of::<tachyon_msg_view_t>(), 32);
        assert_eq!(mem::align_of::<tachyon_msg_view_t>(), 8);
        let base = mem::offset_of!(tachyon_msg_view_t, ptr);
        assert_eq!(base, 0);
        assert_eq!(mem::offset_of!(tachyon_msg_view_t, actual_size), 8);
        assert_eq!(mem::offset_of!(tachyon_msg_view_t, reserved_), 16);
        assert_eq!(mem::offset_of!(tachyon_msg_view_t, type_id), 24);
        assert_eq!(mem::offset_of!(tachyon_msg_view_t, padding_), 28);
    }
}
