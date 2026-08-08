#include "market/L2CacheReader.hpp"

#include "main/json.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <type_traits>

namespace cmf::market {
namespace {

using Json = nlohmann::json;
constexpr std::array<char, 8> cache_magic{'C', 'M', 'F', 'L',
                                          '2', 'C', '0', '1'};
constexpr std::uint16_t cache_version = 1;

struct CacheFile {
  std::string path;
  std::string sha256;
};

struct CacheIndex {
  std::uint64_t snapshot_rows{};
  std::uint64_t trade_rows{};
  TimestampNs min_event_ts_ns{};
  TimestampNs max_event_ts_ns{};
  Sequence min_merged_sequence{};
  Sequence max_merged_sequence{};
};

class Sha256 {
public:
  void update(const unsigned char *data, std::size_t size) {
    if (size > (std::numeric_limits<std::uint64_t>::max() - bit_count_) / 8U) {
      throw L2CacheError("L2 cache is too large to hash");
    }
    bit_count_ += static_cast<std::uint64_t>(size) * 8U;
    for (std::size_t index = 0; index < size; ++index) {
      block_[block_size_++] = data[index];
      if (block_size_ == block_.size()) {
        transform();
        block_size_ = 0;
      }
    }
  }

  [[nodiscard]] std::string finish() {
    block_[block_size_++] = 0x80U;
    if (block_size_ > 56) {
      std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_),
                block_.end(), 0);
      transform();
      block_size_ = 0;
    }
    std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_),
              block_.begin() + 56, 0);
    for (std::size_t index = 0; index < sizeof(bit_count_); ++index) {
      block_[63 - index] =
          static_cast<unsigned char>(bit_count_ >> (index * 8U));
    }
    transform();

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto word : state_) {
      output << std::setw(8) << word;
    }
    return output.str();
  }

private:
  void transform() {
    static constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
        0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
        0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
        0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
        0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
        0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
        0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
        0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
        0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      const std::size_t offset = index * 4;
      words[index] = (static_cast<std::uint32_t>(block_[offset]) << 24U) |
                     (static_cast<std::uint32_t>(block_[offset + 1]) << 16U) |
                     (static_cast<std::uint32_t>(block_[offset + 2]) << 8U) |
                     static_cast<std::uint32_t>(block_[offset + 3]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      const auto s0 = std::rotr(words[index - 15], 7) ^
                      std::rotr(words[index - 15], 18) ^
                      (words[index - 15] >> 3U);
      const auto s1 = std::rotr(words[index - 2], 17) ^
                      std::rotr(words[index - 2], 19) ^
                      (words[index - 2] >> 10U);
      words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }

    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];
    auto e = state_[4];
    auto f = state_[5];
    auto g = state_[6];
    auto h = state_[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const auto choice = (e & f) ^ (~e & g);
      const auto temporary1 =
          h + sum1 + choice + constants[index] + words[index];
      const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temporary2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                      0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                      0x1f83d9abU, 0x5be0cd19U};
  std::array<unsigned char, 64> block_{};
  std::size_t block_size_{};
  std::uint64_t bit_count_{};
};

[[nodiscard]] std::string sha256_file(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw L2CacheError("cannot open L2 replay cache for hashing: " + path);
  }
  Sha256 hash;
  std::array<unsigned char, 64 * 1024> buffer{};
  while (input) {
    input.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
    const auto count = input.gcount();
    if (count > 0) {
      hash.update(buffer.data(), static_cast<std::size_t>(count));
    }
  }
  if (!input.eof()) {
    throw L2CacheError("failed while hashing L2 replay cache: " + path);
  }
  return hash.finish();
}

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

[[nodiscard]] bool one_of(std::string_view value,
                          std::initializer_list<std::string_view> allowed);

[[nodiscard]] const Json &required(const Json &object, const char *name) {
  if (!object.is_object() || !object.contains(name)) {
    throw L2CacheError(std::string("L2 manifest missing '") + name + "'");
  }
  return object.at(name);
}

