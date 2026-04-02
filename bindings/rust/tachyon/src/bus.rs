use crate::error::{TachyonError, from_raw};
use std::ffi::CString;
use std::ptr::NonNull;
use tachyon_sys::*;

const STATE_FATAL_ERROR: u32 = tachyon_state_t_TACHYON_STATE_FATAL_ERROR as u32;

/// SPSC IPC bus. `Send` but not `Sync` — one `Bus` per thread.
pub struct Bus {
    inner: NonNull<tachyon_bus_t>,
}

// SAFETY: tachyon_bus_t transfers safely between threads.
// The C layer uses atomic spinlocks for producer/consumer access.
unsafe impl Send for Bus {}

impl Drop for Bus {
    fn drop(&mut self) {
        unsafe { tachyon_bus_destroy(self.inner.as_ptr()) };
    }
}

impl Bus {
    /// Create SHM arena and wait for one consumer to connect.
    pub fn listen(socket_path: &str, capacity: usize) -> Result<Self, TachyonError> {
        let path = CString::new(socket_path).map_err(|_| TachyonError::SystemError)?;
        let mut raw: *mut tachyon_bus_t = std::ptr::null_mut();

        loop {
            let err = unsafe { tachyon_bus_listen(path.as_ptr(), capacity, &mut raw) };

            if err == tachyon_error_t_TACHYON_ERR_INTERRUPTED as u32 {
                // No GIL in Rust — signal handling is the caller's responsibility.
                // Simply retry on EINTR from poll().
                continue;
            }

            from_raw(err)?;
            break;
        }

        Ok(Self {
            inner: NonNull::new(raw).ok_or(TachyonError::SystemError)?,
        })
    }

    /// Attach to an existing SHM arena via UNIX socket.
    pub fn connect(socket_path: &str) -> Result<Self, TachyonError> {
        let path = CString::new(socket_path).map_err(|_| TachyonError::SystemError)?;
        let mut raw: *mut tachyon_bus_t = std::ptr::null_mut();
        let err = unsafe { tachyon_bus_connect(path.as_ptr(), &mut raw) };
        from_raw(err)?;
        Ok(Self {
            inner: NonNull::new(raw).ok_or(TachyonError::SystemError)?,
        })
    }

    /// Bind the shared memory backing this bus to a specific NUMA node.
    ///
    /// Uses `MPOL_PREFERRED` policy — prefers the requested node but falls back
    /// rather than failing hard. Pages already allocated by `mmap(MAP_POPULATE)`
    /// are migrated via `MPOL_MF_MOVE`.
    ///
    /// **Call immediately after `listen()`/`connect()`, before the first message.**
    /// This ensures all ring buffer pages are on the desired node before the hot
    /// path begins, avoiding cross-socket cache coherence traffic on every access.
    ///
    /// No-op on non-Linux platforms (returns `Ok(())`).
    ///
    /// # Errors
    /// - `InvalidSize` if `node_id` is negative or >= 64
    /// - `SystemError` if `mbind()` fails (check `errno`: `EINVAL` = invalid node,
    ///   `EPERM` = missing `CAP_SYS_NICE`)
    pub fn set_numa_node(&self, node_id: i32) -> Result<(), TachyonError> {
        from_raw(unsafe { tachyon_bus_set_numa_node(self.inner.as_ptr(), node_id) })
    }

    /// Signal that the consumer will never sleep, skipping the futex wake check
    /// on every producer flush.
    ///
    /// When `spin_mode` is `1`, the producer omits the `atomic_thread_fence(seq_cst)`
    /// and the `consumer_sleeping` load on every `flush` call. Use only when the
    /// consumer thread is dedicated and will never park — if it parks, the
    /// producer will not issue a futex wake and the consumer will spin
    /// indefinitely instead of sleeping.
    ///
    /// **Call immediately after `listen()`/`connect()`, before the first message.**
    ///
    /// `spin_mode = 1` enables pure-spin mode, `0` restores hybrid mode.
    pub fn set_polling_mode(&self, spin_mode: i32) {
        unsafe {
            tachyon_bus_set_polling_mode(self.inner.as_ptr(), spin_mode);
        }
    }

    /// Blocking SPSC write. Copies payload, commits, and flushes.
    pub fn send(&self, data: &[u8], type_id: u32) -> Result<(), TachyonError> {
        let guard = self.acquire_tx(data.len())?;
        guard.write(data);
        guard.commit(data.len(), type_id)
    }

