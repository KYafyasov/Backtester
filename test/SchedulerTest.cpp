#include "scheduler/ChronologicalScheduler.hpp"
#include "scheduler/ReadyBarrier.hpp"
#include "scheduler/SchedulerRuntime.hpp"
#include "scheduler/SpscRing.hpp"

#include "MiniTest.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using namespace std::chrono_literals;
using cmf::CancelCommand;
using cmf::DateRange;
using cmf::EventPriority;
using cmf::MarketDelivery;
using cmf::NewOrderCommand;
using cmf::ScheduledEvent;
using cmf::ScheduledKey;
using cmf::Sequence;
using cmf::Side;
using cmf::scheduler::ChronologicalScheduler;
using cmf::scheduler::CommandSink;
using cmf::scheduler::ReadyBarrier;
using cmf::scheduler::SchedulerError;
using cmf::scheduler::SchedulerRuntime;
using cmf::scheduler::SchedulerRuntimeConfig;
using cmf::scheduler::SpscRing;

static_assert(!std::is_default_constructible_v<SchedulerRuntimeConfig>);

ScheduledEvent market(cmf::TimestampNs time, Sequence sequence) {
  return ScheduledEvent{
      MarketDelivery{1, time - 10, time, sequence, {}, {}, {}}};
}

NewOrderCommand new_order(cmf::TimestampNs time, Sequence sequence) {
  return NewOrderCommand{sequence, 1,        Side::Buy, 100,
                         1,        time - 5, time,      sequence};
}

CancelCommand cancel(cmf::TimestampNs time, Sequence sequence) {
  return CancelCommand{sequence, 1, time - 5, time, sequence};
}

struct RecordingConsumer {
  std::vector<ScheduledKey> keys;
  std::vector<Sequence> dispatch_sequences;

  void operator()(const ScheduledEvent &event, CommandSink &) {
    keys.push_back(event.key());
    dispatch_sequences.push_back(event.dispatch_sequence());
  }
};

struct CountingSource {
  std::span<const ScheduledEvent> events;
  std::atomic<int> calls{0};
  std::size_t index{};

  bool next(ScheduledEvent &event) {
    calls.fetch_add(1, std::memory_order_acq_rel);
    if (index == events.size()) {
      return false;
    }
    event = events[index++];
    return true;
  }
};

struct PreparingSource {
  std::span<const ScheduledEvent> events;
  std::vector<Sequence> prepared;
  std::size_t index{};

  bool next(ScheduledEvent &event) {
    if (index == events.size()) {
      return false;
    }
    event = events[index++];
    return true;
  }

  void prepare_for_dispatch(ScheduledEvent &event) {
    prepared.push_back(event.key().source_or_command_sequence);
  }
};

struct ThrowingPreparingSource {
  bool emitted{};

  bool next(ScheduledEvent &event) {
    if (emitted) {
      return false;
    }
    emitted = true;
    event = market(100, 1);
    return true;
  }

  void prepare_for_dispatch(ScheduledEvent &) {
    throw std::runtime_error("exact prepare failure");
  }
};

struct MutatingPreparingSource {
  ScheduledEvent replacement;
  bool emitted{};

  bool next(ScheduledEvent &event) {
    if (emitted) {
      return false;
    }
    emitted = true;
    event = market(100, 1);
    return true;
  }

  void prepare_for_dispatch(ScheduledEvent &event) { event = replacement; }
};

template <typename Function> bool throws_scheduler_error(Function &&function) {
  try {
    function();
  } catch (const SchedulerError &) {
    return true;
  }
  return false;
}

} // namespace

TEST_CASE("SPSC ring supports capacity one and repeated wrap-around",
          "[Scheduler]") {
  SpscRing<int> ring(1);
  REQUIRE(ring.capacity() == 1);
  REQUIRE(ring.empty());
  REQUIRE_FALSE(ring.full());

  for (int value = 0; value < 100; ++value) {
    REQUIRE(ring.try_push(value));
    REQUIRE(ring.full());
    REQUIRE_FALSE(ring.try_push(value + 1));
    int popped = -1;
    REQUIRE(ring.try_pop(popped));
    REQUIRE(popped == value);
    REQUIRE(ring.empty());
  }
}

TEST_CASE("SPSC ring applies backpressure without dropping values",
          "[Scheduler]") {
  SpscRing<int> ring(1);
  std::atomic<bool> first_pushed{false};
  std::atomic<bool> second_pushed{false};

  std::thread producer([&] {
    REQUIRE(ring.push_wait(11));
    first_pushed.store(true, std::memory_order_release);
    first_pushed.notify_one();
    REQUIRE(ring.push_wait(22));
    second_pushed.store(true, std::memory_order_release);
    second_pushed.notify_one();
  });

  first_pushed.wait(false, std::memory_order_acquire);
  REQUIRE_FALSE(second_pushed.load(std::memory_order_acquire));
  int value = 0;
  REQUIRE(ring.pop_wait(value));
  REQUIRE(value == 11);
  second_pushed.wait(false, std::memory_order_acquire);
  REQUIRE(ring.pop_wait(value));
  REQUIRE(value == 22);
  producer.join();
}

