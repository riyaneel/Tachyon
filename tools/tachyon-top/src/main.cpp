#include <atomic>
#include <chrono>
#include <format>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <getopt.h>
#include <unistd.h>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include "bus_view.hpp"
#include "proc_scanner.hpp"
#include "ui.hpp"

namespace {
	[[nodiscard]] double calibrate_tsc() noexcept {
		using namespace std::chrono;
		const auto	   start_wall = steady_clock::now();
		const uint64_t start_tsc  = tachyon::rdtsc();
		std::this_thread::sleep_for(milliseconds(10));

		const uint64_t end_tsc	= tachyon::rdtsc();
		const auto	   end_wall = steady_clock::now();
		const auto	   delta_us = duration_cast<microseconds>(end_wall - start_wall).count();
		return static_cast<double>(end_tsc - start_tsc) / static_cast<double>(delta_us);
	}

	void dump_json_and_exit(const double tsc_ticks_per_us) {
		auto handles = tachyon::top::ProcScanner::scan();

		std::cout << "[\n";
		for (size_t i = 0; i < handles.size(); ++i) {
			tachyon::top::BusView view(std::move(handles[i]), tsc_ticks_per_us);
			const auto			  data = view.sample();

			std::cout << std::format(
				R"(  {{"pid":{},"comm":"{}","capacity":{},"used":{},"fill_pct":{:.2f},"state":{}}})",
				data.pid,
				data.comm,
				data.capacity,
				data.used_bytes,
				data.fill_pct,
				static_cast<uint32_t>(data.state)
			);

			if (i < handles.size() - 1)
				std::cout << ",\n";
		}
		std::cout << "\n]\n";
		exit(0);
	}
} // namespace

int main(const int argc, char **argv) {
	int	 interval_ms_val = 100;
	bool one_shot_json	 = false;

	static struct option long_options[] = {
		{"interval", required_argument, nullptr, 'i'}, {"json", no_argument, nullptr, 'j'}, {nullptr, 0, nullptr, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "i:j", long_options, nullptr)) != -1) {
		switch (opt) {
		case 'i':
			interval_ms_val = std::stoi(optarg);
			break;
		case 'j':
			one_shot_json = true;
			break;
		default:
			std::cerr << "Usage: tachyon_top [--interval <ms>] [--json]\n";
			return 1;
		}
	}

	if (interval_ms_val < 50) {
		interval_ms_val = 50;
	}

	const double tsc_ticks_per_us = calibrate_tsc();

	if (one_shot_json) {
		dump_json_and_exit(tsc_ticks_per_us);
	}

	tachyon::top::UIDataState ui_state;
	std::atomic				  running{true};
	std::atomic				  shared_interval_ms{interval_ms_val};

	auto screen = ftxui::ScreenInteractive::Fullscreen();

	std::jthread poller_thread([&]() {
		std::vector<tachyon::top::BusView> active_views;
		active_views.reserve(16);

		std::vector<tachyon::top::BusUIData> ui_data;
		ui_data.reserve(16);

		while (running.load(std::memory_order_acquire)) {
			auto new_handles = tachyon::top::ProcScanner::scan();
			std::erase_if(active_views, [&new_handles](const tachyon::top::BusView &v) {
				for (const auto &h : new_handles) {
					if (h.inode == v.handle().inode) {
						return false;
					}
				}
				return true;
			});

			for (auto &handle : new_handles) {
				bool exists = false;
				for (const auto &view : active_views) {
					if (view.handle().inode == handle.inode) {
						exists = true;
						break;
					}
				}

				if (!exists) {
					active_views.emplace_back(std::move(handle), tsc_ticks_per_us);
				}
			}

			ui_data.clear();
			for (auto &view : active_views) {
				ui_data.push_back(view.sample());
			}

			ui_state.commit_render_state(ui_data);
			screen.PostEvent(ftxui::Event::Custom);
			std::this_thread::sleep_for(std::chrono::milliseconds(shared_interval_ms.load(std::memory_order_relaxed)));
		}
	});

	size_t selected_idx = 0;

	auto renderer = ftxui::Renderer([&] {
		const auto &views = ui_state.get_render_state();
		if (!views.empty() && selected_idx >= views.size()) {
			selected_idx = views.size() - 1;
		} else if (views.empty()) {
			selected_idx = 0;
		}

		ftxui::Element body = tachyon::top::ui::render_table(views, selected_idx);
		if (!views.empty()) {
			body = ftxui::vbox({body, ftxui::separator(), tachyon::top::ui::render_detail(views[selected_idx])});
		}

		return ftxui::vbox({tachyon::top::ui::render_header(shared_interval_ms.load(std::memory_order_relaxed)), body});
	});

	renderer |= ftxui::CatchEvent([&](const ftxui::Event &event) {
		const auto &views = ui_state.get_render_state();

		if (event == ftxui::Event::Character('q') || event == ftxui::Event::Escape) {
			screen.Exit();
			return true;
		}

		if (event == ftxui::Event::ArrowDown || event == ftxui::Event::Character('j')) {
			if (!views.empty() && selected_idx < views.size() - 1) {
				selected_idx++;
			}
			return true;
		}

		if (event == ftxui::Event::ArrowUp || event == ftxui::Event::Character('k')) {
			if (selected_idx > 0) {
				selected_idx--;
			}
			return true;
		}

		if (event == ftxui::Event::Character('r')) {
			const int current = shared_interval_ms.load(std::memory_order_relaxed);
			if (current == 100)
				shared_interval_ms.store(500, std::memory_order_relaxed);
			else if (current == 500)
				shared_interval_ms.store(1000, std::memory_order_relaxed);
			else
				shared_interval_ms.store(100, std::memory_order_relaxed);
			return true;
		}

		return false;
	});

	screen.Loop(renderer);
	running.store(false, std::memory_order_release);

	return 0;
}
