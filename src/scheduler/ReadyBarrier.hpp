#pragma once

#include "core/Types.hpp"
#include "scheduler/SpscRing.hpp"

#include <atomic>

namespace cmf::scheduler {

class ReadyBarrier {
public:
  explicit ReadyBarrier(WakeSignal *observer = nullptr) : observer_(observer) {}

  [[nodiscard]] bool publish_processed(Sequence dispatch_sequence) noexcept {
    const auto previous = processed_sequence_.load(std::memory_order_relaxed);
    if (dispatch_sequence == 0 || dispatch_sequence <= previous) {
      return false;
    }
    // The consumer publishes only after its complete reaction. Release makes
    // all reaction state and queued commands visible to dispatcher acquire.
    processed_sequence_.store(dispatch_sequence, std::memory_order_release);
    changed_.notify_all();
    notify_observer();
    return true;
  }

  [[nodiscard]] Sequence processed_sequence() const noexcept {
    return processed_sequence_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool wait_until(Sequence expected) const noexcept {
    while (processed_sequence() < expected) {
      if (stopped_.load(std::memory_order_acquire)) {
        return false;
      }
      const auto epoch = changed_.snapshot();
      if (processed_sequence() >= expected ||
          stopped_.load(std::memory_order_acquire)) {
        continue;
      }
      changed_.wait(epoch);
    }
    return true;
  }

  void stop() noexcept {
    if (!stopped_.exchange(true, std::memory_order_acq_rel)) {
      changed_.notify_all();
      notify_observer();
    }
  }

  [[nodiscard]] bool stopped() const noexcept {
    return stopped_.load(std::memory_order_acquire);
  }

private:
  void notify_observer() noexcept {
    if (observer_ != nullptr) {
      observer_->notify_all();
    }
  }

  alignas(64) std::atomic<Sequence> processed_sequence_{};
  std::atomic<bool> stopped_{false};
  WakeSignal changed_;
  WakeSignal *observer_;
};

} // namespace cmf::scheduler
