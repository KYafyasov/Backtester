#pragma once
#include "LimitOrderBook.hpp"
#include "MarketDataEvent.hpp"
#include <memory>
#include <string>
#include <unordered_map>

// Routes events to one LOB per instrument_id and controls periodic snapshots.
class LobRouter {
public:
  // Print a snapshot every N events; zero disables periodic snapshots.
  explicit LobRouter(std::size_t snapshotIntervalEvents = 50000);

  // Route one event from the chronologically ordered producer stream.
  void route(const MarketDataEvent &event);

  // Print the final best bid and ask for each instrument.
  void printFinalState(std::ostream &os) const;

  // Print aggregate statistics for all instruments.
  void printStats(std::ostream &os) const;

private:
  // instrument_id -> LOB
  std::unordered_map<long long, std::shared_ptr<LimitOrderBook>> lobs_;
  // instrument_id -> display symbol
  std::unordered_map<long long, std::string> symbols_;
  std::size_t totalEventsRouted_ = 0;
  std::size_t snapshotInterval_ = 0;
  // Event count at the last F_LAST snapshot.
  std::size_t lastSnapshotAt_ = 0;
};
