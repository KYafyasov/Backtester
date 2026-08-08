#pragma once

#include "core/BacktestConfig.hpp"
#include "core/Events.hpp"

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace cmf::market {

class L2CacheError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

enum class L2EventKind : std::uint8_t {
  Snapshot = 1,
  Trade = 2,
};

struct L2InputEvent {
  L2EventKind kind{L2EventKind::Snapshot};
  InstrumentId instrument_id{};
  TimestampNs event_ts_ns{};
  Sequence merged_sequence{};
  Sequence source_row{};
  Side side{Side::None};
  PriceTicks trade_price{};
  Quantity trade_quantity{};
  std::vector<BookLevel> bids;
  std::vector<BookLevel> asks;
};

struct L2DatasetMetadata {
  InstrumentMeta instrument;
  std::string dataset_id;
  bool verified_metadata{};
  std::uint64_t total_rows{};
  std::uint64_t snapshot_rows{};
  std::uint64_t trade_rows{};
};

class L2CacheReader {
public:
  using InstrumentMap = std::unordered_map<InstrumentId, InstrumentMeta>;

  L2CacheReader(std::string manifest_path, InstrumentMap instruments,
                DateRange range = {}, bool allow_unverified_metadata = false);

  bool next(L2InputEvent &event);
  [[nodiscard]] const InstrumentMeta &instrument() const noexcept {
    return instrument_;
  }
  [[nodiscard]] const std::string &manifest_path() const noexcept {
    return manifest_path_;
  }

  [[nodiscard]] static bool is_l2_manifest(const std::string &path);
  [[nodiscard]] static InstrumentMeta
  discover_instrument(const std::string &manifest_path);
  [[nodiscard]] static L2DatasetMetadata
  inspect_manifest(const std::string &manifest_path);

private:
  void open_next_partition();
  void validate_partition_end();
  void read_partition_header();
  void read_record(L2InputEvent &event);

  std::string manifest_path_;
  InstrumentMeta instrument_;
  std::uint16_t depth_{};
  std::vector<std::string> cache_paths_;
  std::size_t next_cache_index_{};
  std::ifstream input_;
  std::string current_path_;
  std::uint64_t records_remaining_{};
  TimestampNs previous_timestamp_{};
  Sequence previous_sequence_{};
  bool has_previous_{};
};

} // namespace cmf::market
