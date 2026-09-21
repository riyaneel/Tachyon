#include <filesystem>
#include <fstream>
#include <vector>

#include <gtest/gtest.h>

extern "C" {
int tachyon_replay_arena_rx(const uint8_t *data, size_t size);
int tachyon_replay_arena_rx_batch(const uint8_t *data, size_t size);
int tachyon_replay_arena_tx(const uint8_t *data, size_t size);
int tachyon_replay_header_parser(const uint8_t *data, size_t size);
int tachyon_replay_toctou(const uint8_t *data, size_t size);
}

namespace tachyon::core::test {
	namespace {
		using Harness = int (*)(const uint8_t *, size_t);

		void replay(const char *name, const Harness harness) {
			const std::filesystem::path dir = std::filesystem::path(TACHYON_CORPUS_DIR) / name;
			if (!std::filesystem::is_directory(dir)) {
				GTEST_SKIP() << "no corpus at " << dir;
			}

			size_t replayed = 0;
			for (const auto &entry : std::filesystem::directory_iterator(dir)) {
				if (!entry.is_regular_file()) {
					continue;
				}

				std::ifstream file(entry.path(), std::ios::binary);
				ASSERT_TRUE(file) << "cannot read " << entry.path();
				const std::vector<uint8_t> input((std::istreambuf_iterator(file)), std::istreambuf_iterator<char>());

				SCOPED_TRACE(entry.path().string());
				harness(input.data(), input.size());
				++replayed;
			}

			EXPECT_GT(replayed, size_t{0}) << "corpus at " << dir << " is empty";
		}
	} // namespace

	TEST(FuzzReplay, ArenaRx) {
		replay("arena_rx", &tachyon_replay_arena_rx);
	}

	TEST(FuzzReplay, ArenaRxBatch) {
		replay("arena_rx_batch", &tachyon_replay_arena_rx_batch);
	}

	TEST(FuzzReplay, ArenaTx) {
		replay("arena_tx", &tachyon_replay_arena_tx);
	}

	TEST(FuzzReplay, HeaderParser) {
		replay("header_parser", &tachyon_replay_header_parser);
	}

	TEST(FuzzReplay, Toctou) {
		replay("toctou", &tachyon_replay_toctou);
	}
} // namespace tachyon::core::test
