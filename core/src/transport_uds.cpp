#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <tachyon/transport.hpp>

namespace tachyon::core {
	auto
	uds_export_shm(const std::string_view socket_path, const int shm_fd, const TachyonHandshake &handshake) noexcept
		-> std::expected<void, TransportError> {
		const int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
		if (sock < 0)
			return std::unexpected(TransportError::SocketCreation);

		struct sockaddr_un addr = {};
		addr.sun_family			= AF_UNIX;

		if (socket_path.length() >= sizeof(addr.sun_path)) {
			::close(sock);
			return std::unexpected(TransportError::SystemError);
		}
		std::strncpy(addr.sun_path, socket_path.data(), sizeof(addr.sun_path) - 1);

		::unlink(addr.sun_path);

		if (::bind(sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
			::close(sock);
			return std::unexpected(TransportError::BindFailed);
		}

		if (::listen(sock, 1) < 0) {
			::close(sock);
			return std::unexpected(TransportError::ListenFailed);
		}

		int client_sock = -1;
		while (true) {
			struct pollfd pfd{};
			pfd.fd	   = sock;
			pfd.events = POLLIN;

			const int ret = ::poll(&pfd, 1, 100);
			if (ret > 0) {
				client_sock = ::accept(sock, nullptr, nullptr);
				if (client_sock >= 0) {
					break;
				}
				if (errno == EINTR) {
					::close(sock);
					::unlink(addr.sun_path);
					return std::unexpected(TransportError::Interrupted);
				}
			} else if (ret == 0) {
				continue;
			} else {
				if (errno == EINTR) {
					::close(sock);
					::unlink(addr.sun_path);
					return std::unexpected(TransportError::Interrupted);
				}
				::close(sock);
				::unlink(addr.sun_path);
				return std::unexpected(TransportError::AcceptFailed);
			}
		}

		struct msghdr msg{};
		char		  buf[CMSG_SPACE(sizeof(int))] = {};

		struct iovec io = {const_cast<void *>(reinterpret_cast<const void *>(&handshake)), sizeof(TachyonHandshake)};

		msg.msg_iov		   = &io;
		msg.msg_iovlen	   = 1;
		msg.msg_control	   = buf;
		msg.msg_controllen = sizeof(buf);

		struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
		if (!cmsg) {
			::close(client_sock);
			::close(sock);
			::unlink(addr.sun_path);
			return std::unexpected(TransportError::SystemError);
		}

		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type	 = SCM_RIGHTS;
		cmsg->cmsg_len	 = CMSG_LEN(sizeof(int));

		std::memcpy(CMSG_DATA(cmsg), &shm_fd, sizeof(int));

		if (::sendmsg(client_sock, &msg, 0) < 0) {
			::close(client_sock);
			::close(sock);
			::unlink(addr.sun_path);
			return std::unexpected(TransportError::SendFailed);
		}

		::close(client_sock);
		::close(sock);
		::unlink(addr.sun_path);

		return {};
	}

	auto uds_import_shm(const std::string_view socket_path) noexcept -> std::expected<ImportedShm, TransportError> {
		const int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
		if (sock < 0)
			return std::unexpected(TransportError::SocketCreation);

		struct sockaddr_un addr = {};
		addr.sun_family			= AF_UNIX;

		if (socket_path.length() >= sizeof(addr.sun_path)) {
			::close(sock);
			return std::unexpected(TransportError::SystemError);
		}
		std::strncpy(addr.sun_path, socket_path.data(), sizeof(addr.sun_path) - 1);

		if (::connect(sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
			::close(sock);
			return std::unexpected(TransportError::ConnectFailed);
		}

		struct msghdr	 msg{};
		TachyonHandshake hs{};
		struct iovec	 io = {&hs, sizeof(hs)};
		msg.msg_iov			= &io;
		msg.msg_iovlen		= 1;

		char c_buffer[CMSG_SPACE(sizeof(int))] = {};
		msg.msg_control						   = c_buffer;
		msg.msg_controllen					   = sizeof(c_buffer);
		if (::recvmsg(sock, &msg, 0) < static_cast<ssize_t>(sizeof(hs))) {
			::close(sock);
			return std::unexpected(TransportError::ReceiveFailed);
		}

		const struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
		if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) {
			::close(sock);
			return std::unexpected(TransportError::ReceiveFailed);
		}

		int received_fd = -1;
		std::memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(int));
		::close(sock);
		if (received_fd < 0) {
			return std::unexpected(TransportError::SystemError);
		}

		return ImportedShm{received_fd, hs};
	}

