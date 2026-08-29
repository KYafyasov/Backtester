#pragma once
#include <chrono>
#include <ostream>

// Records processing time and throughput between start() and finish().
struct PerfStats {
  using Clock = std::chrono::steady_clock;
  Clock::time_point startTime;
  double elapsedSeconds = 0.0;
  std::size_t totalEvents = 0;
  double eventsPerSecond = 0.0;

  void start() { startTime = Clock::now(); }

  void finish(std::size_t events) {
    auto end = Clock::now();
    elapsedSeconds = std::chrono::duration<double>(end - startTime).count();
    totalEvents = events;
    eventsPerSecond = elapsedSeconds > 0 ? events / elapsedSeconds : 0.0;
  }

  void print(std::ostream &os) const {
    os << "\n===== PERFORMANCE =====\n"
       << "Total events:    " << totalEvents << "\n"
       << "Processing time: " << elapsedSeconds << " s\n"
       << "Events/sec:      " << eventsPerSecond << "\n";
  }
};
