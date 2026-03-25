use tachyon_sys::{
    tachyon_error_t_TACHYON_ERR_ABI_MISMATCH as ERR_ABI_MISMATCH,
    tachyon_error_t_TACHYON_ERR_CHMOD as ERR_CHMOD, tachyon_error_t_TACHYON_ERR_EMPTY as ERR_EMPTY,
    tachyon_error_t_TACHYON_ERR_FULL as ERR_FULL,
    tachyon_error_t_TACHYON_ERR_INTERRUPTED as ERR_INTERRUPTED,
    tachyon_error_t_TACHYON_ERR_INVALID_SZ as ERR_INVALID_SZ,
    tachyon_error_t_TACHYON_ERR_MAP as ERR_MAP, tachyon_error_t_TACHYON_ERR_MEM as ERR_MEM,
    tachyon_error_t_TACHYON_ERR_NETWORK as ERR_NETWORK,
    tachyon_error_t_TACHYON_ERR_NULL_PTR as ERR_NULL_PTR,
    tachyon_error_t_TACHYON_ERR_OPEN as ERR_OPEN, tachyon_error_t_TACHYON_ERR_SEAL as ERR_SEAL,
    tachyon_error_t_TACHYON_ERR_SYSTEM as ERR_SYSTEM,
    tachyon_error_t_TACHYON_ERR_TRUNCATE as ERR_TRUNCATE,
    tachyon_error_t_TACHYON_SUCCESS as SUCCESS,
};

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TachyonError {
    NullPtr,
    OutOfMemory,
    OpenFailed,
    TruncateFailed,
    ChmodFailed,
    SealFailed,
    MapFailed,
    InvalidSize,
    BufferFull,
    BufferEmpty,
    NetworkError,
    SystemError,
    Interrupted,
    AbiMismatch,
    PeerDead,
    Unknown(u32),
}

impl std::fmt::Display for TachyonError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::NullPtr => write!(f, "Internal null pointer"),
            Self::OutOfMemory => write!(f, "Out of memory"),
            Self::OpenFailed => write!(f, "Failed to open shared memory fd"),
            Self::TruncateFailed => write!(f, "Failed to truncate shared memory"),
            Self::ChmodFailed => write!(f, "Failed to set shared memory permissions"),
            Self::SealFailed => write!(f, "Failed to apply fcntl seals"),
            Self::MapFailed => write!(f, "mmap() failed"),
            Self::InvalidSize => write!(f, "Capacity must be a strictly positive power of two"),
            Self::BufferFull => write!(f, "Ring buffer full (anti-overwrite shield)"),
            Self::BufferEmpty => write!(f, "No messages available"),
            Self::NetworkError => write!(f, "UNIX socket error"),
            Self::SystemError => write!(f, "OS error"),
            Self::Interrupted => write!(f, "Interrupted by signal"),
            Self::AbiMismatch => write!(
                f,
                "ABI mismatch: rebuild both producer and consumer from the same Tachyon version"
            ),
            Self::PeerDead => write!(f, "Peer process dead or unresponsive"),
            Self::Unknown(c) => write!(f, "Unknown error code: {c}"),
        }
    }
}

impl std::error::Error for TachyonError {}

pub(crate) fn from_raw(code: u32) -> Result<(), TachyonError> {
    match code {
        c if c == SUCCESS => Ok(()),
        c if c == ERR_NULL_PTR => Err(TachyonError::NullPtr),
        c if c == ERR_MEM => Err(TachyonError::OutOfMemory),
        c if c == ERR_OPEN => Err(TachyonError::OpenFailed),
        c if c == ERR_TRUNCATE => Err(TachyonError::TruncateFailed),
        c if c == ERR_CHMOD => Err(TachyonError::ChmodFailed),
        c if c == ERR_SEAL => Err(TachyonError::SealFailed),
        c if c == ERR_MAP => Err(TachyonError::MapFailed),
        c if c == ERR_INVALID_SZ => Err(TachyonError::InvalidSize),
        c if c == ERR_FULL => Err(TachyonError::BufferFull),
        c if c == ERR_EMPTY => Err(TachyonError::BufferEmpty),
        c if c == ERR_NETWORK => Err(TachyonError::NetworkError),
        c if c == ERR_SYSTEM => Err(TachyonError::SystemError),
        c if c == ERR_INTERRUPTED => Err(TachyonError::Interrupted),
        c if c == ERR_ABI_MISMATCH => Err(TachyonError::AbiMismatch),
        c => Err(TachyonError::Unknown(c)),
    }
}
