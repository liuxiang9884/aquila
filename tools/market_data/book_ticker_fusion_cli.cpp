#include "tools/market_data/book_ticker_fusion_cli.h"

#include <filesystem>
#include <string>
#include <utility>

#include "tools/market_data/fusion_cli.h"

namespace aquila::tools::market_data {

int RunBookTickerFusionCli(int argc, char** argv,
                           std::filesystem::path default_config_path,
                           std::string app_description, std::string error_key) {
  return RunFusionCli<BookTickerFusionCliTraits>(
      argc, argv, std::move(default_config_path), std::move(app_description),
      std::move(error_key));
}

}  // namespace aquila::tools::market_data
