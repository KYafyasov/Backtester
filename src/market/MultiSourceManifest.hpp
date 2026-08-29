#pragma once

#include "market/L2CacheReader.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace cmf::market {

class MultiSourceManifestError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct MultiSourceSpec {
  SourceId source_id{};
  SourcePriority source_priority{};
  std::string format;
  std::string manifest_path;
  std::string sha256;
  std::uint64_t bytes{};
  std::uint64_t record_count{};
  std::uint64_t group_count{};
  TimestampNs min_event_ts_ns{};
  TimestampNs max_event_ts_ns{};
  Sequence min_source_sequence{};
  Sequence max_source_sequence{};
  std::vector<InstrumentId> instrument_ids;
  L2DatasetMetadata l2_metadata;
};

struct MultiSourceManifest {
  std::string dataset_id;
  std::vector<MultiSourceSpec> sources;
  std::uint64_t expected_global_group_count{};
  TimestampSemantics timestamp_semantics{TimestampSemantics::Unknown};
  bool verified_metadata{};
};

[[nodiscard]] bool is_multi_source_manifest(const std::string &path);
[[nodiscard]] MultiSourceManifest
read_multi_source_manifest(const std::string &path);

} // namespace cmf::market