    /// Acquire a TX slot. Write into the guard then call commit().
    pub fn acquire_tx(&self, max_size: usize) -> Result<TxGuard<'_>, TachyonError> {
        let ptr = unsafe { tachyon_acquire_tx(self.inner.as_ptr(), max_size) };

        if ptr.is_null() {
            let state = unsafe { tachyon_get_state(self.inner.as_ptr()) };
            if state == STATE_FATAL_ERROR {
                return Err(TachyonError::PeerDead);
            }
            return Err(TachyonError::BufferFull);
        }

        Ok(TxGuard {
            bus: self,
            ptr: NonNull::new(ptr as *mut u8).unwrap(),
            max_size,
            committed: false,
        })
    }

    /// Blocking RX acquire. Spins then sleeps until a message is available.
    pub fn acquire_rx(&self, spin_threshold: u32) -> Result<RxGuard<'_>, TachyonError> {
        let mut type_id: u32 = 0;
        let mut actual_size: usize = 0;

        loop {
            let ptr = unsafe {
                tachyon_acquire_rx_blocking(
                    self.inner.as_ptr(),
                    &mut type_id,
                    &mut actual_size,
                    spin_threshold,
                )
            };

            if !ptr.is_null() {
                return Ok(RxGuard {
                    bus: self,
                    ptr: NonNull::new(ptr as *mut u8).unwrap(),
                    actual_size,
                    type_id,
                    committed: false,
                });
            }

            let state = unsafe { tachyon_get_state(self.inner.as_ptr()) };
            if state == STATE_FATAL_ERROR {
                return Err(TachyonError::PeerDead);
            }

            // nullptr + not fatal = EINTR from futex. Retry.
        }
    }

    /// Blocking batch drain. Blocks until at least 1 message is available,
    /// then drains up to `max_msgs` in a single FFI crossing.
    pub fn drain_batch(
        &self,
        max_msgs: usize,
        spin_threshold: u32,
    ) -> Result<RxBatchGuard<'_>, TachyonError> {
        let mut views = vec![
            tachyon_msg_view_t {
                ptr: std::ptr::null(),
                actual_size: 0,
                reserved_: 0,
                type_id: 0,
                padding_: 0,
            };
            max_msgs
        ]
        .into_boxed_slice();

        loop {
            let count = unsafe {
                tachyon_drain_batch(
                    self.inner.as_ptr(),
                    views.as_mut_ptr(),
                    max_msgs,
                    spin_threshold,
                )
            };

            if count > 0 {
                return Ok(RxBatchGuard {
                    bus: self,
                    views,
                    count,
                    committed: false,
                });
            }

            let state = unsafe { tachyon_get_state(self.inner.as_ptr()) };
            if state == STATE_FATAL_ERROR {
                return Err(TachyonError::PeerDead);
            }
        }
    }

    pub fn flush(&self) {
        unsafe { tachyon_flush(self.inner.as_ptr()) };
    }
}

pub struct TxGuard<'bus> {
    bus: &'bus Bus,
    ptr: NonNull<u8>,
    max_size: usize,
    committed: bool,
}

impl<'bus> TxGuard<'bus> {
    /// Copy bytes into the TX slot. Panics if `data.len() > max_size`.
    pub fn write(&self, data: &[u8]) {
        assert!(
            data.len() <= self.max_size,
            "write ({} bytes) exceeds reserved TX slot ({} bytes)",
            data.len(),
            self.max_size,
        );
        unsafe {
            std::ptr::copy_nonoverlapping(data.as_ptr(), self.ptr.as_ptr(), data.len());
        }
    }

    /// Raw mutable slice over the entire reserved TX slot — zero-copy write.
    ///
    /// # Safety
    /// Caller must not write beyond `max_size` bytes.
    pub unsafe fn as_mut_slice(&mut self) -> &mut [u8] {
        unsafe { std::slice::from_raw_parts_mut(self.ptr.as_ptr(), self.max_size) }
    }

    pub fn max_size(&self) -> usize {
        self.max_size
    }

    /// Commit the transaction and flush immediately.
    /// Use for single-message sends.
    pub fn commit(mut self, actual_size: usize, type_id: u32) -> Result<(), TachyonError> {
        self.committed = true;
        let err = unsafe { tachyon_commit_tx(self.bus.inner.as_ptr(), actual_size, type_id) };
        unsafe { tachyon_flush(self.bus.inner.as_ptr()) };
        from_raw(err)
    }

