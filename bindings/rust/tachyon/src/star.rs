use std::ptr::NonNull;

use tachyon_sys::{
    tachyon_msg_view_t, tachyon_star_acquire_tx, tachyon_star_commit, tachyon_star_commit_tx,
    tachyon_star_destroy, tachyon_star_flush, tachyon_star_get_state, tachyon_star_n_spokes,
    tachyon_star_poll, tachyon_star_rollback_tx, tachyon_star_t,
    tachyon_state_t_TACHYON_STATE_UNKNOWN,
};

use crate::Bus;
use crate::error::{TachyonError, from_raw};

pub struct StarBus {
    ptr: NonNull<tachyon_star_t>,
}

unsafe impl Send for StarBus {}

impl Drop for StarBus {
    fn drop(&mut self) {
        unsafe {
            tachyon_star_destroy(self.ptr.as_ptr());
        }
    }
}

impl StarBus {
    /// Create a star from `buses`.
    ///
    /// `node_ids`, if provided, must have the same length as `buses`.
    /// A negative value at index `i` means "no NUMA binding for spoke i".
    ///
    /// Internally calls `tachyon_bus_ref` on every bus before returning,
    /// so the caller's `Bus` handles may be dropped independently.
    pub fn create(buses: &[&Bus], node_ids: Option<&[i32]>) -> Result<Self, TachyonError> {
        if let Some(ids) = node_ids {
            if ids.len() != buses.len() {
                return Err(TachyonError::InvalidSize);
            }
        }

        let mut raw: Vec<*mut tachyon_sys::tachyon_bus_t> =
            buses.iter().map(|b| b.as_ptr()).collect();
        let node_ids_ptr = node_ids.map_or(std::ptr::null(), |ids| ids.as_ptr());
        let mut out = std::ptr::null_mut();
        let err = unsafe {
            tachyon_sys::tachyon_star_create(raw.as_mut_ptr(), raw.len(), node_ids_ptr, &mut out)
        };

        from_raw(err)?;
        Ok(Self {
            ptr: NonNull::new(out).expect("tachyon_star_create succeeded but returned null"),
        })
    }

    /// Drain up to `max_total` messages across all spokes in a round-robin.
    ///
    /// Returns a [`StarPollGuard`] that borrows `self`.  The guard commits
    /// (advances all spoke tails) when dropped, so callers must finish
    /// reading message data before dropping it.
    pub fn poll(&self, max_total: usize, budget_us: u64) -> StarPollGuard<'_> {
        let mut views: Vec<tachyon_msg_view_t> = (0..max_total)
            .map(|_| unsafe { std::mem::zeroed() })
            .collect();
        let mut spoke_indices: Vec<usize> = vec![0; max_total];

        let n = unsafe {
            tachyon_star_poll(
                self.ptr.as_ptr(),
                views.as_mut_ptr(),
                max_total,
                budget_us,
                spoke_indices.as_mut_ptr(),
            )
        };

        views.truncate(n);
        spoke_indices.truncate(n);

        StarPollGuard {
            star: self,
            views,
            spoke_indices,
            committed: false,
        }
    }

    /// Acquire a TX slot on `spoke_idx`.
    ///
    /// Returns `None` if the ring is full or `spoke_idx` is out of bounds.
    /// The returned [`StarTxGuard`] rolls back the slot on drop if not committed.
    pub fn acquire_tx(&self, spoke_idx: usize, max_size: usize) -> Option<StarTxGuard<'_>> {
        let raw = unsafe { tachyon_star_acquire_tx(self.ptr.as_ptr(), spoke_idx, max_size) };
        NonNull::new(raw.cast::<u8>()).map(|ptr| StarTxGuard {
            star: self,
            spoke_idx,
            ptr,
            max_size,
            committed: false,
        })
    }

    /// Flush pending TX data on `spoke_idx` to the consumer.
    pub fn flush(&self, spoke_idx: usize) {
        unsafe { tachyon_star_flush(self.ptr.as_ptr(), spoke_idx) }
    }

    /// Number of spokes.
    pub fn n_spokes(&self) -> usize {
        unsafe { tachyon_star_n_spokes(self.ptr.as_ptr()) }
    }

    /// State of spoke `spoke_idx`.  Returns `UNKNOWN` if out of bounds.
    pub fn get_state(&self, spoke_idx: usize) -> tachyon_sys::tachyon_state_t {
        if spoke_idx >= self.n_spokes() {
            return tachyon_state_t_TACHYON_STATE_UNKNOWN;
        }
        unsafe { tachyon_star_get_state(self.ptr.as_ptr(), spoke_idx) }
    }
}

