#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

namespace tachyon::core {

	/**
	 * @brief ABI contract for initial exchange
	 */
	struct TachyonHandshake {
		uint32_t magic;
		uint32_t version;
		uint32_t capacity;
		uint32_t shm_size;
		uint32_t msg_alignment;
	};

	/**
	 * @brief Transport layer specific typed errors
	 */
	enum class TransportError : uint8_t {
		SocketCreation,
		BindFailed,
		ListenFailed,
		AcceptFailed,
		ConnectFailed,
		SendFailed,
		ReceiveFailed,
		ProtocolMismatch,
		SystemError,
		Interrupted
	};

	struct ImportedShm {
		int				 fd;
		TachyonHandshake handshake;
	};

	[[nodiscard]] auto
	uds_export_shm(std::string_view socket_path, int shm_fd, const TachyonHandshake &handshake) noexcept
		-> std::expected<void, TransportError>;

	[[nodiscard]] auto uds_import_shm(std::string_view socket_path) noexcept
		-> std::expected<ImportedShm, TransportError>;
} // namespace tachyon::core