	auto uds_export_shm_rpc(
		const std::string_view socket_path, const int fd_fwd, const int fd_rev, const TachyonHandshake &handshake
	) noexcept -> std::expected<void, TransportError> {
		const int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
		if (sock < 0)
			return std::unexpected(TransportError::SocketCreation);

		struct sockaddr_un addr = {};
		addr.sun_family			= AF_UNIX;

		if (socket_path.length() >= sizeof(addr.sun_path)) {
			::close(sock);
			return std::unexpected(TransportError::SystemError);
		}
		std::strncpy(addr.sun_path, socket_path.data(), sizeof(addr.sun_path) - 1);

		::unlink(addr.sun_path);

		if (::bind(sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
			::close(sock);
			return std::unexpected(TransportError::BindFailed);
		}

		if (::listen(sock, 1) < 0) {
			::close(sock);
			return std::unexpected(TransportError::ListenFailed);
		}

		int client_sock = -1;
		while (true) {
			struct pollfd pfd{};
			pfd.fd	   = sock;
			pfd.events = POLLIN;

			const int ret = ::poll(&pfd, 1, 100);
			if (ret > 0) {
				client_sock = ::accept(sock, nullptr, nullptr);
				if (client_sock >= 0)
					break;
				if (errno == EINTR) {
					::close(sock);
					::unlink(addr.sun_path);
					return std::unexpected(TransportError::Interrupted);
				}
			} else if (ret == 0) {
				continue;
			} else {
				if (errno == EINTR) {
					::close(sock);
					::unlink(addr.sun_path);
					return std::unexpected(TransportError::Interrupted);
				}
				::close(sock);
				::unlink(addr.sun_path);
				return std::unexpected(TransportError::AcceptFailed);
			}
		}

		struct msghdr msg{};
		char		  buf[CMSG_SPACE(2 * sizeof(int))] = {};

		struct iovec io	   = {const_cast<void *>(reinterpret_cast<const void *>(&handshake)), sizeof(TachyonHandshake)};
		msg.msg_iov		   = &io;
		msg.msg_iovlen	   = 1;
		msg.msg_control	   = buf;
		msg.msg_controllen = sizeof(buf);

		struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
		if (!cmsg) {
			::close(client_sock);
			::close(sock);
			::unlink(addr.sun_path);
			return std::unexpected(TransportError::SystemError);
		}

		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type	 = SCM_RIGHTS;
		cmsg->cmsg_len	 = CMSG_LEN(2 * sizeof(int));

		const int fds[2] = {fd_fwd, fd_rev};
		std::memcpy(CMSG_DATA(cmsg), fds, 2 * sizeof(int));

		if (::sendmsg(client_sock, &msg, 0) < 0) {
			::close(client_sock);
			::close(sock);
			::unlink(addr.sun_path);
			return std::unexpected(TransportError::SendFailed);
		}

		::close(client_sock);
		::close(sock);
		::unlink(addr.sun_path);

		return {};
	}

	auto uds_import_shm_rpc(const std::string_view socket_path) noexcept
		-> std::expected<RpcImportedShm, TransportError> {
		const int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
		if (sock < 0)
			return std::unexpected(TransportError::SocketCreation);

		struct sockaddr_un addr = {};
		addr.sun_family			= AF_UNIX;

		if (socket_path.length() >= sizeof(addr.sun_path)) {
			::close(sock);
			return std::unexpected(TransportError::SystemError);
		}
		std::strncpy(addr.sun_path, socket_path.data(), sizeof(addr.sun_path) - 1);

		if (::connect(sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
			::close(sock);
			return std::unexpected(TransportError::ConnectFailed);
		}

		struct msghdr	 msg{};
		TachyonHandshake hs{};
		struct iovec	 io = {&hs, sizeof(hs)};
		msg.msg_iov			= &io;
		msg.msg_iovlen		= 1;

		char c_buffer[CMSG_SPACE(2 * sizeof(int))] = {};
		msg.msg_control							   = c_buffer;
		msg.msg_controllen						   = sizeof(c_buffer);

		if (::recvmsg(sock, &msg, 0) < static_cast<ssize_t>(sizeof(hs))) {
			::close(sock);
			return std::unexpected(TransportError::ReceiveFailed);
		}

		const struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
		if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len < CMSG_LEN(2 * sizeof(int))) {
			::close(sock);
			return std::unexpected(TransportError::ReceiveFailed);
		}

		int fds[2] = {-1, -1};
		std::memcpy(fds, CMSG_DATA(cmsg), 2 * sizeof(int));
		::close(sock);

		if (fds[0] < 0 || fds[1] < 0) {
			if (fds[0] >= 0)
				::close(fds[0]);
			if (fds[1] >= 0)
				::close(fds[1]);
			return std::unexpected(TransportError::SystemError);
		}

		return RpcImportedShm{fds[0], fds[1], hs};
	}
} // namespace tachyon::core