TEST_CASE("SPSC ring close unblocks empty consumer and full producer",
          "[Scheduler]") {
  {
    SpscRing<int> empty_ring(1);
    auto waiting_pop = std::async(std::launch::async, [&] {
      int value = 0;
      return empty_ring.pop_wait(value);
    });
    empty_ring.close();
    REQUIRE(waiting_pop.wait_for(1s) == std::future_status::ready);
    REQUIRE_FALSE(waiting_pop.get());
  }

  {
    SpscRing<int> full_ring(1);
    REQUIRE(full_ring.try_push(1));
    auto waiting_push =
        std::async(std::launch::async, [&] { return full_ring.push_wait(2); });
    full_ring.close();
    REQUIRE(waiting_push.wait_for(1s) == std::future_status::ready);
    REQUIRE_FALSE(waiting_push.get());
    int value = 0;
    REQUIRE(full_ring.try_pop(value));
    REQUIRE(value == 1);
    REQUIRE_FALSE(full_ring.pop_wait(value));
  }
}

TEST_CASE("Ready barrier publishes monotonically and stop wakes a waiter",
          "[Scheduler]") {
  ReadyBarrier barrier;
  REQUIRE(barrier.publish_processed(1));
  REQUIRE(barrier.processed_sequence() == 1);
  REQUIRE_FALSE(barrier.publish_processed(1));
  REQUIRE_FALSE(barrier.publish_processed(0));
  REQUIRE(barrier.publish_processed(2));
  REQUIRE(barrier.wait_until(2));

  auto waiting =
      std::async(std::launch::async, [&] { return barrier.wait_until(3); });
  barrier.stop();
  REQUIRE(waiting.wait_for(1s) == std::future_status::ready);
  REQUIRE_FALSE(waiting.get());
}

TEST_CASE("Chronological scheduler uses time priority and stable sequence",
          "[Scheduler]") {
  ChronologicalScheduler scheduler(8);
  scheduler.push(ScheduledEvent{cancel(100, 1)});
  scheduler.push(market(101, 1));
  scheduler.push(ScheduledEvent{new_order(100, 2)});
  scheduler.push(market(100, 2));
  scheduler.push(ScheduledEvent{new_order(100, 1)});
  scheduler.push(market(100, 1));

  std::vector<ScheduledKey> actual;
  while (!scheduler.empty()) {
    actual.push_back(scheduler.pop_next().key());
  }

  const std::vector<ScheduledKey> expected{
      {100, EventPriority::MarketData, 1}, {100, EventPriority::MarketData, 2},
      {100, EventPriority::NewOrder, 1},   {100, EventPriority::NewOrder, 2},
      {100, EventPriority::Cancel, 1},     {101, EventPriority::MarketData, 1},
  };
  REQUIRE(actual == expected);
}

TEST_CASE("Scheduler runtime merges callback commands by arrival time",
          "[Scheduler]") {
  const std::array initial{market(100, 10), market(300, 20)};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 1, 8});
  RecordingConsumer recording;
  bool commands_sent = false;

  runtime.run(initial, [&](const ScheduledEvent &event, CommandSink &commands) {
    recording(event, commands);
    if (!commands_sent) {
      commands_sent = true;
      REQUIRE(commands.push(cancel(250, 2)));
      REQUIRE(commands.push(new_order(200, 1)));
    }
  });

  const std::vector<ScheduledKey> expected{
      {100, EventPriority::MarketData, 10},
      {200, EventPriority::NewOrder, 1},
      {250, EventPriority::Cancel, 2},
      {300, EventPriority::MarketData, 20},
  };
  REQUIRE(recording.keys == expected);
  REQUIRE(recording.dispatch_sequences == std::vector<Sequence>({1, 2, 3, 4}));
  REQUIRE(runtime.processed_sequence() == 4);
}

