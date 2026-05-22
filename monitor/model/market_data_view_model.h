#ifndef AQUILA_MONITOR_MODEL_MARKET_DATA_VIEW_MODEL_H_
#define AQUILA_MONITOR_MODEL_MARKET_DATA_VIEW_MODEL_H_

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/core.h>
#include <magic_enum/magic_enum.hpp>

#include "core/common/types.h"
#include "core/config/instrument_catalog.h"
#include "monitor/market_data/market_data_update.h"
#include "monitor/model/account_monitor_snapshot.h"

namespace aquila::monitor {

namespace market_data_view_model_detail {

[[nodiscard]] inline std::string ExchangeLabel(Exchange exchange) {
  std::string_view name = magic_enum::enum_name(exchange);
  if (!name.empty() && name.front() == 'k') {
    name.remove_prefix(1);
  }
  return name.empty() ? "Unknown" : std::string(name);
}

[[nodiscard]] inline std::string DeriveExchangeSymbol(Exchange exchange,
                                                      std::string_view symbol) {
  if (exchange == Exchange::kBinance) {
    std::string compact;
    compact.reserve(symbol.size());
    for (const char value : symbol) {
      if (value != '_') {
        compact.push_back(value);
      }
    }
    return compact;
  }
  return std::string(symbol);
}

[[nodiscard]] inline const config::InstrumentInfo* FindInstrument(
    std::span<const config::InstrumentInfo> instruments, Exchange exchange,
    std::string_view symbol) {
  for (const config::InstrumentInfo& instrument : instruments) {
    if (instrument.exchange == exchange && instrument.symbol == symbol) {
      return &instrument;
    }
  }
  return nullptr;
}

[[nodiscard]] inline std::string FormatLocalTime(std::int64_t local_ns) {
  if (local_ns <= 0) {
    return "-";
  }

  constexpr std::int64_t kNsPerMillisecond = 1'000'000;
  constexpr std::int64_t kNsPerSecond = 1'000'000'000;
  constexpr std::int64_t kSecondsPerDay = 24 * 60 * 60;
  constexpr std::int64_t kNsPerDay = kSecondsPerDay * kNsPerSecond;

  std::int64_t ns_of_day = local_ns % kNsPerDay;
  if (ns_of_day < 0) {
    ns_of_day += kNsPerDay;
  }
  const std::int64_t total_seconds = ns_of_day / kNsPerSecond;
  const std::int64_t millisecond =
      (ns_of_day % kNsPerSecond) / kNsPerMillisecond;
  const std::int64_t hour = total_seconds / 3600;
  const std::int64_t minute = (total_seconds / 60) % 60;
  const std::int64_t second = total_seconds % 60;
  return fmt::format("{:02}:{:02}:{:02}.{:03}", hour, minute, second,
                     millisecond);
}

}  // namespace market_data_view_model_detail

class MarketDataViewModel {
 public:
  MarketDataViewModel(std::span<const SymbolSummary> symbols,
                      std::span<const config::InstrumentInfo> instruments,
                      std::string_view selected_symbol)
      : selected_symbol_(selected_symbol) {
    rows_.reserve(symbols.size() * 2);
    for (const SymbolSummary& symbol : symbols) {
      AddRow(symbol.symbol, Exchange::kGate, instruments);
      AddRow(symbol.symbol, Exchange::kBinance, instruments);
    }
    RefreshAllRowViews();
    RefreshSelectedRows();
  }

  void SetSelectedSymbol(std::string_view selected_symbol) {
    selected_symbol_ = selected_symbol;
    RefreshSelectedRows();
  }

  void ApplyBatch(const MarketDataBatch& batch) {
    const std::uint16_t row_count =
        std::min<std::uint16_t>(batch.row_count, kMarketDataBatchCapacity);
    for (std::uint16_t i = 0; i < row_count; ++i) {
      const MarketDataRowUpdate& update = batch.rows[i];
      RowState* row = FindRow(update.exchange, update.symbol_id);
      if (row == nullptr) {
        continue;
      }

      row->market_data_id = fmt::format("{}", update.id);
      row->updated_time =
          market_data_view_model_detail::FormatLocalTime(update.local_ns);
      row->view.has_data = true;
      row->view.has_last_price = false;
      row->view.bid_price = update.bid_price;
      row->view.bid_volume = update.bid_volume;
      row->view.ask_price = update.ask_price;
      row->view.ask_volume = update.ask_volume;
      row->view.has_volume = false;
      row->view.has_turnover = false;
      RefreshRowView(*row);
    }
    RefreshSelectedRows();
  }

  [[nodiscard]] std::span<const MarketDataRow> SelectedRows() const noexcept {
    return selected_rows_;
  }

 private:
  struct RowState {
    Exchange exchange{Exchange::kGate};
    std::int32_t symbol_id{-1};
    std::string symbol;
    std::string exchange_label;
    std::string exchange_symbol;
    std::string market_data_id{"-"};
    std::string updated_time{"-"};
    MarketDataRow view{};
  };

  void AddRow(std::string_view symbol, Exchange exchange,
              std::span<const config::InstrumentInfo> instruments) {
    const config::InstrumentInfo* instrument =
        market_data_view_model_detail::FindInstrument(instruments, exchange,
                                                      symbol);
    RowState row{
        .exchange = exchange,
        .symbol_id = instrument == nullptr ? -1 : instrument->symbol_id,
        .symbol = std::string(symbol),
        .exchange_label =
            market_data_view_model_detail::ExchangeLabel(exchange),
        .exchange_symbol =
            instrument == nullptr
                ? market_data_view_model_detail::DeriveExchangeSymbol(exchange,
                                                                      symbol)
                : instrument->exchange_symbol,
    };
    row.view.has_data = false;
    row.view.has_last_price = false;
    row.view.has_volume = false;
    row.view.has_turnover = false;
    rows_.push_back(std::move(row));
  }

  void RefreshAllRowViews() {
    for (RowState& row : rows_) {
      RefreshRowView(row);
    }
  }

  static void RefreshRowView(RowState& row) {
    row.view.exchange = row.exchange_label;
    row.view.exchange_symbol = row.exchange_symbol;
    row.view.market_data_id = row.market_data_id;
    row.view.updated_time = row.updated_time;
  }

  void RefreshSelectedRows() {
    selected_rows_.clear();
    for (const RowState& row : rows_) {
      if (row.symbol == selected_symbol_) {
        selected_rows_.push_back(row.view);
      }
    }
  }

  [[nodiscard]] RowState* FindRow(Exchange exchange,
                                  std::int32_t symbol_id) noexcept {
    for (RowState& row : rows_) {
      if (row.exchange == exchange && row.symbol_id == symbol_id) {
        return &row;
      }
    }
    return nullptr;
  }

  std::string selected_symbol_;
  std::vector<RowState> rows_;
  std::vector<MarketDataRow> selected_rows_;
};

}  // namespace aquila::monitor

#endif  // AQUILA_MONITOR_MODEL_MARKET_DATA_VIEW_MODEL_H_
