#pragma once

#include "common/MarketDataEvent.hpp"
#include <cstdint>
#include <mutex>
#include <ostream>
#include <string_view>

namespace cmf
{

// Predefined log paths (relative to process working directory).
inline constexpr const char* kOrderBookAnomalyLogPath = "logs/order-book-anomalies.log";
inline constexpr const char* kRouterAnomalyLogPath = "logs/router-anomalies.log";

enum class BookAnomaly : uint8_t
{
    UnknownCancel,
    UnknownModifyIgnored,
    UnknownModifyAsAdd,
    DuplicateAddReplaced,
};

enum class RouterAnomaly : uint8_t
{
    UnresolvedInstrument,
};

class BookAnomalyLog
{
  public:
    [[nodiscard]] static BookAnomalyLog& instance() noexcept;

    void log_book(BookAnomaly kind, const MarketDataEvent& e,
                  std::string_view component = "LimitOrderBook");
    void log_router(RouterAnomaly kind, const MarketDataEvent& e);

    void flush();

    [[nodiscard]] uint64_t book_count(BookAnomaly kind) const noexcept;
    [[nodiscard]] uint64_t router_count(RouterAnomaly kind) const noexcept;

    void write_summary(std::ostream& out) const;

  private:
    BookAnomalyLog();

    void write_line(std::ostream& stream, std::string_view component,
                    std::string_view anomaly, const MarketDataEvent& e);

    mutable std::mutex mutex_;
    std::ostream* book_stream_ = nullptr;
    std::ostream* router_stream_ = nullptr;
    uint64_t book_counts_[4]{};
    uint64_t router_counts_[1]{};
};

} // namespace cmf
