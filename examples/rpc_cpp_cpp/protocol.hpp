#pragma once

#include <cstdint>

static constexpr uint32_t MSG_BOOK_REQUEST	= 1;
static constexpr uint32_t MSG_BOOK_SNAPSHOT = 2;
static constexpr uint32_t MSG_SENTINEL		= 0;

struct BookRequest {
	uint64_t instrument_id;
};

struct BookLevel {
	double price;
	double qty;
};

struct BookSnapshot {
	uint64_t  instrument_id;
	uint64_t  seq;
	BookLevel bids[5];
	BookLevel asks[5];
};

static_assert(sizeof(BookSnapshot) == 176);
