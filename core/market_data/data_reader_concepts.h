#ifndef AQUILA_CORE_MARKET_DATA_DATA_READER_CONCEPTS_H_
#define AQUILA_CORE_MARKET_DATA_DATA_READER_CONCEPTS_H_

#include <concepts>
#include <cstdint>

namespace aquila::market_data {

template <typename ReaderT, typename HandlerT>
concept DataReaderLike =
    requires(ReaderT& reader, HandlerT& handler, std::uint64_t max_events) {
      { reader.Poll(handler) } -> std::convertible_to<std::uint64_t>;
      {
        reader.Drain(handler, max_events)
      } -> std::convertible_to<std::uint64_t>;
    };

template <typename ReaderT>
concept FiniteDataReader = requires(const ReaderT& reader) {
  { reader.finished() } -> std::convertible_to<bool>;
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_DATA_READER_CONCEPTS_H_
