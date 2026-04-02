#pragma once

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include <unistd.h>

#include <ftxui/dom/canvas.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

namespace tachyon::top::ui {
	[[nodiscard]] constexpr const char *to_string(const core::BusState state) noexcept {
		using enum core::BusState;
		switch (state) {
		case Uninitialized:
			return "UNINIT";
		case Initializing:
			return "INIT";
		case Ready:
			return "READY";
		case Disconnected:
			return "DISCONN";
		case FatalError:
			return "FATAL";
		case Unknown:
			return "UNKNOWN";
		default:
			return "???";
		}
	}

	[[nodiscard]] inline ftxui::Element render_header(const int interval_ms) {
		using namespace ftxui;
		return vbox(
			{hbox(
				 {text(" TACHYON-TOP") | bold | inverted,
				  filler(),
				  text(std::format(" Scanner PID: {} ", getpid())),
				  text(std::format(" Cycle: {}ms ", interval_ms)) | dim}
			 ),
			 text(" [q/ESC] Quit | [↑/↓] Select | [r] Cycle Interval (100/500/1000) ") | dim,
			 separator()}
		);
	}

	[[nodiscard]] inline ftxui::Element render_fill_bar(const double pct) {
		using namespace ftxui;

		const int filled = std::clamp(static_cast<int>((pct / 100.0) * 17.0), 0, 17);
		const int empty	 = 17 - filled;

		const std::string bar_str = std::format(
			"[{}{}]", std::string(static_cast<size_t>(filled), '|'), std::string(static_cast<size_t>(empty), ' ')
		);
		const auto elem = text(bar_str);

		if (pct > 80.0)
			return elem | color(Color::Red);
		if (pct > 50.0)
			return elem | color(Color::Yellow);
		return elem | color(Color::Green);
	}

	[[nodiscard]] inline ftxui::Element render_table(const std::vector<BusUIData> &views, const size_t selected_idx) {
		using namespace ftxui;

		if (views.empty()) [[unlikely]] {
			return text("no buses found") | center | flex;
		}

		Elements rows;
		rows.reserve(views.size() + 2);

		rows.push_back(
			hbox(
				{text(" PROCESS/PID ") | size(WIDTH, EQUAL, 20),
				 text(" STATE ") | size(WIDTH, EQUAL, 10),
				 text(" FILL ") | size(WIDTH, EQUAL, 20),
				 text(" MSG/S ") | size(WIDTH, EQUAL, 12),
				 text(" MB/S ") | size(WIDTH, EQUAL, 10),
				 text(" CONSUMER ") | size(WIDTH, EQUAL, 15)}
			) |
			bold
		);

		rows.push_back(separator());

		for (size_t i = 0; i < views.size(); ++i) {
			const auto &v			= views[i];
			const bool	is_selected = (i == selected_idx);
			const bool	is_fatal	= (v.state == core::BusState::FatalError);

			Element row;
			if (is_fatal) [[unlikely]] {
				row = hbox(
						  {text(std::format(" {}/{} ", v.comm, v.pid)) | size(WIDTH, EQUAL, 20),
						   text(" FATAL_ERR ") | size(WIDTH, EQUAL, 10),
						   text(" ??? ") | size(WIDTH, EQUAL, 20),
						   text(" ??? ") | size(WIDTH, EQUAL, 12),
						   text(" ??? ") | size(WIDTH, EQUAL, 10),
						   text(" ??? ") | size(WIDTH, EQUAL, 15)}
					  ) |
					  color(Color::Red) | bold;
			} else [[likely]] {
				row = hbox(
					{text(std::format(" {}/{} ", v.comm, v.pid)) | size(WIDTH, EQUAL, 20),
					 text(std::format(" {} ", to_string(v.state))) | size(WIDTH, EQUAL, 10),
					 render_fill_bar(v.fill_pct) | size(WIDTH, EQUAL, 20),
					 text(std::format(" {:.1f} ", v.msg_per_sec)) | size(WIDTH, EQUAL, 12),
					 text(std::format(" {:.2f} ", v.mb_per_sec)) | size(WIDTH, EQUAL, 10),
					 text(v.consumer_sleeping ? " SLEEPING" : " AWAKE") | size(WIDTH, EQUAL, 15)}
				);
			}

			if (is_selected) {
				row = row | inverted;
			}
			rows.push_back(row);
		}

		return vbox(std::move(rows));
	}

	[[nodiscard]] inline ftxui::Element render_detail(const BusUIData &view) {
		using namespace ftxui;

		constexpr int CANVAS_W = 240;
		constexpr int CANVAS_H = 40;
		auto		  c		   = Canvas(CANVAS_W, CANVAS_H);

		double max_val = 0.001;
		for (const double val : view.sparkline) {
			max_val = std::max(max_val, val);
		}

		for (size_t i = 0; i < 60; ++i) {
			const double val	   = view.sparkline[i];
			const int	 base_x	   = static_cast<int>(i) * 4;
			const int	 y		   = (CANVAS_H - 1) - static_cast<int>((val / max_val) * (CANVAS_H - 1));
			const int	 clamped_y = std::clamp(y, 0, CANVAS_H - 1);

			for (int dx = 0; dx < 4; ++dx) {
				const int x = base_x + dx;
				c.DrawPoint(x, clamped_y, true);
				for (int fill_y = clamped_y + 1; fill_y < CANVAS_H; ++fill_y) {
					c.DrawPoint(x, fill_y, true);
				}
			}
		}

		return window(
			text(std::format(" {}/{} Details ", view.comm, view.pid)) | bold,
			vbox(
				{hbox(
					 {text(std::format(" Head: 0x{:016X} ", view.head)) | flex,
					  text(std::format(" Tail: 0x{:016X} ", view.tail)) | flex}
				 ),
				 hbox(
					 {text(std::format(" Prod HB Age: {} µs ", view.producer_hb_age_us)) | flex,
					  text(std::format(" Cons HB Age: {} µs ", view.consumer_hb_age_us)) | flex}
				 ),
				 separator(),
				 text(std::format(" Throughput (Max: {:.2f} MB/s) ", max_val)) | dim,
				 canvas(std::move(c)) | color(Color::Cyan) | border}
			)
		);
	}
} // namespace tachyon::top::ui
