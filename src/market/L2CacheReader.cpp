#include "market/L2CacheReader.hpp"

#include "main/json.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sstream>
#include <type_traits>

namespace cmf::market {
namespace {

using Json = nlohmann::json;
constexpr std::array<char, 8> cache_magic{'C', 'M', 'F', 'L',
                                          '2', 'C', '0', '1'};
constexpr std::uint16_t cache_version = 1;

[[nodiscard]] Json read_json(const std::string &path) {
  std::ifstream input(path);
  if (!input) {
    throw L2CacheError("cannot open L2 manifest: " + path);
  }
  try {
    return Json::parse(input);
  } catch (const std::exception &error) {
    throw L2CacheError("invalid L2 manifest " + path + ": " + error.what());
  }
}

[[nodiscard]] const Json &required(const Json &object, const char *name) {
  if (!object.is_object() || !object.contains(name)) {
    throw L2CacheError(std::string("L2 manifest missing '") + name + "'");
  }
  return object.at(name);
}

template <typename Value>
[[nodiscard]] Value manifest_integer(const Json &object, const char *name) {
  const auto &value = required(object, name);
  if (!value.is_number_integer() && !value.is_number_unsigned()) {
    throw L2CacheError(std::string("L2 manifest field '") + name +
                       "' must be an integer");
  }
  try {
    return value.get<Value>();
  } catch (const std::exception &) {
    throw L2CacheError(std::string("L2 manifest field '") + name +
                       "' is out of range");
  }
}

[[nodiscard]] InstrumentMeta parse_instrument(const Json &manifest) {
  if (required(manifest, "format") != "cmf-l2-parquet-cache-v1" ||
      manifest_integer<std::uint16_t>(manifest, "manifest_version") != 1 ||
      manifest_integer<std::uint16_t>(manifest, "data_schema_version") != 1) {
    throw L2CacheError("unsupported L2 manifest format or schema version");
  }
  InstrumentMeta meta{
      manifest_integer<InstrumentId>(manifest, "instrument_id"),
      manifest_integer<PriceTicks>(manifest, "tick_size_ticks"),
      manifest_integer<PriceTicks>(manifest, "price_scale"),
      manifest_integer<Quantity>(manifest, "contract_multiplier")};
  if (meta.instrument_id <= 0 || meta.tick_size_ticks <= 0 ||
      meta.price_scale <= 0 || meta.contract_multiplier <= 0) {
    throw L2CacheError("L2 manifest instrument metadata must be positive");
  }
  return meta;
}

template <typename Unsigned>
[[nodiscard]] Unsigned read_unsigned_le(std::istream &input,
                                        const std::string &context) {
  static_assert(std::is_unsigned_v<Unsigned>);
  std::array<unsigned char, sizeof(Unsigned)> bytes{};
  input.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
  if (!input) {
    throw L2CacheError("truncated L2 cache while reading " + context);
  }
  Unsigned value{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    value |= static_cast<Unsigned>(bytes[index]) << (index * 8U);
  }
  return value;
}

template <typename Signed>
[[nodiscard]] Signed read_signed_le(std::istream &input,
                                    const std::string &context) {
  static_assert(std::is_signed_v<Signed>);
  using Unsigned = std::make_unsigned_t<Signed>;
  const Unsigned raw = read_unsigned_le<Unsigned>(input, context);
  Signed value{};
  std::memcpy(&value, &raw, sizeof(value));
  return value;
}

[[nodiscard]] std::vector<std::string>
parse_cache_paths(const Json &manifest, const std::string &manifest_path) {
  const auto &partitions = required(manifest, "partitions");
  if (!partitions.is_array() || partitions.empty()) {
    throw L2CacheError("L2 manifest partitions must be a non-empty array");
  }
  const auto root = std::filesystem::absolute(manifest_path).parent_path();
  std::vector<std::string> paths;
  paths.reserve(partitions.size());
  for (const auto &partition : partitions) {
    const auto &relative = required(partition, "replay_cache");
    if (!relative.is_string() ||
        relative.get_ref<const std::string &>().empty()) {
      throw L2CacheError("L2 partition replay_cache must be a path string");
    }
    const std::filesystem::path candidate = relative.get<std::string>();
    if (candidate.is_absolute() || std::find(candidate.begin(), candidate.end(),
                                             "..") != candidate.end()) {
      throw L2CacheError(
          "L2 replay cache paths must be relative and remain in the dataset");
    }
    const auto path = (root / candidate).lexically_normal();
    const auto expected_size =
        manifest_integer<std::uintmax_t>(partition, "replay_cache_bytes");
    std::error_code error;
    const auto actual_size = std::filesystem::file_size(path, error);
    if (error) {
      throw L2CacheError("cannot inspect L2 replay cache " + path.string() +
                         ": " + error.message());
    }
    if (actual_size != expected_size) {
      throw L2CacheError(
          "L2 replay cache has trailing bytes or is truncated: " +
          path.string());
    }
    paths.push_back(path.string());
  }
  return paths;
}

[[nodiscard]] std::vector<std::string>
select_cache_paths(const Json &manifest, const std::string &manifest_path,
                   DateRange range) {
  const auto all_paths = parse_cache_paths(manifest, manifest_path);
  const auto &partitions = required(manifest, "partitions");
  std::size_t first = partitions.size();
  std::size_t last = partitions.size();
  TimestampNs previous_max = std::numeric_limits<TimestampNs>::lowest();
  Sequence previous_sequence = 0;
  for (std::size_t index = 0; index < partitions.size(); ++index) {
    const auto &partition = partitions[index];
    const auto minimum =
        manifest_integer<TimestampNs>(partition, "min_event_ts_ns");
    const auto maximum =
        manifest_integer<TimestampNs>(partition, "max_event_ts_ns");
    const auto minimum_sequence =
        manifest_integer<Sequence>(partition, "min_merged_sequence");
    const auto maximum_sequence =
        manifest_integer<Sequence>(partition, "max_merged_sequence");
    if (minimum > maximum || minimum < previous_max || minimum_sequence == 0 ||
        minimum_sequence <= previous_sequence ||
        minimum_sequence > maximum_sequence) {
      throw L2CacheError("L2 manifest partitions are not strictly ordered");
    }
    previous_max = maximum;
    previous_sequence = maximum_sequence;
    if (first == partitions.size() && maximum >= range.start_ts_ns) {
      first = index;
    }
    if (last == partitions.size() && minimum > range.end_ts_ns) {
      last = index;
    }
  }
  if (first == partitions.size()) {
    return {};
  }
  if (last == partitions.size()) {
    last = partitions.size();
  }
  // Warm from the nearest earlier partition containing a snapshot. This is
  // sufficient for full-snapshot L2 state and avoids scanning all prior days.
  if (first > 0) {
    for (std::size_t index = first; index-- > 0;) {
      if (manifest_integer<std::uint64_t>(partitions[index], "snapshot_rows") >
          0) {
        first = index;
        break;
      }
    }
  }
  if (first >= last) {
    return {};
  }
  return {all_paths.begin() + static_cast<std::ptrdiff_t>(first),
          all_paths.begin() + static_cast<std::ptrdiff_t>(last)};
}

} // namespace

bool L2CacheReader::is_l2_manifest(const std::string &path) {
  try {
    const auto manifest = read_json(path);
    if (!manifest.is_object() || !manifest.contains("format") ||
        !manifest.at("format").is_string()) {
      return false;
    }
    return manifest.at("format").get_ref<const std::string &>().starts_with(
        "cmf-l2-");
  } catch (const L2CacheError &) {
    return false;
  }
}

InstrumentMeta
L2CacheReader::discover_instrument(const std::string &manifest_path) {
  return parse_instrument(read_json(manifest_path));
}

L2CacheReader::L2CacheReader(std::string manifest_path,
                             InstrumentMap instruments, DateRange range)
    : manifest_path_(std::move(manifest_path)) {
  const auto manifest = read_json(manifest_path_);
  instrument_ = parse_instrument(manifest);
  depth_ = manifest_integer<std::uint16_t>(manifest, "book_depth");
  if (depth_ == 0 || depth_ > 255) {
    throw L2CacheError("L2 manifest book_depth must be in [1, 255]");
  }
  const auto configured = instruments.find(instrument_.instrument_id);
  if (configured == instruments.end() ||
      configured->second.tick_size_ticks != instrument_.tick_size_ticks ||
      configured->second.price_scale != instrument_.price_scale ||
      configured->second.contract_multiplier !=
          instrument_.contract_multiplier) {
    throw L2CacheError(
        "runtime instrument metadata does not match L2 manifest");
  }
  cache_paths_ = select_cache_paths(manifest, manifest_path_, range);
}

void L2CacheReader::open_next_partition() {
  if (next_cache_index_ == cache_paths_.size()) {
    return;
  }
  current_path_ = cache_paths_[next_cache_index_++];
  input_.close();
  input_.clear();
  input_.open(current_path_, std::ios::binary);
  if (!input_) {
    throw L2CacheError("cannot open L2 replay cache: " + current_path_);
  }
  read_partition_header();
}

void L2CacheReader::read_partition_header() {
  std::array<char, cache_magic.size()> magic{};
  input_.read(magic.data(), magic.size());
  if (!input_ || magic != cache_magic) {
    throw L2CacheError("invalid L2 cache magic: " + current_path_);
  }
  const auto version = read_unsigned_le<std::uint16_t>(input_, "version");
  const auto depth = read_unsigned_le<std::uint16_t>(input_, "depth");
  const auto instrument = read_signed_le<InstrumentId>(input_, "instrument");
  const auto scale = read_signed_le<PriceTicks>(input_, "price scale");
  const auto tick = read_signed_le<PriceTicks>(input_, "tick size");
  const auto multiplier = read_signed_le<Quantity>(input_, "multiplier");
  records_remaining_ = read_unsigned_le<std::uint64_t>(input_, "record count");
  if (version != cache_version || depth != depth_ ||
      instrument != instrument_.instrument_id ||
      scale != instrument_.price_scale || tick != instrument_.tick_size_ticks ||
      multiplier != instrument_.contract_multiplier) {
    throw L2CacheError("L2 cache header does not match manifest: " +
                       current_path_);
  }
}

void L2CacheReader::validate_partition_end() {
  if (input_.is_open() && input_.peek() != std::char_traits<char>::eof()) {
    throw L2CacheError(
        "L2 cache contains trailing bytes after declared records: " +
        current_path_);
  }
}

bool L2CacheReader::next(L2InputEvent &event) {
  while (records_remaining_ == 0) {
    validate_partition_end();
    if (next_cache_index_ == cache_paths_.size()) {
      return false;
    }
    open_next_partition();
  }
  read_record(event);
  --records_remaining_;
  if (records_remaining_ == 0) {
    validate_partition_end();
  }
  if (has_previous_ && (event.event_ts_ns < previous_timestamp_ ||
                        event.merged_sequence <= previous_sequence_)) {
    throw L2CacheError("L2 cache chronology or sequence regression in " +
                       current_path_);
  }
  previous_timestamp_ = event.event_ts_ns;
  previous_sequence_ = event.merged_sequence;
  has_previous_ = true;
  return true;
}

void L2CacheReader::read_record(L2InputEvent &event) {
  const auto kind = read_unsigned_le<std::uint8_t>(input_, "event kind");
  const auto side = read_signed_le<std::int8_t>(input_, "side");
  (void)read_unsigned_le<std::uint16_t>(input_, "reserved record flags");
  event = L2InputEvent{};
  event.instrument_id = instrument_.instrument_id;
  event.event_ts_ns = read_signed_le<TimestampNs>(input_, "event timestamp");
  event.merged_sequence = read_unsigned_le<Sequence>(input_, "sequence");
  event.source_row = read_unsigned_le<Sequence>(input_, "source row");
  if (kind == static_cast<std::uint8_t>(L2EventKind::Snapshot)) {
    if (side != 0) {
      throw L2CacheError("snapshot cache record has a non-zero side");
    }
    event.kind = L2EventKind::Snapshot;
    event.bids.resize(depth_);
    event.asks.resize(depth_);
    for (std::size_t index = 0; index < depth_; ++index) {
      event.bids[index].price = read_signed_le<PriceTicks>(input_, "bid price");
      event.bids[index].quantity =
          read_signed_le<Quantity>(input_, "bid quantity");
      event.asks[index].price = read_signed_le<PriceTicks>(input_, "ask price");
      event.asks[index].quantity =
          read_signed_le<Quantity>(input_, "ask quantity");
    }
    return;
  }
  if (kind != static_cast<std::uint8_t>(L2EventKind::Trade) ||
      (side != static_cast<std::int8_t>(Side::Buy) &&
       side != static_cast<std::int8_t>(Side::Sell))) {
    throw L2CacheError("invalid L2 cache event kind or trade side");
  }
  event.kind = L2EventKind::Trade;
  event.side = static_cast<Side>(side);
  event.trade_price = read_signed_le<PriceTicks>(input_, "trade price");
  event.trade_quantity = read_signed_le<Quantity>(input_, "trade quantity");
  if (event.trade_price <= 0 || event.trade_quantity <= 0) {
    throw L2CacheError("trade cache record has non-positive price or quantity");
  }
}

} // namespace cmf::market
