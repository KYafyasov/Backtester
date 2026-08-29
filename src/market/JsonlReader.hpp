#pragma once

#include "core/BacktestConfig.hpp"
#include "market/MarketDataEvent.hpp"

#include <cstddef>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace cmf::market {

class SourceError : public std::runtime_error {
public:
  SourceError(std::string path, std::size_t row, std::string context,
              std::string message);

  [[nodiscard]] const std::string &path() const noexcept { return path_; }
  [[nodiscard]] std::size_t row() const noexcept { return row_; }
  [[nodiscard]] const std::string &context() const noexcept { return context_; }

private:
  std::string path_;
  std::size_t row_{};
  std::string context_;
};

class JsonlReader {
public:
  using InstrumentMap = std::unordered_map<InstrumentId, InstrumentMeta>;

  struct UniformInstrumentPolicy {
    PriceTicks tick_size_ticks{};
    PriceTicks price_scale{};
    Quantity contract_multiplier{1};
  };

  // Databento fixed prices use 1e-9 quoted-price units. Tick size one accepts
  // every exactly representable Databento nanounit while rejecting finer input.
  [[nodiscard]] static constexpr UniformInstrumentPolicy
  databento_nanounit_policy() noexcept {
    return UniformInstrumentPolicy{1, 1'000'000'000, 1};
  }

  explicit JsonlReader(std::string path, InstrumentMap instruments);
  explicit JsonlReader(std::string path, UniformInstrumentPolicy policy);

  JsonlReader(const JsonlReader &) = delete;
  JsonlReader &operator=(const JsonlReader &) = delete;
  JsonlReader(JsonlReader &&) = default;
  JsonlReader &operator=(JsonlReader &&) = default;

  [[nodiscard]] bool next(MarketDataEvent &event);
  [[nodiscard]] std::size_t row() const noexcept { return row_; }

private:
  [[nodiscard]] InstrumentMeta instrument_meta(InstrumentId id) const;
  [[noreturn]] void fail(const std::string &message) const;

  std::string path_;
  InstrumentMap instruments_;
  std::optional<UniformInstrumentPolicy> uniform_policy_;
  std::ifstream input_;
  std::size_t row_{};
  std::string current_line_;
  bool have_previous_{};
  TimestampNs previous_exchange_ts_ns_{};
  Sequence previous_sequence_{};
};

} // namespace cmf::market
