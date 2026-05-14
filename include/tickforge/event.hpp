#pragma once

#include <cstdint>
#include <type_traits>

namespace tickforge {

enum class EventType : uint8_t {
    NONE       = 0,
    TRADE      = 1,
    BOOK_DELTA = 2,
    TICKER     = 3,
};

enum class Side : uint8_t { BID = 0, ASK = 1 };

enum class BookAction : uint8_t {
    UPSERT = 0,
    DELETE = 1,
};

// One cache line per event. Field interpretation depends on `type`:
//
//   TRADE:
//     px      = trade price
//     qty     = trade size
//     side    = aggressor side (BID = buy aggressor, ASK = sell aggressor)
//     extra1  = trade_id (reinterpret as uint64)
//     extra2, extra3, action = unused
//
//   BOOK_DELTA:
//     px      = level price
//     qty     = level size after change (0 means delete; or use action)
//     side    = book side (BID/ASK)
//     action  = UPSERT or DELETE
//     extra*  = unused
//
//   TICKER:
//     extra1  = best bid
//     extra2  = best ask
//     extra3  = bid size (ask size derivable or unused for MVP)
//     px, qty, side, action = unused
struct alignas(64) Event {
    uint64_t  origin_ts_ns;   // when this event "should" happen in the simulated timeline
    uint64_t  seq;            // monotonic sequence number, gap-detection friendly
    uint32_t  symbol_id;      // index into the symbol table (caller-defined)
    EventType type;
    Side      side;
    BookAction action;
    uint8_t   _pad0;
    double    px;
    double    qty;
    double    extra1;
    double    extra2;
    double    extra3;
};

static_assert(sizeof(Event) == 64,
              "Event must be exactly one cache line; check field layout / padding");
static_assert(std::is_trivially_copyable_v<Event>,
              "Event must be trivially copyable so we can mmap and memcpy it");

} // namespace tickforge
