#include "BookAnomalyLog.hpp"

#include <filesystem>
#include <fstream>
#include <memory>

namespace cmf
{

namespace
{

std::unique_ptr<std::ofstream> open_log_file(const char* path)
{
    const std::filesystem::path file_path(path);
    std::error_code ec;
    if (const auto parent = file_path.parent_path(); !parent.empty())
        std::filesystem::create_directories(parent, ec);

    auto stream = std::make_unique<std::ofstream>(
        file_path, std::ios::out | std::ios::app);
    if (!stream->is_open())
        return nullptr;
    return stream;
}

const char* book_anomaly_name(const BookAnomaly kind) noexcept
{
    switch (kind)
    {
    case BookAnomaly::UnknownCancel:
        return "unknown_cancel";
    case BookAnomaly::UnknownModifyIgnored:
        return "unknown_modify_ignored";
    case BookAnomaly::UnknownModifyAsAdd:
        return "unknown_modify_as_add";
    case BookAnomaly::DuplicateAddReplaced:
        return "duplicate_add_replaced";
    }
    return "unknown";
}

const char* router_anomaly_name(const RouterAnomaly kind) noexcept
{
    switch (kind)
    {
    case RouterAnomaly::UnresolvedInstrument:
        return "unresolved_instrument";
    }
    return "unknown";
}

} // namespace

BookAnomalyLog& BookAnomalyLog::instance() noexcept
{
    static BookAnomalyLog log;
    return log;
}

BookAnomalyLog::BookAnomalyLog()
{
    static std::unique_ptr<std::ofstream> book_file =
        open_log_file(kOrderBookAnomalyLogPath);
    static std::unique_ptr<std::ofstream> router_file =
        open_log_file(kRouterAnomalyLogPath);

    book_stream_ = book_file ? static_cast<std::ostream*>(book_file.get()) : nullptr;
    router_stream_ =
        router_file ? static_cast<std::ostream*>(router_file.get()) : nullptr;
}

void BookAnomalyLog::write_line(std::ostream& stream, const std::string_view component,
                                const std::string_view anomaly,
                                const MarketDataEvent& e)
{
    stream << component << ' ' << anomaly << " order_id=" << e.order_id
           << " instrument_id=" << e.instrument_id << " action="
           << static_cast<char>(e.action) << " side=" << static_cast<char>(e.side)
           << " price=" << e.price << " size=" << e.size
           << " ts_event=" << e.ts_event << '\n';
}

void BookAnomalyLog::log_book(const BookAnomaly kind, const MarketDataEvent& e,
                              const std::string_view component)
{
    const auto index = static_cast<std::size_t>(kind);
    {
        const std::lock_guard lock(mutex_);
        ++book_counts_[index];
        if (book_stream_)
            write_line(*book_stream_, component, book_anomaly_name(kind), e);
    }
    flush();
}

void BookAnomalyLog::log_router(const RouterAnomaly kind, const MarketDataEvent& e)
{
    const auto index = static_cast<std::size_t>(kind);
    {
        const std::lock_guard lock(mutex_);
        ++router_counts_[index];
        if (router_stream_)
            write_line(*router_stream_, "OrderBookRouter", router_anomaly_name(kind),
                       e);
    }
    flush();
}

void BookAnomalyLog::flush()
{
    const std::lock_guard lock(mutex_);
    if (book_stream_)
        book_stream_->flush();
    if (router_stream_)
        router_stream_->flush();
}

uint64_t BookAnomalyLog::book_count(const BookAnomaly kind) const noexcept
{
    return book_counts_[static_cast<std::size_t>(kind)];
}

uint64_t BookAnomalyLog::router_count(const RouterAnomaly kind) const noexcept
{
    return router_counts_[static_cast<std::size_t>(kind)];
}

void BookAnomalyLog::write_summary(std::ostream& out) const
{
    const std::lock_guard lock(mutex_);
    out << "// order-book anomaly summary\n";
    out << "unknown_cancel=" << book_counts_[0] << '\n';
    out << "unknown_modify_ignored=" << book_counts_[1] << '\n';
    out << "unknown_modify_as_add=" << book_counts_[2] << '\n';
    out << "duplicate_add_replaced=" << book_counts_[3] << '\n';
    out << "// router anomaly summary\n";
    out << "unresolved_instrument=" << router_counts_[0] << '\n';
}

} // namespace cmf
