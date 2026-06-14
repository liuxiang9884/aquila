add_executable(data_reader_recorder_test
    data_reader_recorder_test.cpp
    ${PROJECT_SOURCE_DIR}/tools/market_data/data_reader_recorder_config.cpp
)

target_link_libraries(data_reader_recorder_test
    PRIVATE
        aquila_config
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
        nova
)

add_test(NAME data_reader_recorder_test
         COMMAND data_reader_recorder_test)

add_executable(data_reader_tool_logging_test
    data_reader_tool_logging_test.cpp
)

target_link_libraries(data_reader_tool_logging_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME data_reader_tool_logging_test
         COMMAND data_reader_tool_logging_test)

add_executable(book_ticker_fusion_metadata_test
    book_ticker_fusion_metadata_test.cpp
)

target_link_libraries(book_ticker_fusion_metadata_test
    PRIVATE
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
)

add_test(NAME book_ticker_fusion_metadata_test
         COMMAND book_ticker_fusion_metadata_test)

add_executable(data_reader_probe_mode_test
    data_reader_probe_mode_test.cpp
)

target_link_libraries(data_reader_probe_mode_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME data_reader_probe_mode_test
         COMMAND data_reader_probe_mode_test)