/// RAII handle returned by [`StarBus::poll`].
///
/// Holds the views and spoke indices for the current poll batch.
/// Commits (advances all spoke ring-buffer tails) when dropped.
/// Call [`commit`](StarPollGuard::commit) explicitly to handle errors.
pub struct StarPollGuard<'a> {
    star: &'a StarBus,
    views: Vec<tachyon_msg_view_t>,
    spoke_indices: Vec<usize>,
    committed: bool,
}

impl StarPollGuard<'_> {
    /// Number of messages received.
    pub fn len(&self) -> usize {
        self.views.len()
    }

    /// True if no messages were received.
    pub fn is_empty(&self) -> bool {
        self.views.is_empty()
    }

    /// Iterate over received messages, each paired with its spoke index.
    pub fn iter(&self) -> impl Iterator<Item = StarMsgView<'_>> {
        self.views
            .iter()
            .zip(self.spoke_indices.iter())
            .map(|(v, &s)| StarMsgView {
                view: v,
                spoke_idx: s,
            })
    }

    /// Advance all spoke ring-buffer tails (release slots to producers).
    ///
    /// Must be called after the caller has finished reading all message data,
    /// the pointers inside each [`StarMsgView`] point into the ring buffer.
    pub fn commit(mut self) -> Result<(), TachyonError> {
        self.committed = true;
        from_raw(unsafe { tachyon_star_commit(self.star.ptr.as_ptr()) })
    }
}

impl Drop for StarPollGuard<'_> {
    fn drop(&mut self) {
        if !self.committed {
            unsafe { tachyon_star_commit(self.star.ptr.as_ptr()) };
        }
    }
}

/// A single message from a poll batch, with its originating spoke index.
pub struct StarMsgView<'a> {
    view: &'a tachyon_msg_view_t,
    spoke_idx: usize,
}

impl StarMsgView<'_> {
    /// Zero-copy slice into the ring buffer payload.
    ///
    /// Valid only until the parent [`StarPollGuard`] is committed or dropped.
    pub fn data(&self) -> &[u8] {
        if self.view.ptr.is_null() || self.view.actual_size == 0 {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.view.ptr.cast::<u8>(), self.view.actual_size) }
    }

    /// Raw type_id (use `route_id()` / `msg_type()` to decode).
    pub fn type_id(&self) -> u32 {
        self.view.type_id
    }

    /// Spoke (bus) index this message was received on.
    pub fn spoke_idx(&self) -> usize {
        self.spoke_idx
    }

    /// Actual payload size in bytes.
    pub fn actual_size(&self) -> usize {
        self.view.actual_size
    }
}

/// RAII TX slot returned by [`StarBus::acquire_tx`].
///
/// Rolls back the slot (releases it without publishing) on drop if
/// [`commit`](StarTxGuard::commit) was not called.
pub struct StarTxGuard<'a> {
    star: &'a StarBus,
    spoke_idx: usize,
    ptr: NonNull<u8>,
    max_size: usize,
    committed: bool,
}

impl StarTxGuard<'_> {
    /// Copy `data` into the slot.  Panics if `data.len() > max_size`.
    pub fn write(&self, data: &[u8]) {
        assert!(
            data.len() <= self.max_size,
            "write: data ({}) exceeds max_size ({})",
            data.len(),
            self.max_size
        );
        unsafe { std::ptr::copy_nonoverlapping(data.as_ptr(), self.ptr.as_ptr(), data.len()) }
    }

    /// Mutable slice over the full reserved slot (length = `max_size`).
    pub fn as_mut_slice(&mut self) -> &mut [u8] {
        unsafe { std::slice::from_raw_parts_mut(self.ptr.as_ptr(), self.max_size) }
    }

    /// Publish `actual_size` bytes with `type_id` to the spoke.
    /// Flushes immediately (consumer-visible after this call).
    pub fn commit(mut self, actual_size: usize, type_id: u32) -> Result<(), TachyonError> {
        assert!(actual_size <= self.max_size);
        self.committed = true;
        from_raw(unsafe {
            tachyon_star_commit_tx(self.star.ptr.as_ptr(), self.spoke_idx, actual_size, type_id)
        })
    }
}

impl Drop for StarTxGuard<'_> {
    fn drop(&mut self) {
        if !self.committed {
            unsafe { tachyon_star_rollback_tx(self.star.ptr.as_ptr(), self.spoke_idx) };
        }
    }
}
