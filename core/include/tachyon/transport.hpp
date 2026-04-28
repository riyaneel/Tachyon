#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

namespace tachyon::core {
	constexpr uint32_t TACHYON_FLAGS_RPC = 0x01u;

	/**
	 * @brief ABI contract for initial exchange
	 */
	struct alignas(32) TachyonHandshake {
		uint32_t magic;
		uint32_t version;
		uint32_t capacity_fwd;
		uint32_t shm_size_fwd;
		uint32_t capacity_rev;
		uint32_t shm_size_rev;
		uint32_t msg_alignment;
		uint32_t flags;
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

	struct RpcImportedShm {
		int				 fd_fwd;
		int				 fd_rev;
		TachyonHandshake handshake;
	};

	[[nodiscard]] auto
	uds_export_shm(std::string_view socket_path, int shm_fd, const TachyonHandshake &handshake) noexcept
		-> std::expected<void, TransportError>;

	[[nodiscard]] auto uds_import_shm(std::string_view socket_path) noexcept
		-> std::expected<ImportedShm, TransportError>;

	[[nodiscard]] auto
	uds_export_shm_rpc(std::string_view socket_path, int fd_fwd, int fd_rev, const TachyonHandshake &handshake) noexcept
		-> std::expected<void, TransportError>;

	[[nodiscard]] auto uds_import_shm_rpc(std::string_view socket_path) noexcept
		-> std::expected<RpcImportedShm, TransportError>;
} // namespace tachyon::core