void validate_object_fields(
    const Json &object, std::string_view context,
    std::initializer_list<std::string_view> allowed_fields) {
  if (!object.is_object()) {
    throw L2CacheError(std::string(context) + " must be an object");
  }
  for (const auto &[name, value] : object.items()) {
    (void)value;
    if (!one_of(name, allowed_fields)) {
      throw L2CacheError(std::string(context) +
                         " contains unsupported field '" + name + "'");
    }
  }
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

[[nodiscard]] std::string manifest_string(const Json &object,
                                          const char *name) {
  const auto &value = required(object, name);
  if (!value.is_string() || value.get_ref<const std::string &>().empty()) {
    throw L2CacheError(std::string("L2 manifest field '") + name +
                       "' must be a non-empty string");
  }
  return value.get<std::string>();
}

[[nodiscard]] bool manifest_boolean(const Json &object, const char *name) {
  const auto &value = required(object, name);
  if (!value.is_boolean()) {
    throw L2CacheError(std::string("L2 manifest field '") + name +
                       "' must be boolean");
  }
  return value.get<bool>();
}

[[nodiscard]] bool one_of(std::string_view value,
                          std::initializer_list<std::string_view> allowed) {
  return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}

[[nodiscard]] std::string
manifest_enum(const Json &object, const char *name,
              std::initializer_list<std::string_view> allowed) {
  const auto value = manifest_string(object, name);
  if (!one_of(value, allowed)) {
    throw L2CacheError(std::string("L2 manifest field '") + name +
                       "' has an unsupported value");
  }
  return value;
}

void validate_nullable_string(const Json &object, const char *name,
                              bool required_for_verified) {
  const auto &value = required(object, name);
  const bool valid_string =
      value.is_string() && !value.get_ref<const std::string &>().empty();
  if (!value.is_null() && !valid_string) {
    throw L2CacheError(std::string("L2 manifest field '") + name +
                       "' must be null or a non-empty string");
  }
  if (required_for_verified && !valid_string) {
    throw L2CacheError(std::string("verified L2 manifest field '") + name +
                       "' must be a non-empty string");
  }
}

[[nodiscard]] bool valid_sha256(std::string_view value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isdigit(character) != 0 ||
                  (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] std::string manifest_sha256(const Json &object,
                                          const char *name) {
  const auto value = manifest_string(object, name);
  if (!valid_sha256(value)) {
    throw L2CacheError(std::string("L2 manifest field '") + name +
                       "' must be lowercase SHA-256 hexadecimal");
  }
  return value;
}

[[nodiscard]] std::string manifest_relative_path(const Json &object,
                                                 const char *name) {
  const auto value = manifest_string(object, name);
  const std::filesystem::path path(value);
  if (path.is_absolute() ||
      std::find(path.begin(), path.end(), "..") != path.end()) {
    throw L2CacheError(std::string("L2 manifest field '") + name +
                       "' must remain relative to the dataset");
  }
  return value;
}

void validate_source_file(const Json &source) {
  validate_object_fields(source, "L2 source file",
                         {"path", "bytes", "rows", "sha256"});
  (void)manifest_relative_path(source, "path");
  if (manifest_integer<std::uint64_t>(source, "bytes") == 0 ||
      manifest_integer<std::uint64_t>(source, "rows") == 0) {
    throw L2CacheError("L2 source file bytes and rows must be positive");
  }
  (void)manifest_sha256(source, "sha256");
}

void validate_partition_shape(const Json &partition) {
  validate_object_fields(
      partition, "L2 partition",
      {"date", "min_event_ts_ns", "max_event_ts_ns", "min_merged_sequence",
       "max_merged_sequence", "snapshot_rows", "trade_rows", "book_parquet",
       "book_parquet_sha256", "trade_parquet", "trade_parquet_sha256",
       "replay_cache", "replay_cache_bytes", "replay_cache_sha256"});
  const auto date = manifest_string(partition, "date");
  if (date.size() != 10 || date[4] != '-' || date[7] != '-' ||
      !std::all_of(date.begin(), date.end(),
                   [index = std::size_t{}](unsigned char character) mutable {
                     const auto current = index++;
                     return current == 4 || current == 7
                                ? character == '-'
                                : std::isdigit(character) != 0;
                   })) {
    throw L2CacheError("L2 partition date must use YYYY-MM-DD");
  }
  (void)manifest_integer<TimestampNs>(partition, "min_event_ts_ns");
  (void)manifest_integer<TimestampNs>(partition, "max_event_ts_ns");
  (void)manifest_integer<Sequence>(partition, "min_merged_sequence");
  (void)manifest_integer<Sequence>(partition, "max_merged_sequence");
  (void)manifest_integer<std::uint64_t>(partition, "snapshot_rows");
  (void)manifest_integer<std::uint64_t>(partition, "trade_rows");
  (void)manifest_relative_path(partition, "book_parquet");
  (void)manifest_sha256(partition, "book_parquet_sha256");
  (void)manifest_relative_path(partition, "trade_parquet");
  (void)manifest_sha256(partition, "trade_parquet_sha256");
  (void)manifest_relative_path(partition, "replay_cache");
  if (manifest_integer<std::uint64_t>(partition, "replay_cache_bytes") == 0) {
    throw L2CacheError("L2 replay cache bytes must be positive");
  }
  (void)manifest_sha256(partition, "replay_cache_sha256");
}

void validate_positive_number(const Json &object, const char *name) {
  const auto &value = required(object, name);
  if (!value.is_number() || value.get<double>() <= 0.0) {
    throw L2CacheError(std::string("L2 manifest field '") + name +
                       "' must be a positive number");
  }
}

[[nodiscard]] L2DatasetMetadata parse_metadata(const Json &manifest) {
  validate_object_fields(manifest, "L2 manifest",
                         {"format",
                          "manifest_version",
                          "data_schema_version",
                          "dataset_id",
                          "verified_metadata",
                          "source_provider",
                          "venue",
                          "symbol",
                          "instrument_id",
                          "timestamp_unit",
                          "timestamp_semantics",
                          "timezone",
                          "price_scale",
                          "tick_size_ticks",
                          "contract_multiplier",
                          "book_depth",
                          "trade_side_semantics",
                          "same_timestamp_policy",
                          "converter_version",
                          "created_at",
                          "source_files",
                          "partitions",
                          "conversion_stats"});
  if (required(manifest, "format") != "cmf-l2-parquet-cache-v1" ||
      manifest_integer<std::uint16_t>(manifest, "manifest_version") != 1 ||
      manifest_integer<std::uint16_t>(manifest, "data_schema_version") != 1) {
    throw L2CacheError("unsupported L2 manifest format or schema version");
  }
  L2DatasetMetadata metadata;
  metadata.instrument = InstrumentMeta{
      manifest_integer<InstrumentId>(manifest, "instrument_id"),
      manifest_integer<PriceTicks>(manifest, "tick_size_ticks"),
      manifest_integer<PriceTicks>(manifest, "price_scale"),
      manifest_integer<Quantity>(manifest, "contract_multiplier")};
  if (metadata.instrument.instrument_id <= 0 ||
      metadata.instrument.tick_size_ticks <= 0 ||
      metadata.instrument.price_scale <= 0 ||
      metadata.instrument.contract_multiplier <= 0) {
    throw L2CacheError("L2 manifest instrument metadata must be positive");
  }
  metadata.dataset_id = manifest_string(manifest, "dataset_id");
  metadata.verified_metadata = manifest_boolean(manifest, "verified_metadata");
  validate_nullable_string(manifest, "source_provider",
                           metadata.verified_metadata);
  validate_nullable_string(manifest, "venue", metadata.verified_metadata);
  validate_nullable_string(manifest, "symbol", metadata.verified_metadata);
  (void)manifest_enum(manifest, "timestamp_unit", {"s", "ms", "us", "ns"});
  const auto timestamp_semantics =
      manifest_enum(manifest, "timestamp_semantics",
                    {"exchange", "receive", "local_receive", "unknown"});
  if (manifest_string(manifest, "timezone") != "UTC") {
    throw L2CacheError("L2 manifest timezone must be UTC");
  }
  const auto side_semantics = manifest_enum(manifest, "trade_side_semantics",
                                            {"aggressor", "maker", "unknown"});
  (void)manifest_enum(manifest, "same_timestamp_policy",
                      {"snapshot_first", "trade_first"});
  (void)manifest_string(manifest, "converter_version");
  (void)manifest_string(manifest, "created_at");
  if (metadata.verified_metadata &&
      (timestamp_semantics == "unknown" || side_semantics == "unknown")) {
    throw L2CacheError("verified L2 manifest cannot contain unknown semantics");
  }
  const auto depth = manifest_integer<std::uint16_t>(manifest, "book_depth");
  if (depth == 0 || depth > 255) {
    throw L2CacheError("L2 manifest book_depth must be in [1, 255]");
  }
  const auto &source_files = required(manifest, "source_files");
  if (!source_files.is_array() || source_files.size() != 2) {
    throw L2CacheError(
        "L2 manifest source_files must contain lob.csv and trades.csv");
  }
  for (const auto &source : source_files) {
    validate_source_file(source);
  }
  const auto &partitions = required(manifest, "partitions");
  if (!partitions.is_array() || partitions.empty()) {
    throw L2CacheError("L2 manifest partitions must be a non-empty array");
  }
  for (const auto &partition : partitions) {
    validate_partition_shape(partition);
    metadata.snapshot_rows +=
        manifest_integer<std::uint64_t>(partition, "snapshot_rows");
    metadata.trade_rows +=
        manifest_integer<std::uint64_t>(partition, "trade_rows");
  }
  const auto &conversion_stats = required(manifest, "conversion_stats");
  validate_object_fields(conversion_stats, "L2 conversion_stats",
                         {"rows", "elapsed_seconds", "rows_per_second",
                          "input_bytes", "output_bytes_before_manifest"});
  metadata.total_rows =
      manifest_integer<std::uint64_t>(conversion_stats, "rows");
  if (metadata.total_rows == 0 ||
      manifest_integer<std::uint64_t>(conversion_stats, "input_bytes") == 0 ||
      manifest_integer<std::uint64_t>(conversion_stats,
                                      "output_bytes_before_manifest") == 0) {
    throw L2CacheError("L2 conversion row and byte counts must be positive");
  }
  validate_positive_number(conversion_stats, "elapsed_seconds");
  validate_positive_number(conversion_stats, "rows_per_second");
  return metadata;
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

void skip_bytes(std::istream &input, std::uint64_t count,
                const std::string &context) {
  if (count >
      static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
    throw L2CacheError("L2 cache record is too large while reading " + context);
  }
  input.ignore(static_cast<std::streamsize>(count));
  if (!input) {
    throw L2CacheError("truncated L2 cache while reading " + context);
  }
}

[[nodiscard]] std::string utc_date(TimestampNs timestamp_ns) {
  using namespace std::chrono;
  const auto day =
      floor<days>(sys_time<nanoseconds>{nanoseconds{timestamp_ns}});
  const year_month_day date{day};
  std::ostringstream output;
  output << std::setfill('0') << std::setw(4) << static_cast<int>(date.year())
         << '-' << std::setw(2) << static_cast<unsigned>(date.month()) << '-'
         << std::setw(2) << static_cast<unsigned>(date.day());
  return output.str();
}

[[nodiscard]] CacheIndex
scan_cache_index(const CacheFile &cache, const InstrumentMeta &instrument,
                 std::uint16_t expected_depth, TimestampNs &previous_timestamp,
                 Sequence &previous_sequence, bool &has_previous) {
  std::ifstream input(cache.path, std::ios::binary);
  if (!input) {
    throw L2CacheError("cannot open L2 replay cache: " + cache.path);
  }
  std::array<char, cache_magic.size()> magic{};
  input.read(magic.data(), magic.size());
  if (!input || magic != cache_magic) {
    throw L2CacheError("invalid L2 cache magic: " + cache.path);
  }
  const auto version = read_unsigned_le<std::uint16_t>(input, "version");
  const auto depth = read_unsigned_le<std::uint16_t>(input, "depth");
  const auto cache_instrument =
      read_signed_le<InstrumentId>(input, "instrument");
  const auto scale = read_signed_le<PriceTicks>(input, "price scale");
  const auto tick = read_signed_le<PriceTicks>(input, "tick size");
  const auto multiplier = read_signed_le<Quantity>(input, "multiplier");
  const auto record_count =
      read_unsigned_le<std::uint64_t>(input, "record count");
  if (version != cache_version || depth != expected_depth ||
      cache_instrument != instrument.instrument_id ||
      scale != instrument.price_scale || tick != instrument.tick_size_ticks ||
      multiplier != instrument.contract_multiplier) {
    throw L2CacheError("L2 cache header does not match manifest: " +
                       cache.path);
  }
  if (record_count == 0) {
    throw L2CacheError("L2 replay cache partition is empty: " + cache.path);
  }

  CacheIndex index;
  for (std::uint64_t row = 0; row < record_count; ++row) {
    const auto kind = read_unsigned_le<std::uint8_t>(input, "event kind");
    const auto side = read_signed_le<std::int8_t>(input, "side");
    (void)read_unsigned_le<std::uint16_t>(input, "reserved record flags");
    const auto timestamp =
        read_signed_le<TimestampNs>(input, "event timestamp");
    const auto sequence = read_unsigned_le<Sequence>(input, "sequence");
    (void)read_unsigned_le<Sequence>(input, "source row");

    if (has_previous && (timestamp < previous_timestamp ||
                         sequence != previous_sequence + Sequence{1})) {
      throw L2CacheError(
          "L2 cache chronology is not globally ordered and contiguous: " +
          cache.path);
    }
    if (sequence == 0 || (!has_previous && sequence != Sequence{1})) {
      throw L2CacheError(
          "L2 cache sequence must start at one and remain positive: " +
          cache.path);
    }
    if (row == 0) {
      index.min_event_ts_ns = timestamp;
      index.min_merged_sequence = sequence;
    }
    index.max_event_ts_ns = timestamp;
    index.max_merged_sequence = sequence;
    previous_timestamp = timestamp;
    previous_sequence = sequence;
    has_previous = true;

    if (kind == static_cast<std::uint8_t>(L2EventKind::Snapshot)) {
      if (side != 0) {
        throw L2CacheError("snapshot cache record has a non-zero side");
      }
      ++index.snapshot_rows;
      constexpr std::uint64_t values_per_level = 4;
      skip_bytes(input,
                 static_cast<std::uint64_t>(depth) * values_per_level *
                     sizeof(std::int64_t),
                 "snapshot payload");
    } else if (kind == static_cast<std::uint8_t>(L2EventKind::Trade) &&
               (side == static_cast<std::int8_t>(Side::Buy) ||
                side == static_cast<std::int8_t>(Side::Sell))) {
      ++index.trade_rows;
      skip_bytes(input, 2U * sizeof(std::int64_t), "trade payload");
    } else {
      throw L2CacheError("invalid L2 cache event kind or trade side");
    }
  }
  if (input.peek() != std::char_traits<char>::eof()) {
    throw L2CacheError(
        "L2 cache contains trailing bytes after declared records: " +
        cache.path);
  }
  return index;
}

[[nodiscard]] std::vector<CacheFile>
parse_cache_paths(const Json &manifest, const std::string &manifest_path) {
  const auto &partitions = required(manifest, "partitions");
  if (!partitions.is_array() || partitions.empty()) {
    throw L2CacheError("L2 manifest partitions must be a non-empty array");
  }
  const auto root = std::filesystem::absolute(manifest_path).parent_path();
  std::vector<CacheFile> paths;
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
    paths.push_back(CacheFile{
        path.string(), manifest_sha256(partition, "replay_cache_sha256")});
  }
  return paths;
}

void validate_manifest_cache_index(const Json &manifest,
                                   const std::string &manifest_path,
                                   const InstrumentMeta &instrument,
                                   std::uint16_t depth) {
  const auto caches = parse_cache_paths(manifest, manifest_path);
  const auto &partitions = required(manifest, "partitions");
  std::uint64_t total_snapshots{};
  std::uint64_t total_trades{};
  TimestampNs previous_timestamp{};
  Sequence previous_sequence{};
  bool has_previous = false;

  for (std::size_t position = 0; position < caches.size(); ++position) {
    const auto actual =
        scan_cache_index(caches[position], instrument, depth,
                         previous_timestamp, previous_sequence, has_previous);
    const auto &declared = partitions[position];
    const auto mismatch =
        actual.snapshot_rows !=
            manifest_integer<std::uint64_t>(declared, "snapshot_rows") ||
        actual.trade_rows !=
            manifest_integer<std::uint64_t>(declared, "trade_rows") ||
        actual.min_event_ts_ns !=
            manifest_integer<TimestampNs>(declared, "min_event_ts_ns") ||
        actual.max_event_ts_ns !=
            manifest_integer<TimestampNs>(declared, "max_event_ts_ns") ||
        actual.min_merged_sequence !=
            manifest_integer<Sequence>(declared, "min_merged_sequence") ||
        actual.max_merged_sequence !=
            manifest_integer<Sequence>(declared, "max_merged_sequence");
    if (mismatch) {
      throw L2CacheError(
          "L2 manifest partition index does not match replay cache: " +
          caches[position].path);
    }
    const auto declared_date = manifest_string(declared, "date");
    if (utc_date(actual.min_event_ts_ns) != declared_date ||
        utc_date(actual.max_event_ts_ns) != declared_date) {
      throw L2CacheError(
          "L2 manifest partition date does not match replay cache: " +
          caches[position].path);
    }
    total_snapshots += actual.snapshot_rows;
    total_trades += actual.trade_rows;
  }

  const auto total_rows = total_snapshots + total_trades;
  if (manifest_integer<std::uint64_t>(required(manifest, "conversion_stats"),
                                      "rows") != total_rows) {
    throw L2CacheError(
        "L2 manifest conversion_stats.rows does not match replay caches");
  }
  const auto &source_files = required(manifest, "source_files");
  if (manifest_integer<std::uint64_t>(source_files[0], "rows") !=
          total_snapshots ||
      manifest_integer<std::uint64_t>(source_files[1], "rows") !=
          total_trades) {
    throw L2CacheError(
        "L2 manifest source row counts do not match replay caches");
  }
}

[[nodiscard]] std::vector<CacheFile>
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

[[nodiscard]] std::vector<std::string>
verify_cache_hashes(std::vector<CacheFile> selected) {
  std::vector<std::string> paths;
  paths.reserve(selected.size());
  for (const auto &cache : selected) {
    if (sha256_file(cache.path) != cache.sha256) {
      throw L2CacheError("L2 replay cache SHA-256 mismatch: " + cache.path);
    }
    paths.push_back(cache.path);
  }
  return paths;
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
  return inspect_manifest(manifest_path).instrument;
}

L2DatasetMetadata
L2CacheReader::inspect_manifest(const std::string &manifest_path) {
  return parse_metadata(read_json(manifest_path));
}

L2CacheReader::L2CacheReader(std::string manifest_path,
                             InstrumentMap instruments, DateRange range,
                             bool allow_unverified_metadata)
    : manifest_path_(std::move(manifest_path)) {
  const auto manifest = read_json(manifest_path_);
  const auto metadata = parse_metadata(manifest);
  instrument_ = metadata.instrument;
  if (!metadata.verified_metadata && !allow_unverified_metadata) {
    throw L2CacheError(
        "unverified L2 metadata requires allow_unverified_metadata=true");
  }
  depth_ = manifest_integer<std::uint16_t>(manifest, "book_depth");
  const auto configured = instruments.find(instrument_.instrument_id);
  if (configured == instruments.end() ||
      configured->second.tick_size_ticks != instrument_.tick_size_ticks ||
      configured->second.price_scale != instrument_.price_scale ||
      configured->second.contract_multiplier !=
          instrument_.contract_multiplier) {
    throw L2CacheError(
        "runtime instrument metadata does not match L2 manifest");
  }
  validate_manifest_cache_index(manifest, manifest_path_, instrument_, depth_);
  cache_paths_ =
      verify_cache_hashes(select_cache_paths(manifest, manifest_path_, range));
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