TEST_CASE("Market source prepares only after chronological selection",
          "[Scheduler]") {
  const std::array initial{market(100, 10), market(300, 20)};
  PreparingSource source{initial, {}, 0};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 1, 8});
  RecordingConsumer recording;
  bool command_sent = false;

  runtime.run(source, [&](const ScheduledEvent &event, CommandSink &commands) {
    recording(event, commands);
    if (event.priority() == EventPriority::MarketData &&
        event.key().scheduled_ts_ns == 100) {
      REQUIRE(source.prepared == std::vector<Sequence>({10}));
      command_sent = true;
      REQUIRE(commands.push(new_order(200, 1)));
    } else if (event.priority() == EventPriority::NewOrder) {
      REQUIRE(command_sent);
      REQUIRE(source.prepared == std::vector<Sequence>({10}));
    } else {
      REQUIRE(source.prepared == std::vector<Sequence>({10, 20}));
    }
  });

  const std::vector<ScheduledKey> expected{
      {100, EventPriority::MarketData, 10},
      {200, EventPriority::NewOrder, 1},
      {300, EventPriority::MarketData, 20},
  };
  REQUIRE(recording.keys == expected);
  REQUIRE(source.prepared == std::vector<Sequence>({10, 20}));
}

TEST_CASE("Equal-time market prepares and dispatches before command",
          "[Scheduler]") {
  const std::array initial{market(100, 10), market(200, 20)};
  PreparingSource source{initial, {}, 0};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 1, 8});
  RecordingConsumer recording;

  runtime.run(source, [&](const ScheduledEvent &event, CommandSink &commands) {
    recording(event, commands);
    if (event.key().scheduled_ts_ns == 100) {
      REQUIRE(commands.push(new_order(200, 1)));
    } else if (event.priority() == EventPriority::NewOrder) {
      REQUIRE(source.prepared == std::vector<Sequence>({10, 20}));
    }
  });

  const std::vector<ScheduledKey> expected{
      {100, EventPriority::MarketData, 10},
      {200, EventPriority::MarketData, 20},
      {200, EventPriority::NewOrder, 1},
  };
  REQUIRE(recording.keys == expected);
  REQUIRE(source.prepared == std::vector<Sequence>({10, 20}));
}

TEST_CASE("Prepare failure joins exactly and a fresh runtime recovers",
          "[Scheduler]") {
  ThrowingPreparingSource source;
  SchedulerRuntime failing(SchedulerRuntimeConfig{DateRange{}, 1, 1, 4});
  auto run = std::async(std::launch::async, [&] {
    try {
      RecordingConsumer recording;
      failing.run(source, recording);
    } catch (const std::runtime_error &error) {
      return std::string(error.what());
    }
    return std::string{"missing prepare failure"};
  });

  REQUIRE(run.wait_for(1s) == std::future_status::ready);
  REQUIRE(run.get() == "exact prepare failure");

  const std::array events{market(100, 1)};
  SchedulerRuntime recovered(SchedulerRuntimeConfig{DateRange{}, 1, 1, 4});
  RecordingConsumer recording;
  recovered.run(events, recording);
  REQUIRE(recording.keys.size() == 1);
}

TEST_CASE("Prepare hook cannot mutate market key or priority", "[Scheduler]") {
  {
    MutatingPreparingSource source{market(101, 1), false};
    SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 1, 4});
    RecordingConsumer recording;
    REQUIRE(throws_scheduler_error([&] { runtime.run(source, recording); }));
  }
  {
    MutatingPreparingSource source{ScheduledEvent{new_order(100, 1)}, false};
    SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 1, 4});
    RecordingConsumer recording;
    REQUIRE(throws_scheduler_error([&] { runtime.run(source, recording); }));
  }
}

TEST_CASE("Dispatcher does not publish the next event before acknowledgement",
          "[Scheduler]") {
  const std::array initial{market(100, 1), market(200, 2)};
  CountingSource source{initial};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 1, 4});
  std::atomic<bool> first_entered{false};
  std::atomic<bool> release_first{false};
  std::atomic<int> calls{0};

  auto run = std::async(std::launch::async, [&] {
    runtime.run(source, [&](const ScheduledEvent &, CommandSink &) {
      const int call = calls.fetch_add(1, std::memory_order_acq_rel);
      if (call == 0) {
        first_entered.store(true, std::memory_order_release);
        first_entered.notify_one();
        release_first.wait(false, std::memory_order_acquire);
      }
    });
  });

  first_entered.wait(false, std::memory_order_acquire);
  REQUIRE(calls.load(std::memory_order_acquire) == 1);
  REQUIRE(source.calls.load(std::memory_order_acquire) == 1);
  REQUIRE(runtime.processed_sequence() == 0);
  release_first.store(true, std::memory_order_release);
  release_first.notify_one();
  REQUIRE(run.wait_for(1s) == std::future_status::ready);
  run.get();
  REQUIRE(calls.load(std::memory_order_acquire) == 2);
  REQUIRE(source.calls.load(std::memory_order_acquire) == 3);
  REQUIRE(runtime.processed_sequence() == 2);
}

