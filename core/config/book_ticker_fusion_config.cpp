#include "core/config/book_ticker_fusion_config.h"

#include <filesystem>
#include <string_view>

#include "core/config/fusion_config_parser.h"

namespace aquila::config {
namespace {

namespace md = aquila::market_data;

struct BookTickerFusionConfigParseTraits {
  using Config = md::BookTickerFusionConfig;
  using SourceConfig = md::BookTickerFusionSourceConfig;
  using Result = BookTickerFusionConfigResult;

  static constexpr std::string_view kDefaultSourceChannel =
      "book_ticker_channel";
  static constexpr std::string_view kLoadErrorPrefix =
      "failed to load fusion config: ";
};

}  // namespace

BookTickerFusionConfigResult ParseBookTickerFusionConfig(
    const toml::table& node) {
  return ParseFusionConfig<BookTickerFusionConfigParseTraits>(node);
}

BookTickerFusionConfigResult LoadBookTickerFusionConfigFile(
    const std::filesystem::path& path) {
  return LoadFusionConfigFile<BookTickerFusionConfigParseTraits>(path);
}

}  // namespace aquila::config
