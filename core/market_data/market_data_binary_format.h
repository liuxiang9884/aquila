#ifndef AQUILA_CORE_MARKET_DATA_MARKET_DATA_BINARY_FORMAT_H_
#define AQUILA_CORE_MARKET_DATA_MARKET_DATA_BINARY_FORMAT_H_

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <istream>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include <fmt/format.h>

#include "core/config/data_reader_config.h"
#include "core/market_data/types.h"

namespace aquila::market_data {

static_assert(std::endian::native == std::endian::little,
              "market data typed binary format currently requires "
              "little-endian hosts");

struct MarketDataBinaryHeader {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t header_size;
  std::uint16_t feed_type;
  std::uint16_t record_size;
  std::uint32_t flags;
};

static_assert(sizeof(MarketDataBinaryHeader) == 16);
static_assert(std::is_trivially_copyable_v<MarketDataBinaryHeader>);
static_assert(std::is_standard_layout_v<MarketDataBinaryHeader>);

inline constexpr std::uint32_t kMarketDataBinaryMagic = 0x444d5141U;
inline constexpr std::uint16_t kMarketDataBinaryVersion = 1;
inline constexpr std::uint16_t kMarketDataBinaryHeaderSize =
    sizeof(MarketDataBinaryHeader);
inline constexpr std::uint16_t kMarketDataBinaryBookTickerFeedType = 1;
inline constexpr std::uint16_t kMarketDataBinaryTradeFeedType = 2;
inline constexpr std::uint32_t kMarketDataBinaryFlags = 0;
inline constexpr std::string_view kMarketDataBinaryFormatName{
    "aquila.market_data.binary"};

[[nodiscard]] constexpr std::uint16_t MarketDataBinaryFeedTypeForFeed(
    config::DataReaderFeed feed) noexcept {
  switch (feed) {
    case config::DataReaderFeed::kBookTicker:
      return kMarketDataBinaryBookTickerFeedType;
    case config::DataReaderFeed::kTrade:
      return kMarketDataBinaryTradeFeedType;
  }
  return 0;
}

[[nodiscard]] constexpr std::size_t MarketDataBinaryRecordSizeForFeed(
    config::DataReaderFeed feed) noexcept {
  switch (feed) {
    case config::DataReaderFeed::kBookTicker:
      return sizeof(BookTicker);
    case config::DataReaderFeed::kTrade:
      return sizeof(Trade);
  }
  return 0;
}

[[nodiscard]] inline std::string_view MarketDataBinaryFeedName(
    config::DataReaderFeed feed) noexcept {
  switch (feed) {
    case config::DataReaderFeed::kBookTicker:
      return "book_ticker";
    case config::DataReaderFeed::kTrade:
      return "trade";
  }
  return "unknown";
}

[[nodiscard]] inline std::string_view MarketDataBinaryFeedTypeName(
    std::uint16_t feed_type) noexcept {
  switch (feed_type) {
    case kMarketDataBinaryBookTickerFeedType:
      return "book_ticker";
    case kMarketDataBinaryTradeFeedType:
      return "trade";
  }
  return "unknown";
}

[[nodiscard]] constexpr MarketDataBinaryHeader MakeMarketDataBinaryHeader(
    config::DataReaderFeed feed) noexcept {
  return MarketDataBinaryHeader{
      .magic = kMarketDataBinaryMagic,
      .version = kMarketDataBinaryVersion,
      .header_size = kMarketDataBinaryHeaderSize,
      .feed_type = MarketDataBinaryFeedTypeForFeed(feed),
      .record_size =
          static_cast<std::uint16_t>(MarketDataBinaryRecordSizeForFeed(feed)),
      .flags = kMarketDataBinaryFlags,
  };
}

[[nodiscard]] inline bool WriteMarketDataBinaryHeader(
    std::ostream& output, config::DataReaderFeed feed) {
  const MarketDataBinaryHeader header = MakeMarketDataBinaryHeader(feed);
  output.write(reinterpret_cast<const char*>(&header), sizeof(header));
  return output.good();
}

[[nodiscard]] inline MarketDataBinaryHeader ReadMarketDataBinaryHeader(
    std::istream& input, const std::filesystem::path& file) {
  MarketDataBinaryHeader header{};
  input.read(reinterpret_cast<char*>(&header), sizeof(header));
  if (input.gcount() != static_cast<std::streamsize>(sizeof(header))) {
    throw std::runtime_error(fmt::format(
        "binary data file '{}' is missing market data header", file.string()));
  }
  return header;
}

[[nodiscard]] inline MarketDataBinaryHeader ReadMarketDataBinaryHeaderFromData(
    const char* data, std::size_t size, const std::filesystem::path& file) {
  if (size < sizeof(MarketDataBinaryHeader)) {
    throw std::runtime_error(fmt::format(
        "binary data file '{}' is missing market data header", file.string()));
  }
  MarketDataBinaryHeader header{};
  std::memcpy(&header, data, sizeof(header));
  return header;
}

inline void ValidateMarketDataBinaryHeader(const MarketDataBinaryHeader& header,
                                           config::DataReaderFeed expected_feed,
                                           const std::filesystem::path& file) {
  if (header.magic != kMarketDataBinaryMagic) {
    throw std::runtime_error(fmt::format(
        "binary data file '{}' has invalid market data magic 0x{:08x}",
        file.string(), header.magic));
  }
  if (header.version != kMarketDataBinaryVersion) {
    throw std::runtime_error(fmt::format(
        "binary data file '{}' has unsupported market data version {}",
        file.string(), header.version));
  }
  if (header.header_size != kMarketDataBinaryHeaderSize) {
    throw std::runtime_error(fmt::format(
        "binary data file '{}' has unsupported market data header size {}",
        file.string(), header.header_size));
  }
  if (header.flags != kMarketDataBinaryFlags) {
    throw std::runtime_error(fmt::format(
        "binary data file '{}' has unsupported market data flags 0x{:08x}",
        file.string(), header.flags));
  }

  const std::uint16_t expected_feed_type =
      MarketDataBinaryFeedTypeForFeed(expected_feed);
  if (header.feed_type != expected_feed_type) {
    throw std::runtime_error(fmt::format(
        "binary data file '{}' feed mismatch: header={}, expected={}",
        file.string(), MarketDataBinaryFeedTypeName(header.feed_type),
        MarketDataBinaryFeedName(expected_feed)));
  }

  const std::size_t expected_record_size =
      MarketDataBinaryRecordSizeForFeed(expected_feed);
  if (header.record_size != expected_record_size) {
    throw std::runtime_error(fmt::format(
        "binary data file '{}' {} record size mismatch: header={}, expected={}",
        file.string(), MarketDataBinaryFeedName(expected_feed),
        header.record_size, expected_record_size));
  }
}

[[nodiscard]] inline std::uint64_t CheckedMarketDataBinaryRecordCount(
    const std::filesystem::path& file, std::uintmax_t file_size,
    const MarketDataBinaryHeader& header,
    config::DataReaderFeed expected_feed) {
  ValidateMarketDataBinaryHeader(header, expected_feed, file);
  if (file_size < header.header_size) {
    throw std::runtime_error(fmt::format(
        "binary data file '{}' size {} is smaller than market data header {}",
        file.string(), file_size, header.header_size));
  }
  const std::uintmax_t payload_bytes = file_size - header.header_size;
  if (header.record_size == 0 || payload_bytes % header.record_size != 0) {
    throw std::runtime_error(fmt::format(
        "binary data file '{}' payload size {} is not a multiple of {} record "
        "size {}",
        file.string(), payload_bytes, MarketDataBinaryFeedName(expected_feed),
        header.record_size));
  }
  const std::uintmax_t record_count = payload_bytes / header.record_size;
  if (record_count >
      static_cast<std::uintmax_t>(std::numeric_limits<std::uint64_t>::max())) {
    throw std::runtime_error(
        fmt::format("binary data file '{}' is too large", file.string()));
  }
  return static_cast<std::uint64_t>(record_count);
}

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_MARKET_DATA_BINARY_FORMAT_H_