    /// Commit without flushing — use when batch-sending multiple messages.
    /// Caller MUST call `bus.flush()` after the last message to make all
    /// committed messages visible to the consumer.
    pub fn commit_unflushed(
        mut self,
        actual_size: usize,
        type_id: u32,
    ) -> Result<(), TachyonError> {
        self.committed = true;
        from_raw(unsafe { tachyon_commit_tx(self.bus.inner.as_ptr(), actual_size, type_id) })
    }
}

impl Drop for TxGuard<'_> {
    fn drop(&mut self) {
        if !self.committed {
            unsafe { tachyon_rollback_tx(self.bus.inner.as_ptr()) };
        }
    }
}

pub struct RxGuard<'bus> {
    bus: &'bus Bus,
    ptr: NonNull<u8>,
    pub actual_size: usize,
    pub type_id: u32,
    committed: bool,
}

impl<'bus> RxGuard<'bus> {
    /// Zero-copy read slice. Lifetime tied to this guard.
    pub fn data(&self) -> &[u8] {
        unsafe { std::slice::from_raw_parts(self.ptr.as_ptr(), self.actual_size) }
    }

    /// Release the slot and advance the consumer tail.
    pub fn commit(mut self) -> Result<(), TachyonError> {
        self.committed = true;
        from_raw(unsafe { tachyon_commit_rx(self.bus.inner.as_ptr()) })
    }
}

impl Drop for RxGuard<'_> {
    fn drop(&mut self) {
        if !self.committed {
            unsafe { tachyon_commit_rx(self.bus.inner.as_ptr()) };
        }
    }
}

pub struct RxBatchGuard<'bus> {
    bus: &'bus Bus,
    views: Box<[tachyon_msg_view_t]>,
    pub count: usize,
    committed: bool,
}

impl<'bus> RxBatchGuard<'bus> {
    pub fn get(&self, i: usize) -> Option<RxMsgView<'_>> {
        if i >= self.count {
            return None;
        }
        Some(RxMsgView {
            view: &self.views[i],
        })
    }

    pub fn len(&self) -> usize {
        self.count
    }

    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    pub fn iter(&self) -> BatchIter<'_> {
        BatchIter {
            views: &self.views[..self.count],
            idx: 0,
        }
    }

    /// Commit all messages in the batch and release the consumer lock.
    pub fn commit(mut self) -> Result<(), TachyonError> {
        self.committed = true;
        from_raw(unsafe {
            tachyon_commit_rx_batch(self.bus.inner.as_ptr(), self.views.as_ptr(), self.count)
        })
    }
}

impl Drop for RxBatchGuard<'_> {
    fn drop(&mut self) {
        if !self.committed {
            unsafe {
                tachyon_commit_rx_batch(self.bus.inner.as_ptr(), self.views.as_ptr(), self.count)
            };
        }
    }
}

impl<'bus> IntoIterator for &'bus RxBatchGuard<'bus> {
    type Item = RxMsgView<'bus>;
    type IntoIter = BatchIter<'bus>;

    fn into_iter(self) -> Self::IntoIter {
        BatchIter {
            views: &self.views[..self.count],
            idx: 0,
        }
    }
}

pub struct BatchIter<'batch> {
    views: &'batch [tachyon_msg_view_t],
    idx: usize,
}

impl<'batch> Iterator for BatchIter<'batch> {
    type Item = RxMsgView<'batch>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.idx >= self.views.len() {
            return None;
        }
        let view = &self.views[self.idx];
        self.idx += 1;
        Some(RxMsgView { view })
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        let remaining = self.views.len() - self.idx;
        (remaining, Some(remaining))
    }
}

impl ExactSizeIterator for BatchIter<'_> {}

pub struct RxMsgView<'batch> {
    view: &'batch tachyon_msg_view_t,
}

impl<'batch> RxMsgView<'batch> {
    /// Zero-copy slice. Lifetime `'batch` prevents access after `RxBatchGuard` commit.
    pub fn data(&self) -> &'batch [u8] {
        unsafe { std::slice::from_raw_parts(self.view.ptr.cast::<u8>(), self.view.actual_size) }
    }

    pub fn actual_size(&self) -> usize {
        self.view.actual_size
    }

    pub fn type_id(&self) -> u32 {
        self.view.type_id
    }
}
