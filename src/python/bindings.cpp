#include "core/BacktestConfig.hpp"
#include "core/Events.hpp"
#include "main/json.hpp"
#include "results/ResultRecorder.hpp"
#include "runtime/BacktestRuntime.hpp"
#include "trading/Strategy.hpp"

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace py = pybind11;

#ifndef BACK_TESTER_VERSION
#define BACK_TESTER_VERSION "unknown"
#endif

namespace {

using namespace cmf;
using Json = nlohmann::json;

struct CallbackCounts {
  std::uint64_t book_update{};
  std::uint64_t trade{};
  std::uint64_t fill{};
  std::uint64_t reject{};
};

struct OwnedBookUpdate {
  InstrumentId instrument_id{};
  TimestampNs exchange_ts_ns{};
  TimestampNs engine_ts_ns{};
  Sequence sequence{};
  bool is_snapshot{};
  std::vector<BookLevel> bids;
  std::vector<BookLevel> asks;
};

class PythonStrategyHandle {
public:
  [[nodiscard]] ClOrdId submit_limit(InstrumentId instrument_id, Side side,
                                     PriceTicks price_ticks,
                                     Quantity quantity) {
    return context().submit_limit(instrument_id, side, price_ticks, quantity);
  }

  bool cancel_order(ClOrdId client_order_id) {
    return context().cancel_order(client_order_id);
  }

  [[nodiscard]] PositionSnapshot position(InstrumentId instrument_id) const {
    return context().position(instrument_id);
  }

  [[nodiscard]] std::vector<OrderQueryRow>
  open_orders(InstrumentId instrument_id) {
    const auto rows = context().open_orders(instrument_id);
    return {rows.begin(), rows.end()};
  }

  [[nodiscard]] TimestampNs now_ns() const { return context().now_ns(); }

  trading::StrategyContext *
  activate(trading::StrategyContext *active) noexcept {
    active_thread_ =
        active == nullptr ? std::thread::id{} : std::this_thread::get_id();
    return std::exchange(active_, active);
  }

  void begin_run() {
    // Binding entry and guard destruction hold the GIL; the flag is set before
    // run() releases it, so another Python thread observes one owner.
    if (running_) {
      throw std::runtime_error("strategy is already running");
    }
    running_ = true;
  }

  void end_run() noexcept { running_ = false; }

private:
  [[nodiscard]] trading::StrategyContext &context() const {
    if (active_ == nullptr || active_thread_ != std::this_thread::get_id()) {
      throw std::runtime_error(
          "strategy context is available only during a callback");
    }
    return *active_;
  }

  trading::StrategyContext *active_{};
  std::thread::id active_thread_;
  bool running_{};
};

class ActiveContext {
public:
  ActiveContext(PythonStrategyHandle &handle,
                trading::StrategyContext &context) noexcept
      : handle_(handle), previous_(handle_.activate(&context)) {}

  ~ActiveContext() { handle_.activate(previous_); }

private:
  PythonStrategyHandle &handle_;
  trading::StrategyContext *previous_;
};

class StrategyRunGuard {
public:
  explicit StrategyRunGuard(PythonStrategyHandle &handle) : handle_(handle) {
    handle_.begin_run();
  }

  ~StrategyRunGuard() { handle_.end_run(); }

  StrategyRunGuard(const StrategyRunGuard &) = delete;
  StrategyRunGuard &operator=(const StrategyRunGuard &) = delete;

private:
  PythonStrategyHandle &handle_;
};

class PythonStrategyAdapter final : public trading::Strategy {
public:
  PythonStrategyAdapter(py::object strategy,
                        std::shared_ptr<PythonStrategyHandle> handle,
                        bool collect_statistics)
      : strategy_(std::move(strategy)), handle_(std::move(handle)),
        collect_statistics_(collect_statistics) {}

  void on_book_update(const BookUpdateView &view,
                      trading::StrategyContext &context) override {
    OwnedBookUpdate owned{view.instrument_id,
                          view.exchange_ts_ns,
                          view.engine_ts_ns,
                          view.sequence,
                          view.is_snapshot,
                          {view.bids.begin(), view.bids.end()},
                          {view.asks.begin(), view.asks.end()}};
    invoke("on_book_update", std::move(owned), context,
           callback_counts_.book_update);
  }

  void on_trade(const TradeView &view,
                trading::StrategyContext &context) override {
    invoke("on_trade", view, context, callback_counts_.trade);
  }

