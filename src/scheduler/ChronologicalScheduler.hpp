#pragma once

#include "core/Events.hpp"

#include <cstddef>
#include <stdexcept>
#include <variant>
#include <vector>

namespace cmf::scheduler {

using OrderCommand = std::variant<NewOrderCommand, CancelCommand>;

class SchedulerError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class ChronologicalScheduler {
public:
  explicit ChronologicalScheduler(std::size_t maximum_pending_events);

  void push(const ScheduledEvent &event);
  void push(const OrderCommand &command);
  [[nodiscard]] const ScheduledEvent &peek_next() const;
  [[nodiscard]] ScheduledEvent pop_next();
  [[nodiscard]] bool empty() const noexcept { return heap_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return heap_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
  struct Later {
    bool operator()(const ScheduledEvent &left,
                    const ScheduledEvent &right) const noexcept {
      return left.key() > right.key();
    }
  };

  const std::size_t capacity_;
  std::vector<ScheduledEvent> heap_;
};

[[nodiscard]] ScheduledEvent with_dispatch_sequence(const ScheduledEvent &event,
                                                    Sequence dispatch_sequence);

} // namespace cmf::scheduler
