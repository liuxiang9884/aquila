#include "tools/market_data/book_ticker_fusion_cli.h"

int main(int argc, char** argv) {
  return aquila::tools::market_data::RunBookTickerFusionCli(
      argc, argv,
      "config/market_data_fusion/binance_book_ticker_fusion_4sources.toml",
      "Binance BookTicker fastest-route fusion", "binance_book_ticker_fusion");
}
