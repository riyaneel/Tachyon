#ifndef TACHYON_C_API_H
#define TACHYON_C_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define TACHYON_NOEXCEPT noexcept
extern "C" {
#else // #ifdef __cplusplus
#define TACHYON_NOEXCEPT
#endif // #ifdef __cplusplus #else

#if defined(_WIN32) || defined(__CYGWIN__)
#define TACHYON_ABI __declspec(dllexport)
#else // #if defined(_WIN32) || defined(__CYGWIN__)
#define TACHYON_ABI __attribute__((visibility("default")))
#endif // #if defined(_WIN32) || defined(__CYGWIN__) #else

typedef enum {
	TACHYON_SUCCESS		   = 0,
	TACHYON_ERR_NULL_PTR   = 1,
	TACHYON_ERR_MEM		   = 2,
	TACHYON_ERR_OPEN	   = 3,
	TACHYON_ERR_TRUNCATE   = 4,
	TACHYON_ERR_CHMOD	   = 5,
	TACHYON_ERR_SEAL	   = 6,
	TACHYON_ERR_MAP		   = 7,
	TACHYON_ERR_INVALID_SZ = 8,
	TACHYON_ERR_FULL	   = 9,
	TACHYON_ERR_EMPTY	   = 10,
	TACHYON_ERR_NETWORK	   = 11,
	TACHYON_ERR_SYSTEM	   = 12
} tachyon_error_t;

typedef enum {
	TACHYON_STATE_UNINITIALIZED = 0,
	TACHYON_STATE_INITIALIZING	= 1,
	TACHYON_STATE_READY			= 2,
	TACHYON_STATE_DISCONNECTED	= 3,
	TACHYON_STATE_FATAL_ERROR	= 4,
	TACHYON_STATE_UNKNOWN		= 5
} tachyon_state_t;

typedef struct tachyon_bus tachyon_bus_t;

TACHYON_ABI void tachyon_memory_barrier_acquire(void) TACHYON_NOEXCEPT;

TACHYON_ABI tachyon_error_t
tachyon_bus_listen(const char *socket_path, size_t capacity, tachyon_bus_t **out_bus) TACHYON_NOEXCEPT;

TACHYON_ABI tachyon_error_t tachyon_bus_connect(const char *socket_path, tachyon_bus_t **out_bus) TACHYON_NOEXCEPT;

TACHYON_ABI void tachyon_bus_ref(tachyon_bus_t *bus) TACHYON_NOEXCEPT;

TACHYON_ABI void tachyon_bus_destroy(tachyon_bus_t *bus) TACHYON_NOEXCEPT;

TACHYON_ABI void *tachyon_acquire_tx(tachyon_bus_t *bus, size_t max_payload_size) TACHYON_NOEXCEPT;

TACHYON_ABI tachyon_error_t
tachyon_commit_tx(tachyon_bus_t *bus, size_t actual_payload_size, uint32_t type_id) TACHYON_NOEXCEPT;

TACHYON_ABI const void *
tachyon_acquire_rx(tachyon_bus_t *bus, uint32_t *out_type_id, size_t *out_actual_size) TACHYON_NOEXCEPT;

TACHYON_ABI const void *tachyon_acquire_rx_spin(
	tachyon_bus_t *bus, uint32_t *out_type_id, size_t *out_actual_size, uint32_t max_spins
) TACHYON_NOEXCEPT;

TACHYON_ABI const void *tachyon_acquire_rx_blocking(
	tachyon_bus_t *bus, uint32_t *out_type_id, size_t *out_actual_size, uint32_t spin_threshold
) TACHYON_NOEXCEPT;

TACHYON_ABI tachyon_error_t tachyon_commit_rx(tachyon_bus_t *bus) TACHYON_NOEXCEPT;

TACHYON_ABI void tachyon_flush(tachyon_bus_t *bus) TACHYON_NOEXCEPT;

TACHYON_ABI tachyon_state_t tachyon_get_state(const tachyon_bus_t *bus) TACHYON_NOEXCEPT;

#ifdef __cplusplus
}
#endif // #ifdef __cplusplus

#endif // TACHYON_C_API_H
