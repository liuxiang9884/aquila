#ifndef AQUILA_TOOLS_MARKET_DATA_TRADE_FUSION_CLI_H_
#define AQUILA_TOOLS_MARKET_DATA_TRADE_FUSION_CLI_H_

#include <filesystem>
#include <string>

namespace aquila::tools::market_data {

int RunTradeFusionCli(int argc, char** argv,
                      std::filesystem::path default_config_path,
                      std::string app_description, std::string error_key);

}  // namespace aquila::tools::market_data

#endif  // AQUILA_TOOLS_MARKET_DATA_TRADE_FUSION_CLI_H_
