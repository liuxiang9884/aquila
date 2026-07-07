#include "tools/market_data/trade_fusion_cli.h"

#include <filesystem>
#include <string>
#include <utility>

#include "tools/market_data/fusion_cli.h"

namespace aquila::tools::market_data {

int RunTradeFusionCli(int argc, char** argv,
                      std::filesystem::path default_config_path,
                      std::string app_description, std::string error_key) {
  return RunFusionCli<TradeFusionCliTraits>(
      argc, argv, std::move(default_config_path), std::move(app_description),
      std::move(error_key));
}

}  // namespace aquila::tools::market_data
