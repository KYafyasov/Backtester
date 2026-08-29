#include "market/JsonlReader.hpp"

#include "main/json.hpp"
#include "market/Parsing.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <utility>

namespace cmf::market {
namespace {

using Json = nlohmann::json;

[[nodiscard]] std::string make_error_message(const std::string &path,
                                             std::size_t row,
                                             const std::string &message) {
  std::ostringstream output;
  output << path << ':' << row << ": " << message;
  return output.str();
}

[[nodiscard]] const Json &required(const Json &object, const char *key) {
  if (!object.is_object() || !object.contains(key)) {
    throw std::invalid_argument(std::string("missing required field '") + key +
                                "'");
  }
  return object.at(key);
}

[[nodiscard]] std::string required_string(const Json &object, const char *key) {
  const Json &value = required(object, key);
  if (!value.is_string()) {
    throw std::invalid_argument(std::string("field '") + key +
                                "' must be a string");
  }
  return value.get<std::string>();
}

[[nodiscard]] std::int64_t required_int64(const Json &object, const char *key) {
  const Json &value = required(object, key);
  if (value.is_number_unsigned()) {
    const auto unsigned_value = value.get<std::uint64_t>();
    if (unsigned_value >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      throw std::out_of_range(std::string("field '") + key +
                              "' is outside int64 range");
    }
    return static_cast<std::int64_t>(unsigned_value);
  }
  if (!value.is_number_integer()) {
    throw std::invalid_argument(std::string("field '") + key +
                                "' must be an integer");
  }
  return value.get<std::int64_t>();
}

[[nodiscard]] Sequence required_sequence(const Json &object, const char *key) {
  const Json &value = required(object, key);
  if (!value.is_number_unsigned() && !value.is_number_integer()) {
    throw std::invalid_argument(std::string("field '") + key +
                                "' must be an unsigned integer");
  }
  if (value.is_number_unsigned()) {
    return value.get<Sequence>();
  }
  if (value.is_number_integer()) {
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value < 0) {
      throw std::invalid_argument(std::string("field '") + key +
                                  "' must be non-negative");
    }
    return static_cast<Sequence>(signed_value);
  }
  throw std::invalid_argument(std::string("field '") + key +
                              "' must be an unsigned integer");
}

[[nodiscard]] ExchangeOrderId parse_order_id(const Json &object) {
  const Json &value = required(object, "order_id");
  if (value.is_number_unsigned()) {
    return value.get<ExchangeOrderId>();
  }
  if (value.is_number_integer()) {
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value < 0) {
      throw std::invalid_argument("field 'order_id' must be non-negative");
    }
    return static_cast<ExchangeOrderId>(signed_value);
  }
  if (!value.is_string()) {
    throw std::invalid_argument(
        "field 'order_id' must be a decimal string or integer");
  }

  const std::string text = value.get<std::string>();
  if (text.empty()) {
    throw std::invalid_argument("field 'order_id' must not be empty");
  }
  ExchangeOrderId result = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      throw std::invalid_argument(
          "field 'order_id' contains a non-decimal character");
    }
    const auto digit = static_cast<ExchangeOrderId>(character - '0');
    if (result > (std::numeric_limits<ExchangeOrderId>::max() - digit) / 10U) {
      throw std::out_of_range("field 'order_id' overflows uint64");
    }
    result = result * 10U + digit;
  }
  return result;
}

[[nodiscard]] MarketAction parse_action(const Json &object) {
  const std::string value = required_string(object, "action");
  if (value.size() != 1) {
    throw std::invalid_argument("field 'action' must contain one character");
  }
  switch (value.front()) {
  case 'A':
    return MarketAction::Add;
  case 'C':
    return MarketAction::Cancel;
  case 'M':
    return MarketAction::Modify;
  case 'T':
    return MarketAction::Trade;
  case 'F':
    return MarketAction::Fill;
  case 'R':
    return MarketAction::Clear;
  default:
    throw std::invalid_argument("field 'action' has an unsupported value");
  }
}

[[nodiscard]] Side parse_side(const Json &object, bool required_value) {
  if (!object.contains("side")) {
    if (required_value) {
      throw std::invalid_argument("missing required field 'side'");
    }
    return Side::None;
  }
  const Json &json_side = object.at("side");
  if (!json_side.is_string()) {
    throw std::invalid_argument("field 'side' must be a string");
  }
  const std::string value = json_side.get<std::string>();
  if (value == "B") {
    return Side::Buy;
  }
  if (value == "A" || value == "S") {
    return Side::Sell;
  }
  if (!required_value && (value.empty() || value == "N")) {
    return Side::None;
  }
  throw std::invalid_argument("field 'side' has an unsupported value");
}

[[nodiscard]] std::uint32_t optional_flags(const Json &object) {
  if (!object.contains("flags")) {
    return 0;
  }
  const auto value = required_int64(object, "flags");
  if (value < 0 || static_cast<std::uint64_t>(value) >
                       std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("field 'flags' is outside uint32 range");
  }
  return static_cast<std::uint32_t>(value);
}

} // namespace

