use std::ptr::NonNull;

use tachyon_sys::{
    tachyon_rpc_acquire_reply_tx, tachyon_rpc_acquire_tx, tachyon_rpc_bus_t, tachyon_rpc_call,
    tachyon_rpc_commit_call, tachyon_rpc_commit_reply, tachyon_rpc_commit_rx,
    tachyon_rpc_commit_serve, tachyon_rpc_connect, tachyon_rpc_destroy, tachyon_rpc_listen,
    tachyon_rpc_reply, tachyon_rpc_rollback_call, tachyon_rpc_rollback_reply, tachyon_rpc_serve,
    tachyon_rpc_set_polling_mode, tachyon_rpc_wait,
};

use crate::error::{TachyonError, from_raw};

pub struct RpcBus {
    inner: NonNull<tachyon_rpc_bus_t>,
}

unsafe impl Send for RpcBus {}

unsafe impl Sync for RpcBus {}

impl RpcBus {
    /// Creates two SHM arenas (fwd + rev) and blocks until a connector arrives.
    pub fn listen(socket_path: &str, cap_fwd: usize, cap_rev: usize) -> Result<Self, TachyonError> {
        let path = std::ffi::CString::new(socket_path).map_err(|_| TachyonError::NullPtr)?;
        let mut ptr: *mut tachyon_rpc_bus_t = std::ptr::null_mut();
        let err = unsafe { tachyon_rpc_listen(path.as_ptr(), cap_fwd, cap_rev, &mut ptr) };
        from_raw(err)?;
        Ok(Self {
            inner: NonNull::new(ptr).ok_or(TachyonError::NullPtr)?,
        })
    }

    /// Attaches to existing SHM arenas via UNIX socket.
    pub fn connect(socket_path: &str) -> Result<Self, TachyonError> {
        let path = std::ffi::CString::new(socket_path).map_err(|_| TachyonError::NullPtr)?;
        let mut ptr: *mut tachyon_rpc_bus_t = std::ptr::null_mut();
        let err = unsafe { tachyon_rpc_connect(path.as_ptr(), &mut ptr) };
        from_raw(err)?;
        Ok(Self {
            inner: NonNull::new(ptr).ok_or(TachyonError::NullPtr)?,
        })
    }

    /// Zero-copy call: Acquire a TX slot in arena_fwd, let the caller fill it,
    /// commit, and return the assigned correlation_id.
    pub fn acquire_call(&self, max_size: usize) -> Result<RpcTxGuard<'_>, TachyonError> {
        let ptr = unsafe { tachyon_rpc_acquire_tx(self.inner.as_ptr(), max_size) };
        if ptr.is_null() {
            return Err(TachyonError::BufferFull);
        }
        Ok(RpcTxGuard {
            bus: self,
            ptr: ptr as *mut u8,
            max_size,
            is_reply: false,
            cid: 0,
            committed: false,
        })
    }

    /// Copy-based call shorthand. Commits and flushes immediately.
    pub fn call(&self, payload: &[u8], msg_type: u32) -> Result<u64, TachyonError> {
        let mut out_cid: u64 = 0;
        let err = unsafe {
            tachyon_rpc_call(
                self.inner.as_ptr(),
                payload.as_ptr() as *const _,
                payload.len(),
                msg_type,
                &mut out_cid,
            )
        };
        from_raw(err)?;
        Ok(out_cid)
    }

    /// Block until the response matching `correlation_id` arrives in arena_rev.
    pub fn wait(
        &self,
        correlation_id: u64,
        spin_threshold: u32,
    ) -> Result<RpcRxGuard<'_>, TachyonError> {
        let mut actual_size: usize = 0;
        let mut type_id: u32 = 0;
        let ptr = unsafe {
            tachyon_rpc_wait(
                self.inner.as_ptr(),
                correlation_id,
                &mut actual_size,
                &mut type_id,
                spin_threshold,
            )
        };
        if ptr.is_null() {
            return Err(TachyonError::PeerDead);
        }
        Ok(RpcRxGuard {
            bus: self,
            ptr: ptr as *const u8,
            actual_size,
            type_id,
            correlation_id,
            is_serve: false,
            committed: false,
        })
    }

    /// Block until a request arrives in arena_fwd.
    pub fn serve(&self, spin_threshold: u32) -> Result<RpcRxGuard<'_>, TachyonError> {
        let mut correlation_id: u64 = 0;
        let mut type_id: u32 = 0;
        let mut actual_size: usize = 0;
        let ptr = unsafe {
            tachyon_rpc_serve(
                self.inner.as_ptr(),
                &mut correlation_id,
                &mut type_id,
                &mut actual_size,
                spin_threshold,
            )
        };
        if ptr.is_null() {
            return Err(TachyonError::Interrupted);
        }
        Ok(RpcRxGuard {
            bus: self,
            ptr: ptr as *const u8,
            actual_size,
            type_id,
            correlation_id,
            is_serve: true,
            committed: false,
        })
    }

    /// Zero-copy reply: Acquire a TX slot in arena_rev for `correlation_id`.
    pub fn acquire_reply(
        &self,
        correlation_id: u64,
        max_size: usize,
    ) -> Result<RpcTxGuard<'_>, TachyonError> {
        if correlation_id == 0 {
            return Err(TachyonError::InvalidSize);
        }
        let ptr = unsafe { tachyon_rpc_acquire_reply_tx(self.inner.as_ptr(), max_size) };
        if ptr.is_null() {
            return Err(TachyonError::BufferFull);
        }
        Ok(RpcTxGuard {
            bus: self,
            ptr: ptr as *mut u8,
            max_size,
            is_reply: true,
            cid: correlation_id,
            committed: false,
        })
    }

    /// Copy-based reply shorthand.
    pub fn reply(
        &self,
        correlation_id: u64,
        payload: &[u8],
        msg_type: u32,
    ) -> Result<(), TachyonError> {
        let err = unsafe {
            tachyon_rpc_reply(
                self.inner.as_ptr(),
                correlation_id,
                payload.as_ptr() as *const _,
                payload.len(),
                msg_type,
            )
        };
        from_raw(err)
    }

    pub fn set_polling_mode(&self, pure_spin: bool) {
        unsafe { tachyon_rpc_set_polling_mode(self.inner.as_ptr(), pure_spin as i32) }
    }
}

