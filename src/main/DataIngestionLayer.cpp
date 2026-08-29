#include <iostream>
#include <stdexcept>
#include <string>

#include "PerfStats.hpp"
#include "market/HistoricalLOBStore.hpp"
#include "market/JsonlReader.hpp"

int RunDataIngestionFile(const std::string &filePath) {
  PerfStats stats;
  stats.start();

  cmf::market::JsonlReader reader(
      filePath, cmf::market::JsonlReader::databento_nanounit_policy());
  cmf::market::HistoricalLOBStore books;
  cmf::market::MarketDataEvent event;
  std::size_t eventCount = 0;
  while (reader.next(event)) {
    books.apply(event);
    ++eventCount;
  }

  std::cout << "\n===== FINAL BEST BID / ASK =====\n";
  for (const cmf::InstrumentId instrumentId : books.instrument_ids()) {
    const auto *book = books.find(instrumentId);
    const auto bid = book->best_bid();
    const auto ask = book->best_ask();
    std::cout << "instrument_id=" << instrumentId
              << " best_bid=" << (bid ? std::to_string(bid->price) : "null")
              << " best_bid_size=" << (bid ? bid->quantity : 0)
              << " best_ask=" << (ask ? std::to_string(ask->price) : "null")
              << " best_ask_size=" << (ask ? ask->quantity : 0) << "\n";
  }
  std::cout << "\n===== ROUTER STATS =====\n"
            << "Events routed: " << eventCount << "\n"
            << "Instruments:   " << books.size() << "\n";

  stats.finish(eventCount);
  stats.print(std::cout);

  return 0;
}