TEST_CASE("Consumer failure unblocks dispatcher and is rethrown",
          "[Scheduler]") {
  const std::array initial{market(100, 1), market(200, 2)};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 1, 4});
  bool saw_original = false;

  try {
    runtime.run(initial, [](const ScheduledEvent &, CommandSink &) {
      throw std::runtime_error("scripted consumer failure");
    });
  } catch (const std::runtime_error &error) {
    saw_original = std::string(error.what()) == "scripted consumer failure";
  }
  REQUIRE(saw_original);
}

TEST_CASE("Runtime rejects a callback command that moves virtual time back",
          "[Scheduler]") {
  const std::array initial{market(100, 1)};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 1, 4});
  bool sent = false;
  REQUIRE(throws_scheduler_error([&] {
    runtime.run(initial, [&](const ScheduledEvent &, CommandSink &commands) {
      if (!sent) {
        sent = true;
        REQUIRE(commands.push(new_order(99, 1)));
      }
    });
  }));
}

TEST_CASE("Command arrival exactly at the date-range end is dispatched",
          "[Scheduler]") {
  const std::array initial{market(100, 1)};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{0, 100}, 1, 1, 4});
  RecordingConsumer recording;
  bool sent = false;

  runtime.run(initial, [&](const ScheduledEvent &event, CommandSink &commands) {
    recording(event, commands);
    if (!sent) {
      sent = true;
      REQUIRE(commands.push(new_order(100, 2)));
    }
  });

  const std::vector<ScheduledKey> expected{
      {100, EventPriority::MarketData, 1},
      {100, EventPriority::NewOrder, 2},
  };
  REQUIRE(recording.keys == expected);
  REQUIRE(runtime.processed_sequence() == 2);
}

TEST_CASE("Commands after the date-range end are skipped without stranding EOD",
          "[Scheduler]") {
  const std::array initial{market(90, 1), market(100, 2)};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{0, 100}, 1, 1, 8});
  RecordingConsumer recording;

  runtime.run(initial, [&](const ScheduledEvent &event, CommandSink &commands) {
    recording(event, commands);
    if (event.priority() != EventPriority::MarketData) {
      return;
    }
    if (event.key().scheduled_ts_ns == 90) {
      REQUIRE(commands.push(cancel(100, 4)));
      REQUIRE(commands.push(new_order(95, 3)));
      REQUIRE(commands.push(cancel(101, 5)));
    } else if (event.key().scheduled_ts_ns == 100) {
      REQUIRE(commands.push(new_order(101, 6)));
      REQUIRE(commands.push(cancel(200, 7)));
    }
  });

  const std::vector<ScheduledKey> expected{
      {90, EventPriority::MarketData, 1},
      {95, EventPriority::NewOrder, 3},
      {100, EventPriority::MarketData, 2},
      {100, EventPriority::Cancel, 4},
  };
  REQUIRE(recording.keys == expected);
  REQUIRE(runtime.processed_sequence() == 4);
}

TEST_CASE("Scheduler overflow stops a blocked command producer",
          "[Scheduler]") {
  const std::array initial{market(100, 1), market(500, 2)};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 1, 1});
  bool sent = false;
  REQUIRE(throws_scheduler_error([&] {
    runtime.run(initial, [&](const ScheduledEvent &, CommandSink &commands) {
      if (!sent) {
        sent = true;
        (void)commands.push(new_order(200, 1));
        (void)commands.push(new_order(300, 2));
        (void)commands.push(new_order(400, 3));
      }
    });
  }));
}

TEST_CASE("Twenty scheduler runs produce identical normalized order",
          "[Scheduler]") {
  std::vector<ScheduledKey> baseline;

  for (int run = 0; run < 20; ++run) {
    const std::array initial{market(100, 1), market(100, 3), market(300, 9)};
    SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 1, 16});
    RecordingConsumer recording;
    bool sent = false;
    runtime.run(initial,
                [&](const ScheduledEvent &event, CommandSink &commands) {
                  recording(event, commands);
                  if (!sent) {
                    sent = true;
                    REQUIRE(commands.push(cancel(200, 5)));
                    REQUIRE(commands.push(new_order(100, 4)));
                    REQUIRE(commands.push(cancel(100, 2)));
                  }
                });

    if (run == 0) {
      baseline = recording.keys;
    } else {
      REQUIRE(recording.keys == baseline);
    }
  }
}

TEST_CASE("Runtime handles clean empty end-of-data", "[Scheduler]") {
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 1, 1});
  RecordingConsumer recording;
  runtime.run({}, recording);
  REQUIRE(recording.keys.empty());
  REQUIRE(runtime.processed_sequence() == 0);
}