  void on_fill(const FillView &view,
               trading::StrategyContext &context) override {
    invoke("on_fill", view, context, callback_counts_.fill);
  }

  void on_reject(const RejectView &view,
                 trading::StrategyContext &context) override {
    invoke("on_reject", view, context, callback_counts_.reject);
  }

  [[nodiscard]] const CallbackCounts &callback_counts() const noexcept {
    return callback_counts_;
  }

private:
  template <typename Payload>
  void invoke(const char *method, Payload payload,
              trading::StrategyContext &context, std::uint64_t &count) {
    if (collect_statistics_) {
      ++count;
    }
    py::gil_scoped_acquire gil;
    ActiveContext active(*handle_, context);
    strategy_.attr(method)(std::move(payload));
  }

  py::object strategy_;
  std::shared_ptr<PythonStrategyHandle> handle_;
  CallbackCounts callback_counts_;
  bool collect_statistics_{};
};

class PythonResult {
public:
  explicit PythonResult(results::FrozenResults frozen)
      : frozen_(std::make_shared<results::FrozenResults>(std::move(frozen))) {}

  [[nodiscard]] py::object fills_df() const {
    const auto columns = frozen_->fills();
    py::dict values;
    values["exchange_ts_ns"] = array(columns.exchange_ts_ns);
    values["engine_ts_ns"] = array(columns.engine_ts_ns);
    values["instrument_id"] = array(columns.instrument_id);
    values["client_order_id"] = array(columns.client_order_id);
    values["side"] = enum_array<std::int8_t>(columns.side);
    values["price_ticks"] = array(columns.price_ticks);
    values["quantity"] = array(columns.quantity);
    values["remaining_quantity"] = array(columns.remaining_quantity);
    values["liquidity_source"] =
        enum_array<std::uint8_t>(columns.liquidity_source);
    values["trigger_source_sequence"] = array(columns.trigger_source_sequence);
    return py::module_::import("pandas").attr("DataFrame")(
        values, py::arg("copy") = false);
  }

  [[nodiscard]] py::object order_log_df() const {
    const auto columns = frozen_->order_log();
    py::dict values;
    values["engine_ts_ns"] = array(columns.engine_ts_ns);
    values["instrument_id"] = array(columns.instrument_id);
    values["client_order_id"] = array(columns.client_order_id);
    values["event_type"] = enum_array<std::uint8_t>(columns.event_type);
    values["state"] = enum_array<std::uint8_t>(columns.state);
    values["side"] = enum_array<std::int8_t>(columns.side);
    values["limit_price_ticks"] = array(columns.limit_price_ticks);
    values["order_quantity"] = array(columns.order_quantity);
    values["filled_quantity"] = array(columns.filled_quantity);
    values["remaining_quantity"] = array(columns.remaining_quantity);
    values["reject_reason"] = enum_array<std::uint8_t>(columns.reject_reason);
    return py::module_::import("pandas").attr("DataFrame")(
        values, py::arg("copy") = false);
  }

  [[nodiscard]] py::object pnl_series() const {
    const auto columns = frozen_->pnl();
    py::object pandas = py::module_::import("pandas");
    py::object index = pandas.attr("Index")(array(columns.engine_ts_ns),
                                            py::arg("name") = "engine_ts_ns",
                                            py::arg("copy") = false);
    return pandas.attr("Series")(
        array(columns.total_pnl), py::arg("index") = index,
        py::arg("name") = "total_pnl", py::arg("copy") = false);
  }

  [[nodiscard]] std::optional<std::string> dataset_id() const {
    const auto &metadata = frozen_->dataset_metadata();
    return metadata.has_value()
               ? std::optional<std::string>{metadata->dataset_id}
               : std::nullopt;
  }

  [[nodiscard]] std::optional<bool> verified_metadata() const {
    const auto &metadata = frozen_->dataset_metadata();
    return metadata.has_value()
               ? std::optional<bool>{metadata->verified_metadata}
               : std::nullopt;
  }

private:
  [[nodiscard]] py::capsule owner() const {
    auto *copy = new std::shared_ptr<results::FrozenResults>(frozen_);
    return py::capsule(copy, [](void *pointer) {
      delete static_cast<std::shared_ptr<results::FrozenResults> *>(pointer);
    });
  }

