#pragma once

#include "core/BacktestConfig.hpp"
#include "core/ResultSchemas.hpp"
#include "trading/Strategy.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

namespace cmf::results {

class ResultError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct ResultReserveEstimate {
  std::size_t fills{};
  std::size_t order_events{};
  std::size_t pnl_points{};
  std::size_t position_lots_per_instrument{};
};

struct PositionStorageStats {
  std::size_t active_lots{};
  std::size_t capacity{};
  std::size_t runtime_reallocations{};
};

struct DatasetMetadata {
  std::string dataset_id;
  bool verified_metadata{};
};

struct FillColumnsView {
  std::span<const TimestampNs> exchange_ts_ns;
  std::span<const TimestampNs> engine_ts_ns;
  std::span<const InstrumentId> instrument_id;
  std::span<const ClOrdId> client_order_id;
  std::span<const Side> side;
  std::span<const PriceTicks> price_ticks;
  std::span<const Quantity> quantity;
  std::span<const Quantity> remaining_quantity;
  std::span<const LiquiditySource> liquidity_source;
  std::span<const Sequence> trigger_source_sequence;
  std::span<const SourceId> trigger_source_id;
  std::span<const Sequence> trigger_global_market_sequence;

  [[nodiscard]] std::size_t size() const noexcept {
    return exchange_ts_ns.size();
  }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
};

struct OrderLogColumnsView {
  std::span<const TimestampNs> engine_ts_ns;
  std::span<const InstrumentId> instrument_id;
  std::span<const ClOrdId> client_order_id;
  std::span<const OrderLogEventType> event_type;
  std::span<const OrderState> state;
  std::span<const Side> side;
  std::span<const PriceTicks> limit_price_ticks;
  std::span<const Quantity> order_quantity;
  std::span<const Quantity> filled_quantity;
  std::span<const Quantity> remaining_quantity;
  std::span<const RejectReason> reject_reason;

  [[nodiscard]] std::size_t size() const noexcept {
    return engine_ts_ns.size();
  }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
};

struct PnlColumnsView {
  std::span<const TimestampNs> engine_ts_ns;
  std::span<const double> total_pnl;

  [[nodiscard]] std::size_t size() const noexcept {
    return engine_ts_ns.size();
  }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
};

class FrozenResults {
public:
  FrozenResults() = default;

  [[nodiscard]] FillColumnsView fills() const noexcept;
  [[nodiscard]] OrderLogColumnsView order_log() const noexcept;
  [[nodiscard]] PnlColumnsView pnl() const noexcept;
  [[nodiscard]] std::span<const AccountCurrencyAmount>
  exact_pnl() const noexcept;
  [[nodiscard]] const std::optional<DatasetMetadata> &
  dataset_metadata() const noexcept;

  [[nodiscard]] explicit operator bool() const noexcept {
    return static_cast<bool>(storage_);
  }

private:
  class Storage;
  explicit FrozenResults(std::shared_ptr<const Storage> storage)
      : storage_(std::move(storage)) {}

  std::shared_ptr<const Storage> storage_;

  friend class ResultRecorder;
};

class ResultRecorder final : public trading::Recorder {
public:
  ResultRecorder(
      std::span<const InstrumentMeta> instruments,
      ResultReserveEstimate estimate = {},
      std::optional<DatasetMetadata> dataset_metadata = std::nullopt);
  ~ResultRecorder() override;

  void on_order_event(const OrderLogResultRow &row) override;
  void on_fill(const FillResultRow &row) override;

  // A missing side is not a valid mark and deliberately leaves the last valid
  // midpoint unchanged. Returns true only when the stored mark changed.
  bool on_book_mark(InstrumentId instrument_id, TimestampNs engine_ts_ns,
                    std::optional<PriceTicks> best_bid,
                    std::optional<PriceTicks> best_ask);

  [[nodiscard]] Quantity position(InstrumentId instrument_id) const;
  [[nodiscard]] PositionStorageStats
  position_storage_stats(InstrumentId instrument_id) const;
  [[nodiscard]] FrozenResults freeze();
  [[nodiscard]] bool frozen() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace cmf::results
