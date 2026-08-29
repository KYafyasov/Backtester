#pragma once

#include <optional>
#include <ostream>
#include <string>

#include "json.hpp"

using json = nlohmann::json;

// One record from a Databento MBO JSONL file.
// Corresponds to the documented rtype=0xA0 MBO schema.
// Exchange-provided ISO-8601 timestamps and decimal prices remain strings in
// this legacy CLI model.
struct MarketDataEvent {
  // Timestamps use ISO-8601 when pretty_ts=true.
  std::string tsRecv;  // Databento receive time (GPS, monotonic)
  std::string tsEvent; // exchange time (FIX tag 60, nanoseconds)

  static MarketDataEvent fromJson(const json &j);

  // Header (hd).
  int rtype = 0;              // record type: 160=MBO, 32-35=OHLCV, ...
  int publisherId = 0;        // publisher ID (101 = Eurex EOBI)
  long long instrumentId = 0; // option/future instrument ID

  // Order fields.
  char action = 'N'; // A=Add C=Cancel M=Modify T=Trade F=Fill R=Clear N=None
  char side = 'N';   // B=Buy/bid A=Ask/sell N=no side
  std::optional<std::string> price; // decimal string, null when unavailable
  long long size = 0;
  long long channelId = 0;
  std::string orderId;     // uint64 encoded as a string to avoid precision loss
  int flags = 0;           // bit field: 128=F_LAST, 64=F_TOB, ...
  long long tsInDelta = 0; // ns between ts_recv and ts_publisher_send
  unsigned long long sequence = 0;
  std::string symbol; // human-readable ticker when map_symbols=true

  std::string priceOrNull() const {
    return price.has_value() ? *price : "null";
  }
  double priceAsDouble() const {
    // Convert the decimal price string for the legacy double-based LOB.
    return price.has_value() ? std::stod(*price) : 0.0;
  }
  bool isLastInEvent() const {
    // F_LAST (bit 7): the book is consistent after this record.
    return (flags & 128) != 0;
  }
};

std::ostream &operator<<(std::ostream &os, const MarketDataEvent &e);
