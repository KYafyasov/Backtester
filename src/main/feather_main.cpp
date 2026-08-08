#include "args.hpp"
#include "common/BlockingQueue.hpp"
#include "common/NaiveTimer.hpp"
#include "ingestion/FeatherDataParser.hpp"
#include "ingestion/FlatMerger.hpp"
#include "ingestion/IngestionPipeline.hpp"

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <deque>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/MarketDataEvent.hpp"
#include "order_book/AbseilOrderBook.hpp"
#include "order_book/BookAnomalyLog.hpp"
#include "order_book/SimpleOrderBookRouter.hpp"

#define PROCESS_MARKET_DATA_EVENT_MODE 1 // 1 = COUNT mode, 2 = PRINT mode

using namespace cmf;

constexpr uint64_t REPORT_AFTER_EACH_N_EVENTS = 5'000'000;

template <typename T, template <typename> typename QImpl,
          std::size_t BatchSize = 256>
struct BatchPusher {
  QImpl<T> &queue;
  std::array<T, BatchSize> batch = {};
  std::size_t count = 0;

  explicit BatchPusher(QImpl<T> &q) : queue(q) {}

  void push(T item) {
    batch[count++] = std::move(item);
    if (count >= BatchSize) {
      flush();
    }
  }

  void flush() {
    if (count > 0 && !queue.is_closed()) {
      queue.push_batch(batch.data(), count);
      count = 0;
    }
  }

  ~BatchPusher() { flush(); }
};

struct ProcessMarketDataEvent {
  ProcessMarketDataEvent() : counter_(0) {}

  BlockingQueue<std::string> print_queue;

  void operator()(const MarketDataEvent &event) {
    order_book_router_.apply(event);

#if PROCESS_MARKET_DATA_EVENT_MODE == 1
    if (event.ts_recv > 0) {
      const std::uint64_t count =
          counter_.fetch_add(1, std::memory_order_relaxed) + 1;
      if (count % REPORT_AFTER_EACH_N_EVENTS == 0) {
        std::ostringstream stream;
        stream << "\n=== Snapshot at " << count << " events ===\n"
               << order_book_router_.snapshot_as_string(5)
               << "===============================\n\n";
        print_queue.push(std::move(stream).str());
      }
    }
#endif

#if PROCESS_MARKET_DATA_EVENT_MODE == 2
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "ts_recv=%lld ts_event=%lld order_id=%llu side=%d price=%" PRId64
                  " size=%u action=%d\n",
                  event.ts_recv, event.ts_event, event.order_id,
                  static_cast<int>(event.side), event.price, event.size,
                  static_cast<int>(event.action));
    print_queue.push(std::string(buffer));
#endif
  }

  void summary(double elapsed_seconds) const {
    const std::uint64_t count = counter_.load(std::memory_order_acquire);
    const double throughput =
        elapsed_seconds > 0.0 ? static_cast<double>(count) / elapsed_seconds
                              : 0.0;
    std::printf("Total messages processed : %" PRIu64 "\n", count);
    std::printf("Wall-clock time          : %.3f s\n", elapsed_seconds);
    std::printf("Throughput               : %.0f msg/s\n", throughput);
  }

  void print_best_bid_ask(std::ostream &output) const {
    order_book_router_.print_best_bid_ask(output);
  }

private:
  mutable std::atomic_ullong counter_;
  SimpleOrderBookRouter<AbseilOrderBook> order_book_router_;
};

int main(int argc, const char *argv[]) {
  try {
    using Parser = FeatherDataParser;
    const Config config = parse_args(std::span(argv, argc), Parser::filename_ext);
    const std::size_t data_files_count = config.data_files.size();

    [[maybe_unused]] auto &anomaly_log = BookAnomalyLog::instance();

    NaiveTimer timer;
    std::deque<BlockingQueue<MarketDataEvent>> file_queues;
    for (std::size_t i = 0; i < data_files_count; ++i) {
      file_queues.emplace_back();
    }

    BlockingQueue<MarketDataEvent> merged_queue;
    const FlatMerger<BlockingQueue, BlockingQueue> merger(file_queues,
                                                          merged_queue);

    ProcessMarketDataEvent sink;
    std::thread io_thread([&sink]() {
      while (sink.print_queue.pop([](std::string &&message) {
        std::fputs(message.c_str(), stdout);
      })) {
      }
    });
    std::thread merger_thread([&merger]() { merger.run_impl(); });
    std::thread dispatcher_thread([&]() {
      while (merged_queue.pop(
          [&sink](MarketDataEvent &&event) { sink(event); })) {
      }

#if PROCESS_MARKET_DATA_EVENT_MODE == 1
      sink.summary(timer.elapsed_seconds());
#endif

      sink.print_best_bid_ask(std::cout);
      BookAnomalyLog::instance().write_summary(std::cerr);
      BookAnomalyLog::instance().flush();
      sink.print_queue.close();
    });

    std::vector<std::thread> producers;
    producers.reserve(data_files_count);
    for (std::size_t i = 0; i < data_files_count; ++i) {
      producers.emplace_back([&file_queues, &config, i]() {
        BatchPusher batcher{file_queues[i]};
        auto push = [&batcher](const MarketDataEvent &event) {
          batcher.push(event);
        };
        IngestionPipeline<Parser, decltype(push)> pipeline(config.data_files[i],
                                                          push);
        pipeline.ingest();
        file_queues[i].close();
      });
    }

    for (auto &producer : producers) {
      producer.join();
    }
    merger_thread.join();
    dispatcher_thread.join();
    io_thread.join();
  } catch (const std::exception &error) {
    std::cerr << "Back-tester threw an exception: " << error.what()
              << std::endl;
    return 1;
  }

  return 0;
}
