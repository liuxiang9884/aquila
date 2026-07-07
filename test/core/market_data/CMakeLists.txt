add_executable(core_market_data_types_test
    types_test.cpp
)

target_link_libraries(core_market_data_types_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME core_market_data_types_test
         COMMAND core_market_data_types_test)

add_executable(core_market_data_shm_test
    data_shm_test.cpp
)

target_link_libraries(core_market_data_shm_test
    PRIVATE
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
        nova
)

add_test(NAME core_market_data_shm_test
         COMMAND core_market_data_shm_test)

add_executable(core_market_data_fusion_fastest_route_test
    fusion/fastest_route_test.cpp
)

target_link_libraries(core_market_data_fusion_fastest_route_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME core_market_data_fusion_fastest_route_test
         COMMAND core_market_data_fusion_fastest_route_test)

add_executable(core_market_data_fusion_book_ticker_test
    fusion/book_ticker_test.cpp
)

target_link_libraries(core_market_data_fusion_book_ticker_test
    PRIVATE
        aquila_core
        GTest::gtest_main
        nova
)

add_test(NAME core_market_data_fusion_book_ticker_test
         COMMAND core_market_data_fusion_book_ticker_test)

add_executable(core_market_data_fusion_book_ticker_metadata_test
    fusion/book_ticker_metadata_test.cpp
)

target_link_libraries(core_market_data_fusion_book_ticker_metadata_test
    PRIVATE
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
)

add_test(NAME core_market_data_fusion_book_ticker_metadata_test
         COMMAND core_market_data_fusion_book_ticker_metadata_test)

add_executable(core_market_data_fusion_trade_metadata_test
    fusion/trade_metadata_test.cpp
)

target_link_libraries(core_market_data_fusion_trade_metadata_test
    PRIVATE
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
)

add_test(NAME core_market_data_fusion_trade_metadata_test
         COMMAND core_market_data_fusion_trade_metadata_test)

add_executable(core_market_data_fusion_metadata_policy_test
    fusion/metadata_policy_test.cpp
)

target_link_libraries(core_market_data_fusion_metadata_policy_test
    PRIVATE
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
)

add_test(NAME core_market_data_fusion_metadata_policy_test
         COMMAND core_market_data_fusion_metadata_policy_test)

add_executable(core_market_data_fusion_trade_test
    fusion/trade_test.cpp
)

target_link_libraries(core_market_data_fusion_trade_test
    PRIVATE
        aquila_core
        GTest::gtest_main
        nova
)

add_test(NAME core_market_data_fusion_trade_test
         COMMAND core_market_data_fusion_trade_test)

add_executable(core_market_data_fusion_book_ticker_runner_test
    fusion/book_ticker_runner_test.cpp
)

target_link_libraries(core_market_data_fusion_book_ticker_runner_test
    PRIVATE
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
        nova
)

add_test(NAME core_market_data_fusion_book_ticker_runner_test
         COMMAND core_market_data_fusion_book_ticker_runner_test)

add_executable(core_market_data_fusion_trade_runner_test
    fusion/trade_runner_test.cpp
)

target_link_libraries(core_market_data_fusion_trade_runner_test
    PRIVATE
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
        nova
)

add_test(NAME core_market_data_fusion_trade_runner_test
         COMMAND core_market_data_fusion_trade_runner_test)

add_test(NAME core_market_data_fusion_book_ticker_static_review
         COMMAND bash -c [=[
if rg -n 'BookTickerFusionConfig config_|config_\.max_events_per_source|BookTicker ticker\{\};' "$1"; then
  exit 1
fi
]=] _
         ${PROJECT_SOURCE_DIR}/core/market_data/fusion/book_ticker.h)

add_executable(core_market_data_fusion_book_ticker_thread_test
    fusion/book_ticker_thread_test.cpp
)

target_link_libraries(core_market_data_fusion_book_ticker_thread_test
    PRIVATE
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
        nova
)

add_test(NAME core_market_data_fusion_book_ticker_thread_test
         COMMAND core_market_data_fusion_book_ticker_thread_test)

add_executable(core_market_data_fusion_trade_thread_test
    fusion/trade_thread_test.cpp
)

target_link_libraries(core_market_data_fusion_trade_thread_test
    PRIVATE
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
        nova
)

add_test(NAME core_market_data_fusion_trade_thread_test
         COMMAND core_market_data_fusion_trade_thread_test)

add_executable(core_market_data_book_ticker_replay_benchmark_support_test
    book_ticker_replay_benchmark_support_test.cpp
)

target_link_libraries(core_market_data_book_ticker_replay_benchmark_support_test
    PRIVATE
        aquila_evaluation
        GTest::gtest_main
)

add_test(NAME core_market_data_book_ticker_replay_benchmark_support_test
         COMMAND core_market_data_book_ticker_replay_benchmark_support_test)

add_executable(core_market_data_data_session_diagnostics_test
    data_session_diagnostics_test.cpp
)

target_link_libraries(core_market_data_data_session_diagnostics_test
    PRIVATE
        aquila_core
        GTest::gtest_main
        nova
)

add_test(NAME core_market_data_data_session_diagnostics_test
         COMMAND core_market_data_data_session_diagnostics_test)

add_executable(core_market_data_realtime_data_reader_test
    realtime_data_reader_test.cpp
)

target_link_libraries(core_market_data_realtime_data_reader_test
    PRIVATE
        aquila_config
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
        nova
)

add_test(NAME core_market_data_realtime_data_reader_test
         COMMAND core_market_data_realtime_data_reader_test)

add_executable(core_market_data_historical_data_reader_test
    historical_data_reader_test.cpp
)

target_link_libraries(core_market_data_historical_data_reader_test
    PRIVATE
        aquila_config
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
)

add_test(NAME core_market_data_historical_data_reader_test
         COMMAND core_market_data_historical_data_reader_test)