SourceError::SourceError(std::string path, std::size_t row, std::string context,
                         std::string message)
    : std::runtime_error(make_error_message(path, row, message)),
      path_(std::move(path)), row_(row), context_(std::move(context)) {}

JsonlReader::JsonlReader(std::string path, InstrumentMap instruments)
    : path_(std::move(path)), instruments_(std::move(instruments)),
      input_(path_) {
  if (!input_.is_open()) {
    throw SourceError(path_, 0, {}, "cannot open source file");
  }
}

JsonlReader::JsonlReader(std::string path, UniformInstrumentPolicy policy)
    : path_(std::move(path)), uniform_policy_(policy), input_(path_) {
  if (policy.price_scale <= 0 || policy.tick_size_ticks <= 0 ||
      policy.contract_multiplier <= 0) {
    throw SourceError(path_, 0, {}, "invalid uniform instrument price policy");
  }
  if (!input_.is_open()) {
    throw SourceError(path_, 0, {}, "cannot open source file");
  }
}

InstrumentMeta JsonlReader::instrument_meta(InstrumentId id) const {
  const auto iterator = instruments_.find(id);
  if (iterator != instruments_.end()) {
    const InstrumentMeta &meta = iterator->second;
    if (meta.instrument_id != id || meta.price_scale <= 0 ||
        meta.tick_size_ticks <= 0) {
      throw std::invalid_argument(
          "invalid instrument metadata for instrument_id " +
          std::to_string(id));
    }
    return meta;
  }
  if (uniform_policy_.has_value()) {
    return InstrumentMeta{id, uniform_policy_->tick_size_ticks,
                          uniform_policy_->price_scale,
                          uniform_policy_->contract_multiplier};
  }
  throw std::invalid_argument("unknown instrument_id " + std::to_string(id));
}

[[noreturn]] void JsonlReader::fail(const std::string &message) const {
  constexpr std::size_t max_context = 160;
  throw SourceError(path_, row_, current_line_.substr(0, max_context), message);
}

bool JsonlReader::next(MarketDataEvent &event) {
  while (std::getline(input_, current_line_)) {
    ++row_;
    if (std::all_of(
            current_line_.begin(), current_line_.end(), [](char character) {
              return std::isspace(static_cast<unsigned char>(character)) != 0;
            })) {
      fail("blank JSONL row");
    }

    try {
      const Json root = Json::parse(current_line_);
      const Json &header = required(root, "hd");
      const InstrumentId instrument_id =
          required_int64(header, "instrument_id");
      const InstrumentMeta meta = instrument_meta(instrument_id);
      const MarketAction action = parse_action(root);
      const bool needs_side = action == MarketAction::Add ||
                              action == MarketAction::Modify ||
                              action == MarketAction::Trade;

      MarketDataEvent parsed;
      parsed.receive_ts_ns =
          parse_iso8601_timestamp_ns(required_string(root, "ts_recv"));
      parsed.exchange_ts_ns =
          parse_iso8601_timestamp_ns(required_string(header, "ts_event"));
      parsed.instrument_id = instrument_id;
      parsed.source_sequence = required_sequence(root, "sequence");
      parsed.action = action;
      parsed.side = parse_side(root, needs_side);
      parsed.flags = optional_flags(root);

      if (action != MarketAction::Clear && action != MarketAction::Trade) {
        parsed.exchange_order_id = parse_order_id(root);
      } else if (root.contains("order_id") && !root.at("order_id").is_null() &&
                 root.at("order_id") != "") {
        parsed.exchange_order_id = parse_order_id(root);
      }

      const bool needs_quantity =
          action == MarketAction::Add || action == MarketAction::Modify ||
          action == MarketAction::Fill || action == MarketAction::Trade;
      if (needs_quantity) {
        parsed.quantity = required_int64(root, "size");
        if (parsed.quantity <= 0) {
          throw std::invalid_argument("field 'size' must be positive");
        }
      }

      const bool needs_price = action == MarketAction::Add ||
                               action == MarketAction::Modify ||
                               action == MarketAction::Trade;
      if (needs_price) {
        const std::string price = required_string(root, "price");
        parsed.price_ticks =
            parse_decimal_ticks(price, meta.price_scale, meta.tick_size_ticks);
      }

      if (have_previous_) {
        if (parsed.exchange_ts_ns < previous_exchange_ts_ns_) {
          throw std::invalid_argument(
              "exchange timestamp regressed relative to previous row");
        }
        if (parsed.source_sequence <= previous_sequence_) {
          throw std::invalid_argument(
              "source sequence is not strictly increasing");
        }
      }

      event = parsed;
      previous_exchange_ts_ns_ = parsed.exchange_ts_ns;
      previous_sequence_ = parsed.source_sequence;
      have_previous_ = true;
      return true;
    } catch (const SourceError &) {
      throw;
    } catch (const std::exception &error) {
      fail(error.what());
    }
  }

  if (input_.bad()) {
    fail("I/O failure while reading source file");
  }
  return false;
}

} // namespace cmf::market
