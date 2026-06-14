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
