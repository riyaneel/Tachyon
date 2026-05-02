#pragma once

#include <tachyon.h>
#include <tachyon/shm.hpp>
#include <tachyon/transport.hpp>

using namespace tachyon::core;

TACHYON_INLINE static tachyon_error_t map_shm_error(const ShmError error) TACHYON_NOEXCEPT {
	switch (error) {
	case ShmError::OpenFailed:
		return TACHYON_ERR_OPEN;
	case ShmError::TruncateFailed:
		return TACHYON_ERR_TRUNCATE;
	case ShmError::MapFailed:
		return TACHYON_ERR_MAP;
	case ShmError::InvalidSize:
		return TACHYON_ERR_INVALID_SZ;
	case ShmError::SealFailed:
		return TACHYON_ERR_SEAL;
	case ShmError::ChmodFailed:
		return TACHYON_ERR_CHMOD;
	default:
		return TACHYON_ERR_SYSTEM;
	}
}

TACHYON_INLINE static tachyon_error_t map_transport_error(const TransportError error) TACHYON_NOEXCEPT {
	switch (error) {
	case TransportError::ProtocolMismatch:
		return TACHYON_ERR_ABI_MISMATCH;
	case TransportError::Interrupted:
		return TACHYON_ERR_INTERRUPTED;
	default:
		return TACHYON_ERR_NETWORK;
	}
}
