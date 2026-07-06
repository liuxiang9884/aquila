#include "tools/market_data/trade_fusion_cli.h"

int main(int argc, char** argv) {
  return aquila::tools::market_data::RunTradeFusionCli(
      argc, argv,
      "config/market_data_fusion/binance_trade_fusion_4sources.toml",
      "Binance Trade fastest-route fusion", "binance_trade_fusion");
}
