#pragma once

#include "core/BacktestConfig.hpp"
#include "results/ResultRecorder.hpp"
#include "trading/Strategy.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cmf::runtime {

inline constexpr TimestampNs default_order_latency_ns = 1;

struct RunStatistics {
  bool l2_input{};
  std::uint64_t source_records_read{};
  std::uint64_t source_records_warmed{};
  std::uint64_t source_records_replayed{};
  std::uint64_t source_records_after_end{};
  std::uint64_t replayed_book_records{};
  std::uint64_t replayed_trade_records{};
  std::optional<Sequence> first_replayed_sequence;
  std::optional<Sequence> last_replayed_sequence;
  std::uint64_t replayed_sequence_digest{};
  std::uint64_t scheduled_events{};
  std::uint64_t market_deliveries{};
  std::uint64_t new_order_arrivals{};
  std::uint64_t cancel_arrivals{};
  std::uint64_t source_trade_events{};
  std::optional<std::string> dataset_id;
  std::optional<bool> verified_metadata;
  std::optional<std::uint64_t> manifest_total_records;
  std::optional<std::uint64_t> manifest_snapshot_records;
  std::optional<std::uint64_t> manifest_trade_records;
};

[[nodiscard]] std::vector<InstrumentMeta>
discover_databento_instruments(const std::string &data_path);

[[nodiscard]] results::FrozenResults
run_backtest(trading::Strategy &strategy, const std::string &data_path,
             DateRange date_range, BacktestConfig config,
             std::vector<InstrumentMeta> instruments,
             RunStatistics *statistics = nullptr);

} // namespace cmf::runtime
