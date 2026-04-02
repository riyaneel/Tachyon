#pragma once

#include <charconv>
#include <cstring>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <tachyon/arena.hpp>

namespace tachyon::top {
	struct BusHandle {
		pid_t		pid;
		int			fd;
		size_t		shm_size;
		std::string comm;
		ino_t		inode;
	};

	class ProcScanner {
		[[nodiscard]] __always_inline static bool is_numeric(const char *str) noexcept {
			if (!*str) [[unlikely]]
				return false;

			bool valid = true;
			for (size_t i = 0; str[i] != '\0'; ++i)
				valid &= (str[i] >= '0' && str[i] <= '9');

			return valid;
		}

		__always_inline static std::string read_comm(const pid_t pid) noexcept {
			char path_buf[64];
			std::memcpy(path_buf, "/proc/", 6);
			auto [ptr, ec] = std::to_chars(path_buf + 6, path_buf + sizeof(path_buf) - 6, pid);
			std::memcpy(ptr, "/comm\0", 6);

			const int fd = open(path_buf, O_RDONLY | O_CLOEXEC);
			if (fd < 0) [[unlikely]]
				return "";

			char	comm_buf[16];
			ssize_t n = read(fd, comm_buf, sizeof(comm_buf));
			close(fd);

			if (n > 0) [[likely]] {
				if (comm_buf[n - 1] == '\n') {
					n--;
				}

				return std::string(comm_buf, static_cast<size_t>(n));
			}

			return "";
		}

		__always_inline static void scan_pid(
			const pid_t pid, std::vector<BusHandle> &__restrict handles, std::vector<ino_t> &__restrict seen_inodes
		) noexcept {
			char path_buf[256];
			std::memcpy(path_buf, "/proc/", 6);
			auto [ptr, ec] = std::to_chars(path_buf + 6, path_buf + sizeof(path_buf) - 20, pid);
			std::memcpy(ptr, "/fd\0", 4);

			DIR *fd_dir = opendir(path_buf);
			if (!fd_dir) [[unlikely]]
				return;

			std::memcpy(ptr, "/fd/", 4);
			char			 *fd_base_name = ptr + 4;
			const std::string comm		   = read_comm(pid);

			struct dirent *fd_ent;
			while ((fd_ent = readdir(fd_dir)) != nullptr) {
				if (fd_ent->d_type != DT_LNK) [[unlikely]]
					continue;

				const size_t d_name_len = std::strlen(fd_ent->d_name);
				std::memcpy(fd_base_name, fd_ent->d_name, d_name_len + 1);

				char		  link_buf[256];
				const ssize_t link_len = readlink(path_buf, link_buf, sizeof(link_buf) - 1);
				if (link_len <= 0) [[unlikely]] {
					continue;
				}

				constexpr std::string_view MEMFD_PREFIX = "/memfd:";
				if (static_cast<size_t>(link_len) < MEMFD_PREFIX.size()) [[likely]]
					continue;

				if (std::memcmp(link_buf, MEMFD_PREFIX.data(), MEMFD_PREFIX.size()) != 0) [[likely]]
					continue;

				const int fd = open(path_buf, O_RDONLY | O_CLOEXEC);
				if (fd < 0) [[unlikely]]
					continue;

				struct stat st;
				if (fstat(fd, &st) < 0) [[unlikely]] {
					close(fd);
					continue;
				}

				if (static_cast<size_t>(st.st_size) < sizeof(tachyon::core::ArenaHeader)) [[unlikely]] {
					close(fd);
					continue;
				}

				bool seen = false;
				for (const auto seen_ino : seen_inodes) {
					seen |= (seen_ino == st.st_ino);
				}

				if (seen) [[unlikely]] {
					close(fd);
					continue;
				}

				void *map_ptr = mmap(nullptr, sizeof(tachyon::core::ArenaHeader), PROT_READ, MAP_SHARED, fd, 0);
				if (map_ptr == MAP_FAILED) [[unlikely]] {
					close(fd);
					continue;
				}

				const auto *header	  = static_cast<tachyon::core::ArenaHeader *>(map_ptr);
				const bool	valid_abi = (header->magic == tachyon::core::TACHYON_MAGIC) &
									   (header->version == tachyon::core::TACHYON_VERSION) &
									   (header->msg_alignment == TACHYON_MSG_ALIGNMENT);

				munmap(map_ptr, sizeof(tachyon::core::ArenaHeader));

				if (!valid_abi) [[likely]] {
					close(fd);
					continue;
				}

				seen_inodes.push_back(st.st_ino);
				handles.push_back(
					BusHandle{
						.pid	  = pid,
						.fd		  = fd,
						.shm_size = static_cast<size_t>(st.st_size),
						.comm	  = comm,
						.inode	  = st.st_ino
					}
				);
			}

			closedir(fd_dir);
		}

	public:
		[[nodiscard]] static std::vector<BusHandle> scan() noexcept {
			std::vector<BusHandle> handles;
			handles.reserve(16);

			DIR *proc_dir = opendir("/proc");
			if (!proc_dir) [[unlikely]]
				return handles;

			std::vector<ino_t> seen_inodes;
			seen_inodes.reserve(16);

			const pid_t	   self_pid = getpid();
			struct dirent *proc_ent;
			while ((proc_ent = readdir(proc_dir)) != nullptr) {
				if (proc_ent->d_type != DT_DIR || !is_numeric(proc_ent->d_name)) [[unlikely]]
					continue;

				pid_t pid = 0;
				std::from_chars(proc_ent->d_name, proc_ent->d_name + std::strlen(proc_ent->d_name), pid);
				if (pid == self_pid) [[unlikely]]
					continue;

				scan_pid(pid, handles, seen_inodes);
			}

			closedir(proc_dir);

			return handles;
		}
	};
} // namespace tachyon::top
