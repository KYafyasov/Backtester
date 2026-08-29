#pragma once

#include "core/Types.hpp"

#include <string_view>

namespace cmf::market {

[[nodiscard]] PriceTicks parse_decimal_ticks(std::string_view text,
                                             PriceTicks price_scale,
                                             PriceTicks tick_size_ticks);

[[nodiscard]] TimestampNs parse_iso8601_timestamp_ns(std::string_view text);

} // namespace cmf::market
