#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cmf::scheduler {

class WakeSignal {
public:
  [[nodiscard]] std::uint64_t snapshot() const noexcept {
    return epoch_.load(std::memory_order_acquire);
  }

  void notify_all() noexcept {
    // Release pairs with wait/snapshot acquire. The signal does not carry the
    // queue payload itself, but guarantees that preceding state publication is
    // visible before a waiter rechecks that state.
    epoch_.fetch_add(1, std::memory_order_release);
    epoch_.notify_all();
  }

  void wait(std::uint64_t snapshot) const noexcept {
    epoch_.wait(snapshot, std::memory_order_acquire);
  }

private:
  std::atomic<std::uint64_t> epoch_{};
};

template <typename T> class SpscRing {
public:
  explicit SpscRing(std::size_t capacity, WakeSignal *observer = nullptr)
      : slots_(checked_storage_size(capacity)), capacity_(capacity),
        observer_(observer) {}

  SpscRing(const SpscRing &) = delete;
  SpscRing &operator=(const SpscRing &) = delete;

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  template <typename U> bool try_push(U &&value) {
    const auto head = head_.value.load(std::memory_order_relaxed);
    // Acquire observes the consumer's slot destruction before this producer
    // reuses that slot. Only the producer writes head.
    const auto tail = tail_.value.load(std::memory_order_acquire);
    const auto next = increment(head);
    if (next == tail || closed_.load(std::memory_order_acquire)) {
      return false;
    }

    slots_[head].emplace(std::forward<U>(value));
    // Release publishes the completed slot construction to consumer acquire.
    head_.value.store(next, std::memory_order_release);
    changed_.notify_all();
    notify_observer();
    return true;
  }

  template <typename U> bool push_wait(U &&value) {
    while (true) {
      if (closed_.load(std::memory_order_acquire)) {
        return false;
      }
      if (try_push(std::forward<U>(value))) {
        return true;
      }

      const auto epoch = changed_.snapshot();
      if (closed_.load(std::memory_order_acquire) || !full()) {
        continue;
      }
      changed_.wait(epoch);
    }
  }

  bool try_pop(T &value) {
    const auto tail = tail_.value.load(std::memory_order_relaxed);
    // Acquire observes the producer's completed slot construction. Only the
    // consumer writes tail.
    const auto head = head_.value.load(std::memory_order_acquire);
    if (tail == head) {
      return false;
    }

    value = std::move(*slots_[tail]);
    slots_[tail].reset();
    // Release publishes slot destruction before producer reuses the slot.
    tail_.value.store(increment(tail), std::memory_order_release);
    changed_.notify_all();
    notify_observer();
    return true;
  }

  bool pop_wait(T &value) {
    while (true) {
      if (try_pop(value)) {
        return true;
      }
      if (closed_.load(std::memory_order_acquire)) {
        return false;
      }

      const auto epoch = changed_.snapshot();
      if (closed_.load(std::memory_order_acquire) || !empty()) {
        continue;
      }
      changed_.wait(epoch);
    }
  }

  void close() noexcept {
    // Acq_rel makes close idempotent and publishes all work preceding shutdown.
    if (!closed_.exchange(true, std::memory_order_acq_rel)) {
      changed_.notify_all();
      notify_observer();
    }
  }

  [[nodiscard]] bool closed() const noexcept {
    return closed_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool empty() const noexcept {
    return head_.value.load(std::memory_order_acquire) ==
           tail_.value.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool full() const noexcept {
    const auto head = head_.value.load(std::memory_order_acquire);
    return increment(head) == tail_.value.load(std::memory_order_acquire);
  }

private:
  struct alignas(64) PaddedIndex {
    std::atomic<std::size_t> value{};
  };

  static std::size_t checked_storage_size(std::size_t capacity) {
    if (capacity == 0 || capacity == std::numeric_limits<std::size_t>::max()) {
      throw std::invalid_argument("SPSC ring capacity must be positive");
    }
    return capacity + 1;
  }

  [[nodiscard]] std::size_t increment(std::size_t index) const noexcept {
    ++index;
    return index == slots_.size() ? 0 : index;
  }

  void notify_observer() noexcept {
    if (observer_ != nullptr) {
      observer_->notify_all();
    }
  }

  std::vector<std::optional<T>> slots_;
  const std::size_t capacity_;
  WakeSignal *observer_;
  PaddedIndex head_;
  PaddedIndex tail_;
  std::atomic<bool> closed_{false};
  WakeSignal changed_;
};

} // namespace cmf::scheduler