  template <typename Value>
  [[nodiscard]] py::array array(std::span<const Value> values) const {
    return py::array(
        py::dtype::of<Value>(), {static_cast<py::ssize_t>(values.size())},
        {static_cast<py::ssize_t>(sizeof(Value))}, values.data(), owner());
  }

  template <typename Storage, typename Enum>
  [[nodiscard]] py::array enum_array(std::span<const Enum> values) const {
    static_assert(sizeof(Storage) == sizeof(Enum));
    return py::array(
        py::dtype::of<Storage>(), {static_cast<py::ssize_t>(values.size())},
        {static_cast<py::ssize_t>(sizeof(Storage))}, values.data(), owner());
  }

  std::shared_ptr<results::FrozenResults> frozen_;
};

[[nodiscard]] std::string
exception_message(const std::exception_ptr &failure) noexcept {
  try {
    std::rethrow_exception(failure);
  } catch (const std::exception &error) {
    return error.what();
  } catch (...) {
    return "unknown non-standard exception";
  }
}

[[nodiscard]] Json optional_sequence(const std::optional<Sequence> &value) {
  return value.has_value() ? Json(*value) : Json(nullptr);
}

[[nodiscard]] Json sequence_digest(const runtime::RunStatistics &statistics) {
  if (!statistics.first_replayed_sequence.has_value()) {
    return nullptr;
  }
  std::ostringstream output;
  output << std::hex << std::setfill('0') << std::setw(16)
         << statistics.replayed_sequence_digest;
  return output.str();
}

void write_run_summary(const std::string &summary_path,
                       const std::string &data_path, DateRange date_range,
                       BacktestConfig config,
                       std::span<const InstrumentMeta> instruments,
                       const runtime::RunStatistics &statistics,
                       const CallbackCounts &callbacks,
                       std::int64_t duration_ns,
                       const results::FrozenResults *results,
                       const std::exception_ptr &failure = nullptr) {
  if (summary_path.empty()) {
    throw std::invalid_argument("run_summary_path must not be empty");
  }

  Json instrument_rows = Json::array();
  for (const auto &instrument : instruments) {
    instrument_rows.push_back({
        {"instrument_id", instrument.instrument_id},
        {"tick_size_ticks", instrument.tick_size_ticks},
        {"price_scale", instrument.price_scale},
        {"contract_multiplier", instrument.contract_multiplier},
    });
  }

  Json counts = {
      {"scheduled_events", statistics.scheduled_events},
      {"market_deliveries", statistics.market_deliveries},
      {"new_order_arrivals", statistics.new_order_arrivals},
      {"cancel_arrivals", statistics.cancel_arrivals},
      {"source_trade_events", statistics.source_trade_events},
      {"callbacks",
       {{"book_update", callbacks.book_update},
        {"trade", callbacks.trade},
        {"fill", callbacks.fill},
        {"reject", callbacks.reject}}},
  };
  if (results != nullptr) {
    counts["fills"] = results->fills().size();
    counts["order_log_rows"] = results->order_log().size();
    counts["pnl_points"] = results->pnl().size();
  } else {
    counts["fills"] = nullptr;
    counts["order_log_rows"] = nullptr;
    counts["pnl_points"] = nullptr;
  }

  const bool read_accounting =
      statistics.source_records_read == statistics.source_records_warmed +
                                            statistics.source_records_replayed +
                                            statistics.source_records_after_end;
  const bool replay_type_accounting =
      statistics.source_records_replayed ==
      statistics.replayed_book_records + statistics.replayed_trade_records;
  const bool sequence_span_matches =
      !statistics.first_replayed_sequence.has_value() ||
      (*statistics.last_replayed_sequence -
           *statistics.first_replayed_sequence + 1 ==
       statistics.source_records_replayed);
  const bool full_manifest_replay =
      statistics.l2_input && statistics.manifest_total_records.has_value() &&
      statistics.source_records_warmed == 0 &&
      statistics.source_records_after_end == 0 &&
      statistics.source_records_replayed == *statistics.manifest_total_records;
  Json manifest_counts = nullptr;
  if (statistics.manifest_total_records.has_value()) {
    manifest_counts = {
        {"total", *statistics.manifest_total_records},
        {"snapshots", *statistics.manifest_snapshot_records},
        {"trades", *statistics.manifest_trade_records},
    };
  }
  Json source_audit = {
      {"input_format", statistics.l2_input ? "l2_cache" : "mbo_jsonl"},
      {"records_read", statistics.source_records_read},
      {"records_warmed", statistics.source_records_warmed},
      {"records_replayed", statistics.source_records_replayed},
      {"records_after_end", statistics.source_records_after_end},
      {"replayed_book_records", statistics.replayed_book_records},
      {"replayed_trade_records", statistics.replayed_trade_records},
      {"first_replayed_sequence",
       optional_sequence(statistics.first_replayed_sequence)},
      {"last_replayed_sequence",
       optional_sequence(statistics.last_replayed_sequence)},
      {"replayed_sequence_digest_fnv1a64", sequence_digest(statistics)},
      {"manifest_records", std::move(manifest_counts)},
      {"checks",
       {{"read_accounting", read_accounting},
        {"replay_type_accounting", replay_type_accounting},
        {"trade_callbacks_match_replayed_trades",
         callbacks.trade == statistics.replayed_trade_records},
        {"l2_market_deliveries_match_replayed_records",
         statistics.l2_input ? Json(statistics.market_deliveries ==
                                    statistics.source_records_replayed)
                             : Json(nullptr)},
        {"sequence_span_matches_replayed_records", sequence_span_matches},
        {"full_manifest_replay", full_manifest_replay},
        {"full_manifest_counts_match",
         full_manifest_replay
             ? Json(statistics.replayed_book_records ==
                        *statistics.manifest_snapshot_records &&
                    statistics.replayed_trade_records ==
                        *statistics.manifest_trade_records)
             : Json(nullptr)}}},
  };

  Json summary = {
      {"schema_version", 1},
      {"status", failure == nullptr ? "success" : "failed"},
      {"data_path", data_path},
      {"dataset_id", statistics.dataset_id.has_value()
                         ? Json(*statistics.dataset_id)
                         : Json(nullptr)},
      {"verified_metadata", statistics.verified_metadata.has_value()
                                ? Json(*statistics.verified_metadata)
                                : Json(nullptr)},
      {"date_range",
       {{"start_ts_ns", date_range.start_ts_ns},
        {"end_ts_ns", date_range.end_ts_ns}}},
      {"config",
       {{"market_data_latency_ns", config.market_data_latency_ns},
        {"order_latency_ns", config.order_latency_ns},
        {"book_depth", config.book_depth},
        {"allow_unverified_metadata", config.allow_unverified_metadata}}},
      {"instruments", std::move(instrument_rows)},
      {"source_audit", std::move(source_audit)},
      {"counts", std::move(counts)},
      {"duration_ns", duration_ns},
      {"error", failure == nullptr
                    ? Json(nullptr)
                    : Json{{"message", exception_message(failure)}}},
  };

  const std::filesystem::path target(summary_path);
  if (target.has_parent_path()) {
    std::filesystem::create_directories(target.parent_path());
  }
  std::filesystem::path temporary = target;
  temporary += ".tmp";
  try {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw std::runtime_error("cannot open run summary temporary file: " +
                               temporary.string());
    }
    output << summary.dump(2) << '\n';
    output.close();
    if (!output) {
      throw std::runtime_error("cannot write run summary temporary file: " +
                               temporary.string());
    }
    std::filesystem::rename(temporary, target);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

} // namespace

PYBIND11_MODULE(_backtester, module) {
  module.doc() = "Deterministic native options back-testing runtime";
  module.def("version", []() { return BACK_TESTER_VERSION; });

  py::enum_<cmf::Side>(module, "Side")
      .value("SELL", cmf::Side::Sell)
      .value("NONE", cmf::Side::None)
      .value("BUY", cmf::Side::Buy);
  py::enum_<cmf::LiquiditySource>(module, "LiquiditySource")
      .value("HISTORICAL_DISPLAYED", cmf::LiquiditySource::HistoricalDisplayed)
      .value("QUOTE_CROSS", cmf::LiquiditySource::QuoteCross)
      .value("TRADE_CROSS", cmf::LiquiditySource::TradeCross);
  py::enum_<cmf::OrderState>(module, "OrderState")
      .value("PENDING_NEW", cmf::OrderState::PendingNew)
      .value("OPEN", cmf::OrderState::Open)
      .value("PARTIALLY_FILLED", cmf::OrderState::PartiallyFilled)
      .value("FILLED", cmf::OrderState::Filled)
      .value("PENDING_CANCEL", cmf::OrderState::PendingCancel)
      .value("CANCELLED", cmf::OrderState::Cancelled)
      .value("REJECTED", cmf::OrderState::Rejected);
  py::enum_<cmf::RejectReason>(module, "RejectReason")
      .value("NONE", cmf::RejectReason::None)
      .value("UNKNOWN_INSTRUMENT", cmf::RejectReason::UnknownInstrument)
      .value("INVALID_SIDE", cmf::RejectReason::InvalidSide)
      .value("NON_POSITIVE_QUANTITY", cmf::RejectReason::NonPositiveQuantity)
      .value("INVALID_PRICE", cmf::RejectReason::InvalidPrice)
      .value("TICK_MISALIGNMENT", cmf::RejectReason::TickMisalignment)
      .value("DUPLICATE_CLIENT_ORDER_ID",
             cmf::RejectReason::DuplicateClientOrderId)
      .value("UNSUPPORTED_ORDER_TYPE", cmf::RejectReason::UnsupportedOrderType)
      .value("UNSUPPORTED_TIME_IN_FORCE",
             cmf::RejectReason::UnsupportedTimeInForce)
      .value("UNKNOWN_ORDER", cmf::RejectReason::UnknownOrder)
      .value("ALREADY_TERMINAL", cmf::RejectReason::AlreadyTerminal);

  py::class_<cmf::BacktestConfig>(module, "BacktestConfig")
      .def(py::init([](cmf::TimestampNs market_data_latency_ns,
                       cmf::TimestampNs order_latency_ns,
                       std::uint32_t book_depth,
                       bool allow_unverified_metadata) {
             return cmf::BacktestConfig{market_data_latency_ns,
                                        order_latency_ns, book_depth,
                                        allow_unverified_metadata};
           }),
           py::arg("market_data_latency_ns") = 0,
           py::arg("order_latency_ns") = cmf::runtime::default_order_latency_ns,
           py::arg("book_depth") = 15,
           py::arg("allow_unverified_metadata") = false)
      .def_readwrite("market_data_latency_ns",
                     &cmf::BacktestConfig::market_data_latency_ns)
      .def_readwrite("order_latency_ns", &cmf::BacktestConfig::order_latency_ns)
      .def_readwrite("book_depth", &cmf::BacktestConfig::book_depth)
      .def_readwrite("allow_unverified_metadata",
                     &cmf::BacktestConfig::allow_unverified_metadata);

  py::class_<cmf::DateRange>(module, "DateRange")
      .def(py::init<cmf::TimestampNs, cmf::TimestampNs>(),
           py::arg("start_ts_ns") =
               std::numeric_limits<cmf::TimestampNs>::lowest(),
           py::arg("end_ts_ns") = std::numeric_limits<cmf::TimestampNs>::max())
      .def_readwrite("start_ts_ns", &cmf::DateRange::start_ts_ns)
      .def_readwrite("end_ts_ns", &cmf::DateRange::end_ts_ns);

  py::class_<cmf::InstrumentMeta>(module, "InstrumentMeta")
      .def(py::init<cmf::InstrumentId, cmf::PriceTicks, cmf::PriceTicks,
                    cmf::Quantity>(),
           py::arg("instrument_id"), py::arg("tick_size_ticks") = 1,
           py::arg("price_scale") = 1'000'000'000,
           py::arg("contract_multiplier") = 1)
      .def_readonly("instrument_id", &cmf::InstrumentMeta::instrument_id)
      .def_readonly("tick_size_ticks", &cmf::InstrumentMeta::tick_size_ticks)
      .def_readonly("price_scale", &cmf::InstrumentMeta::price_scale)
      .def_readonly("contract_multiplier",
                    &cmf::InstrumentMeta::contract_multiplier);

  py::class_<cmf::BookLevel>(module, "BookLevel")
      .def_readonly("price", &cmf::BookLevel::price)
      .def_readonly("quantity", &cmf::BookLevel::quantity);
  py::class_<OwnedBookUpdate>(module, "BookUpdate")
      .def_readonly("instrument_id", &OwnedBookUpdate::instrument_id)
      .def_readonly("exchange_ts_ns", &OwnedBookUpdate::exchange_ts_ns)
      .def_readonly("engine_ts_ns", &OwnedBookUpdate::engine_ts_ns)
      .def_readonly("sequence", &OwnedBookUpdate::sequence)
      .def_readonly("is_snapshot", &OwnedBookUpdate::is_snapshot)
      .def_readonly("bids", &OwnedBookUpdate::bids)
      .def_readonly("asks", &OwnedBookUpdate::asks);
  py::class_<cmf::TradeView>(module, "Trade")
      .def_readonly("instrument_id", &cmf::TradeView::instrument_id)
      .def_readonly("exchange_ts_ns", &cmf::TradeView::exchange_ts_ns)
      .def_readonly("engine_ts_ns", &cmf::TradeView::engine_ts_ns)
      .def_readonly("sequence", &cmf::TradeView::sequence)
      .def_readonly("aggressor_side", &cmf::TradeView::aggressor_side)
      .def_readonly("price", &cmf::TradeView::price)
      .def_readonly("quantity", &cmf::TradeView::quantity);
  py::class_<cmf::FillView>(module, "Fill")
      .def_readonly("instrument_id", &cmf::FillView::instrument_id)
      .def_readonly("client_order_id", &cmf::FillView::client_order_id)
      .def_readonly("side", &cmf::FillView::side)
      .def_readonly("price", &cmf::FillView::price)
      .def_readonly("quantity", &cmf::FillView::quantity)
      .def_readonly("remaining_quantity", &cmf::FillView::remaining_quantity)
      .def_readonly("exchange_ts_ns", &cmf::FillView::exchange_ts_ns)
      .def_readonly("engine_ts_ns", &cmf::FillView::engine_ts_ns)
      .def_readonly("sequence", &cmf::FillView::fill_sequence)
      .def_readonly("liquidity_source", &cmf::FillView::liquidity_source)
      .def_readonly("trigger_source_sequence",
                    &cmf::FillView::trigger_source_sequence);
  py::class_<cmf::RejectView>(module, "Reject")
      .def_readonly("instrument_id", &cmf::RejectView::instrument_id)
      .def_readonly("client_order_id", &cmf::RejectView::client_order_id)
      .def_readonly("reason", &cmf::RejectView::reason)
      .def_readonly("exchange_ts_ns", &cmf::RejectView::exchange_ts_ns)
      .def_readonly("engine_ts_ns", &cmf::RejectView::engine_ts_ns)
      .def_readonly("sequence", &cmf::RejectView::sequence);
  py::class_<cmf::PositionSnapshot>(module, "Position")
      .def_readonly("instrument_id", &cmf::PositionSnapshot::instrument_id)
      .def_readonly("net_quantity", &cmf::PositionSnapshot::net_quantity)
      .def_readonly("average_open_price_ticks",
                    &cmf::PositionSnapshot::average_open_price_ticks)
      .def_readonly("realized_pnl", &cmf::PositionSnapshot::realized_pnl)
      .def_readonly("unrealized_pnl", &cmf::PositionSnapshot::unrealized_pnl);
  py::class_<cmf::OrderQueryRow>(module, "OpenOrder")
      .def_readonly("instrument_id", &cmf::OrderQueryRow::instrument_id)
      .def_readonly("client_order_id", &cmf::OrderQueryRow::client_order_id)
      .def_readonly("state", &cmf::OrderQueryRow::state)
      .def_readonly("side", &cmf::OrderQueryRow::side)
      .def_readonly("limit_price_ticks", &cmf::OrderQueryRow::limit_price_ticks)
      .def_readonly("order_quantity", &cmf::OrderQueryRow::order_quantity)
      .def_readonly("filled_quantity", &cmf::OrderQueryRow::filled_quantity)
      .def_readonly("remaining_quantity",
                    &cmf::OrderQueryRow::remaining_quantity);

  py::class_<PythonStrategyHandle, std::shared_ptr<PythonStrategyHandle>>(
      module, "Strategy")
      .def(py::init<>())
      .def("on_book_update",
           [](PythonStrategyHandle &, const OwnedBookUpdate &) {})
      .def("on_trade", [](PythonStrategyHandle &, const cmf::TradeView &) {})
      .def("on_fill", [](PythonStrategyHandle &, const cmf::FillView &) {})
      .def("on_reject", [](PythonStrategyHandle &, const cmf::RejectView &) {})
      .def("submit_limit", &PythonStrategyHandle::submit_limit,
           py::arg("instrument_id"), py::arg("side"), py::arg("price_ticks"),
           py::arg("quantity"))
      .def("cancel_order", &PythonStrategyHandle::cancel_order)
      .def("position", &PythonStrategyHandle::position)
      .def("open_orders", &PythonStrategyHandle::open_orders)
      .def_property_readonly("now_ns", &PythonStrategyHandle::now_ns);

  py::class_<PythonResult>(module, "Result")
      .def_property_readonly("fills_df", &PythonResult::fills_df)
      .def_property_readonly("order_log_df", &PythonResult::order_log_df)
      .def_property_readonly("pnl_series", &PythonResult::pnl_series)
      .def_property_readonly("dataset_id", &PythonResult::dataset_id)
      .def_property_readonly("verified_metadata",
                             &PythonResult::verified_metadata);

  module.def(
      "run",
      [](py::object strategy, const std::string &data_path,
         cmf::DateRange date_range,
         std::optional<cmf::BacktestConfig> optional_config,
         std::optional<std::vector<cmf::InstrumentMeta>> optional_instruments,
         std::optional<std::string> run_summary_path) {
        if (run_summary_path.has_value() && run_summary_path->empty()) {
          throw std::invalid_argument("run_summary_path must not be empty");
        }
        auto handle = strategy.cast<std::shared_ptr<PythonStrategyHandle>>();
        StrategyRunGuard run_guard(*handle);
        cmf::BacktestConfig config =
            optional_config.value_or(cmf::BacktestConfig{
                0, cmf::runtime::default_order_latency_ns, 15, false});
        PythonStrategyAdapter adapter(std::move(strategy), handle,
                                      run_summary_path.has_value());
        cmf::results::FrozenResults frozen;
        cmf::runtime::RunStatistics statistics;
        std::vector<cmf::InstrumentMeta> instruments;
        const auto started = std::chrono::steady_clock::now();
        try {
          {
            py::gil_scoped_release release;
            instruments =
                optional_instruments.has_value()
                    ? std::move(*optional_instruments)
                    : cmf::runtime::discover_databento_instruments(data_path);
            frozen = cmf::runtime::run_backtest(
                adapter, data_path, date_range, config, instruments,
                run_summary_path.has_value() ? &statistics : nullptr);
          }
        } catch (...) {
          const auto failure = std::current_exception();
          const auto duration =
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - started)
                  .count();
          if (run_summary_path.has_value()) {
            try {
              write_run_summary(*run_summary_path, data_path, date_range,
                                config, instruments, statistics,
                                adapter.callback_counts(), duration, nullptr,
                                failure);
            } catch (...) {
              // Preserve the replay/callback failure as the primary error.
            }
          }
          std::rethrow_exception(failure);
        }
        if (run_summary_path.has_value()) {
          const auto duration =
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - started)
                  .count();
          write_run_summary(*run_summary_path, data_path, date_range, config,
                            instruments, statistics, adapter.callback_counts(),
                            duration, &frozen);
        }
        return PythonResult{std::move(frozen)};
      },
      py::arg("strategy"), py::arg("data_path"), py::arg("date_range"),
      py::arg("config") = py::none(), py::arg("instruments") = py::none(),
      py::arg("run_summary_path") = py::none());

  module.def(
      "_benchmark_book_callbacks",
      [](py::object strategy, std::size_t depth, std::size_t iterations) {
        (void)strategy.cast<std::shared_ptr<PythonStrategyHandle>>();
        if (depth == 0 || iterations == 0) {
          throw std::invalid_argument(
              "benchmark depth and iterations must be positive");
        }
        OwnedBookUpdate payload;
        payload.instrument_id = 1;
        payload.sequence = 1;
        payload.bids.reserve(depth);
        payload.asks.reserve(depth);
        for (std::size_t index = 0; index < depth; ++index) {
          const auto offset = static_cast<PriceTicks>(index);
          payload.bids.push_back(BookLevel{100 - offset, 1});
          payload.asks.push_back(BookLevel{101 + offset, 1});
        }

        std::chrono::nanoseconds elapsed;
        {
          py::gil_scoped_release release;
          const auto start = std::chrono::steady_clock::now();
          for (std::size_t index = 0; index < iterations; ++index) {
            py::gil_scoped_acquire acquire;
            strategy.attr("on_book_update")(payload);
          }
          elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - start);
        }
        return elapsed.count();
      },
      py::arg("strategy"), py::arg("depth"), py::arg("iterations") = 1'000);
}
