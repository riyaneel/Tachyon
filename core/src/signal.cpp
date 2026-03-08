#include <csignal>

#include <tachyon/signal.hpp>

namespace tachyon::core {
	std::atomic<bool> SignalHandler::terminate_flag_{false};

	extern "C" void signal_callback(const int) {
		SignalHandler::trigger();
	}

	void SignalHandler::setup() noexcept {
		struct ::sigaction sig{};
		sig.sa_handler = signal_callback;
		::sigemptyset(&sig.sa_mask);
		sig.sa_flags = 0;
		::sigaction(SIGINT, &sig, nullptr);
		::sigaction(SIGTERM, &sig, nullptr);
	}
} // namespace tachyon::core
