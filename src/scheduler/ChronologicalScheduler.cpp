#include "scheduler/ChronologicalScheduler.hpp"

#include <algorithm>
#include <limits>

namespace cmf::scheduler {

ChronologicalScheduler::ChronologicalScheduler(
    std::size_t maximum_pending_events)
    : capacity_(maximum_pending_events) {
  if (capacity_ == 0) {
    throw std::invalid_argument("scheduler capacity must be positive");
  }
  heap_.reserve(capacity_);
}

void ChronologicalScheduler::push(const ScheduledEvent &event) {
  if (heap_.size() == capacity_) {
    throw SchedulerError("scheduler pending-event capacity exceeded");
  }
  heap_.push_back(event);
  std::push_heap(heap_.begin(), heap_.end(), Later{});
}

void ChronologicalScheduler::push(const OrderCommand &command) {
  std::visit([this](const auto &payload) { push(ScheduledEvent{payload}); },
             command);
}

const ScheduledEvent &ChronologicalScheduler::peek_next() const {
  if (heap_.empty()) {
    throw SchedulerError("cannot peek an empty scheduler");
  }
  return heap_.front();
}

ScheduledEvent ChronologicalScheduler::pop_next() {
  if (heap_.empty()) {
    throw SchedulerError("cannot pop an empty scheduler");
  }
  std::pop_heap(heap_.begin(), heap_.end(), Later{});
  auto event = std::move(heap_.back());
  heap_.pop_back();
  return event;
}

ScheduledEvent with_dispatch_sequence(const ScheduledEvent &event,
                                      Sequence dispatch_sequence) {
  return std::visit(
      [dispatch_sequence](const auto &payload) {
        return ScheduledEvent{payload, dispatch_sequence};
      },
      event.payload());
}

} // namespace cmf::scheduler
