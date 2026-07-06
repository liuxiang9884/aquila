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

add_executable(core_market_data_fastest_route_fusion_test
    fastest_route_fusion_test.cpp
)

target_link_libraries(core_market_data_fastest_route_fusion_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME core_market_data_fastest_route_fusion_test
         COMMAND core_market_data_fastest_route_fusion_test)

add_executable(core_market_data_book_ticker_fusion_test
    book_ticker_fusion_test.cpp
)

target_link_libraries(core_market_data_book_ticker_fusion_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME core_market_data_book_ticker_fusion_test
         COMMAND core_market_data_book_ticker_fusion_test)

add_executable(core_market_data_book_ticker_fusion_metadata_test
    book_ticker_fusion_metadata_test.cpp
)

target_link_libraries(core_market_data_book_ticker_fusion_metadata_test
    PRIVATE
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
)

add_test(NAME core_market_data_book_ticker_fusion_metadata_test
         COMMAND core_market_data_book_ticker_fusion_metadata_test)

add_executable(core_market_data_trade_fusion_metadata_test
    trade_fusion_metadata_test.cpp
)

target_link_libraries(core_market_data_trade_fusion_metadata_test
    PRIVATE
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
)

add_test(NAME core_market_data_trade_fusion_metadata_test
         COMMAND core_market_data_trade_fusion_metadata_test)

add_executable(core_market_data_trade_fusion_test
    trade_fusion_test.cpp
)

target_link_libraries(core_market_data_trade_fusion_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME core_market_data_trade_fusion_test
         COMMAND core_market_data_trade_fusion_test)

add_executable(core_market_data_book_ticker_fusion_runner_test
    book_ticker_fusion_runner_test.cpp
)

target_link_libraries(core_market_data_book_ticker_fusion_runner_test
    PRIVATE
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
        nova
)

add_test(NAME core_market_data_book_ticker_fusion_runner_test
         COMMAND core_market_data_book_ticker_fusion_runner_test)

add_executable(core_market_data_trade_fusion_runner_test
    trade_fusion_runner_test.cpp
)

target_link_libraries(core_market_data_trade_fusion_runner_test
    PRIVATE
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
        nova
)

add_test(NAME core_market_data_trade_fusion_runner_test
         COMMAND core_market_data_trade_fusion_runner_test)

add_test(NAME core_market_data_book_ticker_fusion_static_review
         COMMAND bash -c [=[
if rg -n 'BookTickerFusionConfig config_|config_\.max_events_per_source|BookTicker ticker\{\};' "$1"; then
  exit 1
fi
]=] _
         ${PROJECT_SOURCE_DIR}/core/market_data/book_ticker_fusion_runner.h)

add_executable(core_market_data_book_ticker_fusion_thread_test
    book_ticker_fusion_thread_test.cpp
)

target_link_libraries(core_market_data_book_ticker_fusion_thread_test
    PRIVATE
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
        nova
)

add_test(NAME core_market_data_book_ticker_fusion_thread_test
         COMMAND core_market_data_book_ticker_fusion_thread_test)

add_executable(core_market_data_trade_fusion_thread_test
    trade_fusion_thread_test.cpp
)

target_link_libraries(core_market_data_trade_fusion_thread_test
    PRIVATE
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
        nova
)

add_test(NAME core_market_data_trade_fusion_thread_test
         COMMAND core_market_data_trade_fusion_thread_test)

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
