#include <fcntl.h>
#include <sys/mman.h>

#include <gtest/gtest.h>

#include <tachyon/shm.hpp>

namespace tachyon::core::test {
	class ShmTest : public ::testing::Test {
	protected:
		const std::string test_name = "/tachyon_test_shm";
		const size_t	  test_size = 4096;

		void TearDown() override {
			::shm_unlink(test_name.c_str());
		}
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

	TEST_F(ShmTest, ExclusiveCreate) {
		const auto first = SharedMemory::create(test_name, test_size);
		ASSERT_TRUE(first.has_value());
		auto second = SharedMemory::create(test_name, test_size);
		EXPECT_FALSE(second.has_value());
		EXPECT_EQ(second.error(), ShmError::OpenFailed);
	}

	TEST_F(ShmTest, JoinSuccess) {
		{
			const auto owner = SharedMemory::create(test_name, test_size);
			ASSERT_TRUE(owner.has_value());
			const auto guest = SharedMemory::join(test_name, test_size);
			ASSERT_TRUE(guest.has_value());
			EXPECT_EQ(guest->get_size(), test_size);
		}

		const auto late_join = SharedMemory::join(test_name, test_size);
		EXPECT_FALSE(late_join.has_value());
	}

#if defined(__linux__)
	TEST_F(ShmTest, VerifySeals) {
		const auto shm = SharedMemory::create(test_name, test_size);
		ASSERT_TRUE(shm.has_value());
		const int fd = ::shm_open(test_name.c_str(), O_RDWR, 0600);
		ASSERT_NE(fd, -1);
		EXPECT_EQ(::ftruncate(fd, static_cast<off_t>(test_size * 2)), -1);
		::close(fd);
	}
#endif

} // namespace tachyon::core::test
