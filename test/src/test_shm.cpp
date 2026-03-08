#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <tachyon/shm.hpp>

namespace tachyon::core::test {
	class ShmTest : public ::testing::Test {
	protected:
		const std::string test_name = "tachyon_test_memfd";
		const size_t	  test_size = 4096;
	};

	TEST_F(ShmTest, CreateSuccess) {
		const auto result = SharedMemory::create(test_name, test_size);
		ASSERT_TRUE(result.has_value());
		EXPECT_NE(result->get_ptr(), nullptr);
		EXPECT_EQ(result->get_size(), test_size);
		auto data = result->data();
		data[0]	  = std::byte{0xAA};
		EXPECT_EQ(data[0], std::byte{0xAA});
	}

	TEST_F(ShmTest, JoinViaFD) {
		const auto owner = SharedMemory::create(test_name, test_size);
		ASSERT_TRUE(owner.has_value());
		const auto guest = SharedMemory::join(owner->get_fd(), test_size);
		ASSERT_TRUE(guest.has_value());
		EXPECT_EQ(guest->get_size(), test_size);
		auto owner_data = owner->data();
		auto guest_data = guest->data();
		owner_data[0]	= std::byte{0x42};
		EXPECT_EQ(guest_data[0], std::byte{0x42});
		guest_data[1] = std::byte{0x84};
		EXPECT_EQ(owner_data[1], std::byte{0x84});
	}

	TEST_F(ShmTest, JoinInvalidFD) {
		const auto late_join = SharedMemory::join(-1, test_size);
		EXPECT_FALSE(late_join.has_value());
		EXPECT_EQ(late_join.error(), ShmError::OpenFailed);
	}

#if defined(__linux__)
	TEST_F(ShmTest, VerifySeals) {
		const auto shm = SharedMemory::create(test_name, test_size);
		ASSERT_TRUE(shm.has_value());
		const int fd = shm->get_fd();
		ASSERT_NE(fd, -1);
		EXPECT_EQ(::ftruncate(fd, static_cast<off_t>(test_size * 2)), -1);
		EXPECT_EQ(errno, EPERM);
	}
#endif

} // namespace tachyon::core::test