impl Drop for RpcBus {
    fn drop(&mut self) {
        unsafe { tachyon_rpc_destroy(self.inner.as_ptr()) }
    }
}

/// Zero-copy TX slot. Fill via `write()`, then `commit(actual_size, msg_type)`.
/// Dropped without commit -> automatic rollback.
pub struct RpcTxGuard<'a> {
    bus: &'a RpcBus,
    ptr: *mut u8,
    max_size: usize,
    is_reply: bool,
    cid: u64, /* reply_cid for is_reply=true, 0 for call side */
    committed: bool,
}

impl<'a> RpcTxGuard<'a> {
    /// Write `data` into the TX slot. Panics if `data.len() > max_size`.
    pub fn write(&self, data: &[u8]) {
        assert!(
            data.len() <= self.max_size,
            "write exceeds reserved TX slot"
        );
        unsafe { std::ptr::copy_nonoverlapping(data.as_ptr(), self.ptr, data.len()) }
    }

    /// Returns a mutable slice over the full reserved slot for in-place writes.
    pub fn as_mut_slice(&mut self) -> &mut [u8] {
        unsafe { std::slice::from_raw_parts_mut(self.ptr, self.max_size) }
    }

    /// Commit the TX slot. Returns `correlation_id` (call side) or 0 (reply side).
    pub fn commit(mut self, actual_size: usize, msg_type: u32) -> Result<u64, TachyonError> {
        self.committed = true;
        if !self.is_reply {
            let mut out_cid: u64 = 0;
            let err = unsafe {
                tachyon_rpc_commit_call(
                    self.bus.inner.as_ptr(),
                    actual_size,
                    msg_type,
                    &mut out_cid,
                )
            };
            from_raw(err)?;
            Ok(out_cid)
        } else {
            let err = unsafe {
                tachyon_rpc_commit_reply(self.bus.inner.as_ptr(), self.cid, actual_size, msg_type)
            };
            from_raw(err)?;
            Ok(0)
        }
    }
}

impl Drop for RpcTxGuard<'_> {
    fn drop(&mut self) {
        if !self.committed {
            if !self.is_reply {
                unsafe { tachyon_rpc_rollback_call(self.bus.inner.as_ptr()) };
            } else {
                unsafe { tachyon_rpc_rollback_reply(self.bus.inner.as_ptr()) };
            }
        }
    }
}

/// Zero-copy RX slot. Read via `data()`, then `commit()` to release the slot.
/// Dropped without commit -> automatic commit (slot must be released).
pub struct RpcRxGuard<'a> {
    bus: &'a RpcBus,
    ptr: *const u8,
    pub actual_size: usize,
    pub type_id: u32,
    pub correlation_id: u64,
    is_serve: bool,
    committed: bool,
}

impl<'a> RpcRxGuard<'a> {
    /// Zero-copy slice of the received payload. Valid until `commit()`.
    pub fn data(&self) -> &[u8] {
        unsafe { std::slice::from_raw_parts(self.ptr, self.actual_size) }
    }

    /// Release the RX slot back to the arena.
    pub fn commit(mut self) -> Result<(), TachyonError> {
        self.committed = true;
        let err = if self.is_serve {
            unsafe { tachyon_rpc_commit_serve(self.bus.inner.as_ptr()) }
        } else {
            unsafe { tachyon_rpc_commit_rx(self.bus.inner.as_ptr()) }
        };
        from_raw(err)
    }
}

impl Drop for RpcRxGuard<'_> {
    fn drop(&mut self) {
        if !self.committed {
            let err = if self.is_serve {
                unsafe { tachyon_rpc_commit_serve(self.bus.inner.as_ptr()) }
            } else {
                unsafe { tachyon_rpc_commit_rx(self.bus.inner.as_ptr()) }
            };
            let _ = from_raw(err);
        }
    }
}
