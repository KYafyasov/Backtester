#include "market/MultiSourceManifest.hpp"

#include "main/json.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>
#include <type_traits>
#include <unordered_set>

namespace cmf::market {
namespace {

using Json = nlohmann::json;

[[nodiscard]] Json read_json(const std::string &path) {
  std::ifstream input(path);
  if (!input) {
    throw MultiSourceManifestError("cannot open multi-source manifest: " +
                                   path);
  }
  try {
    return Json::parse(input);
  } catch (const Json::exception &error) {
    throw MultiSourceManifestError("invalid multi-source manifest " + path +
                                   ": " + error.what());
  }
}

void validate_fields(const Json &object, std::string_view context,
                     std::initializer_list<std::string_view> allowed) {
  if (!object.is_object()) {
    throw MultiSourceManifestError(std::string(context) + " must be an object");
  }
  for (const auto &[name, value] : object.items()) {
    (void)value;
    if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
      throw MultiSourceManifestError(std::string(context) +
                                     " contains unknown field '" + name + "'");
    }
  }
}

[[nodiscard]] const Json &required(const Json &object, const char *name) {
  if (!object.contains(name)) {
    throw MultiSourceManifestError(
        std::string("multi-source manifest missing '") + name + "'");
  }
  return object.at(name);
}

template <typename Value>
[[nodiscard]] Value checked_integer(const Json &field, const char *name) {
  static_assert(std::is_integral_v<Value> &&
                !std::is_same_v<std::remove_cv_t<Value>, bool>);
  const auto range_error = [name]() {
    throw MultiSourceManifestError(std::string("field '") + name +
                                   "' is outside its supported range");
  };

  if (field.is_number_unsigned()) {
    const auto raw = field.get<std::uint64_t>();
    if constexpr (std::is_signed_v<Value>) {
      if (raw >
          static_cast<std::uint64_t>(std::numeric_limits<Value>::max())) {
        range_error();
      }
    } else if (raw > std::numeric_limits<Value>::max()) {
      range_error();
    }
    return static_cast<Value>(raw);
  }

  if (field.is_number_integer()) {
    const auto raw = field.get<std::int64_t>();
    if constexpr (std::is_signed_v<Value>) {
      if (raw < static_cast<std::int64_t>(std::numeric_limits<Value>::min()) ||
          raw > static_cast<std::int64_t>(std::numeric_limits<Value>::max())) {
        range_error();
      }
    } else {
      if (raw < 0 || static_cast<std::uint64_t>(raw) >
                         std::numeric_limits<Value>::max()) {
        range_error();
      }
    }
    return static_cast<Value>(raw);
  }

  throw MultiSourceManifestError(
      std::string("field '") + name + "' must be " +
      (std::is_signed_v<Value> ? "an integer" : "an unsigned integer"));
}

template <typename Value>
[[nodiscard]] Value integer(const Json &object, const char *name) {
  return checked_integer<Value>(required(object, name), name);
}

[[nodiscard]] std::string string(const Json &object, const char *name) {
  const auto &field = required(object, name);
  if (!field.is_string()) {
    throw MultiSourceManifestError(std::string("field '") + name +
                                   "' must be a string");
  }
  const auto value = field.get<std::string>();
  if (value.empty()) {
    throw MultiSourceManifestError(std::string("field '") + name +
                                   "' must not be empty");
  }
  return value;
}

[[nodiscard]] bool valid_sha256(std::string_view value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isdigit(character) != 0 ||
                  (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] std::filesystem::path
resolve_child(const std::filesystem::path &root, const std::string &value) {
  const std::filesystem::path relative(value);
  if (relative.empty() || relative.is_absolute()) {
    throw MultiSourceManifestError("source path must be relative");
  }
  for (const auto &component : relative) {
    if (component == "..") {
      throw MultiSourceManifestError("source path must not contain '..'");
    }
  }
  const auto resolved = std::filesystem::weakly_canonical(root / relative);
  const auto canonical_root = std::filesystem::weakly_canonical(root);
  const auto mismatch =
      std::mismatch(canonical_root.begin(), canonical_root.end(),
                    resolved.begin(), resolved.end());
  if (mismatch.first != canonical_root.end()) {
    throw MultiSourceManifestError("source path escapes manifest directory");
  }
  return resolved;
}

[[nodiscard]] MultiSourceSpec parse_source(const Json &source,
                                           const std::filesystem::path &root) {
  validate_fields(source, "multi-source entry",
                  {"source_id", "source_priority", "format", "path", "sha256",
                   "bytes", "record_count", "group_count", "min_event_ts_ns",
                   "max_event_ts_ns", "min_source_sequence",
                   "max_source_sequence", "instrument_ids"});
  MultiSourceSpec result;
  result.source_id = integer<SourceId>(source, "source_id");
  result.source_priority = integer<SourcePriority>(source, "source_priority");
  if (result.source_id == 0 || result.source_priority == 0) {
    throw MultiSourceManifestError("source ID and priority must be positive");
  }
  result.format = string(source, "format");
  if (result.format != "cmf-l2-parquet-cache-v1") {
    throw MultiSourceManifestError(
        "restricted multi-source mode supports only L2 manifests");
  }
  const auto relative_path = string(source, "path");
  result.manifest_path = resolve_child(root, relative_path).string();
  result.sha256 = string(source, "sha256");
  if (!valid_sha256(result.sha256)) {
    throw MultiSourceManifestError("source sha256 must be lowercase hex");
  }
  result.bytes = integer<std::uint64_t>(source, "bytes");
  result.record_count = integer<std::uint64_t>(source, "record_count");
  result.group_count = integer<std::uint64_t>(source, "group_count");
  result.min_event_ts_ns = integer<TimestampNs>(source, "min_event_ts_ns");
  result.max_event_ts_ns = integer<TimestampNs>(source, "max_event_ts_ns");
  result.min_source_sequence = integer<Sequence>(source, "min_source_sequence");
  result.max_source_sequence = integer<Sequence>(source, "max_source_sequence");
  if (result.bytes == 0 || result.record_count == 0 ||
      result.group_count == 0 ||
      result.min_event_ts_ns > result.max_event_ts_ns ||
      result.min_source_sequence == 0 ||
      result.min_source_sequence > result.max_source_sequence) {
    throw MultiSourceManifestError("source counts or bounds are invalid");
  }
  const auto &ids = required(source, "instrument_ids");
  if (!ids.is_array() || ids.empty()) {
    throw MultiSourceManifestError("instrument_ids must be a non-empty array");
  }
  for (const auto &id : ids) {
    const auto value = checked_integer<InstrumentId>(id, "instrument_id");
    if (value <= 0 ||
        std::find(result.instrument_ids.begin(), result.instrument_ids.end(),
                  value) != result.instrument_ids.end()) {
      throw MultiSourceManifestError(
          "instrument_ids must be positive and unique per source");
    }
    result.instrument_ids.push_back(value);
  }
  if (!std::filesystem::is_regular_file(result.manifest_path)) {
    throw MultiSourceManifestError("source manifest does not exist: " +
                                   result.manifest_path);
  }
  if (std::filesystem::file_size(result.manifest_path) != result.bytes) {
    throw MultiSourceManifestError("source manifest byte size mismatch: " +
                                   result.manifest_path);
  }
  if (L2CacheReader::sha256_file(result.manifest_path) != result.sha256) {
    throw MultiSourceManifestError("source manifest SHA-256 mismatch: " +
                                   result.manifest_path);
  }
  result.l2_metadata = L2CacheReader::inspect_manifest(result.manifest_path);
  const auto &metadata = result.l2_metadata;
  if (result.instrument_ids.size() != 1 ||
      result.instrument_ids.front() != metadata.instrument.instrument_id ||
      result.record_count != metadata.total_rows ||
      result.group_count != metadata.total_rows ||
      result.min_event_ts_ns != metadata.min_event_ts_ns ||
      result.max_event_ts_ns != metadata.max_event_ts_ns ||
      result.min_source_sequence != metadata.min_merged_sequence ||
      result.max_source_sequence != metadata.max_merged_sequence) {
    throw MultiSourceManifestError(
        "source declaration does not match child L2 manifest: " +
        result.manifest_path);
  }
  return result;
}

} // namespace

bool is_multi_source_manifest(const std::string &path) {
  try {
    const auto manifest = read_json(path);
    return manifest.is_object() && manifest.contains("format") &&
           manifest.at("format") == "cmf-multi-source-v1";
  } catch (const MultiSourceManifestError &) {
    return false;
  }
}

MultiSourceManifest read_multi_source_manifest(const std::string &path) {
  const auto manifest = read_json(path);
  validate_fields(manifest, "multi-source manifest",
                  {"format", "manifest_version", "dataset_id", "sources",
                   "expected_global_group_count"});
  if (required(manifest, "format") != "cmf-multi-source-v1" ||
      integer<std::uint16_t>(manifest, "manifest_version") != 1) {
    throw MultiSourceManifestError("unsupported multi-source manifest version");
  }
  MultiSourceManifest result;
  result.dataset_id = string(manifest, "dataset_id");
  result.expected_global_group_count =
      integer<std::uint64_t>(manifest, "expected_global_group_count");
  const auto &sources = required(manifest, "sources");
  if (!sources.is_array() || sources.size() < 2) {
    throw MultiSourceManifestError(
        "multi-source manifest must contain at least two sources");
  }
  const auto root = std::filesystem::absolute(path).parent_path();
  std::unordered_set<SourceId> source_ids;
  std::unordered_set<SourcePriority> priorities;
  std::unordered_set<InstrumentId> instruments;
  std::uint64_t total_groups{};
  result.verified_metadata = true;
  result.sources.reserve(sources.size());
  for (const auto &source : sources) {
    auto parsed = parse_source(source, root);
    if (result.sources.empty()) {
      result.timestamp_semantics = parsed.l2_metadata.timestamp_semantics;
    } else if (parsed.l2_metadata.timestamp_semantics !=
               result.timestamp_semantics) {
      throw MultiSourceManifestError(
          "multi-source children have incompatible timestamp_semantics: " +
          std::string(timestamp_semantics_name(result.timestamp_semantics)) +
          " versus " +
          std::string(timestamp_semantics_name(
              parsed.l2_metadata.timestamp_semantics)));
    }
    if (!source_ids.insert(parsed.source_id).second) {
      throw MultiSourceManifestError("duplicate source_id");
    }
    if (!priorities.insert(parsed.source_priority).second) {
      throw MultiSourceManifestError("duplicate source_priority");
    }
    for (const auto instrument_id : parsed.instrument_ids) {
      if (!instruments.insert(instrument_id).second) {
        throw MultiSourceManifestError(
            "instrument_id is owned by more than one source");
      }
    }
    if (parsed.group_count >
        std::numeric_limits<std::uint64_t>::max() - total_groups) {
      throw MultiSourceManifestError("global group count overflow");
    }
    total_groups += parsed.group_count;
    result.verified_metadata =
        result.verified_metadata && parsed.l2_metadata.verified_metadata;
    result.sources.push_back(std::move(parsed));
  }
  if (result.expected_global_group_count == 0 ||
      result.expected_global_group_count != total_groups) {
    throw MultiSourceManifestError(
        "expected_global_group_count does not match child sources");
  }
  return result;
}

} // namespace cmf::market
