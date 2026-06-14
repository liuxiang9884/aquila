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

add_executable(book_ticker_fusion_config_test
    book_ticker_fusion_config_test.cpp
    ${PROJECT_SOURCE_DIR}/tools/market_data/book_ticker_fusion_config.cpp
)

target_link_libraries(book_ticker_fusion_config_test
    PRIVATE
        aquila_core
        GTest::gtest_main
        PkgConfig::tomlplusplus
)

target_compile_definitions(book_ticker_fusion_config_test
    PRIVATE
        AQUILA_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME book_ticker_fusion_config_test
         COMMAND book_ticker_fusion_config_test)

add_executable(book_ticker_fusion_runner_test
    book_ticker_fusion_runner_test.cpp
    ${PROJECT_SOURCE_DIR}/tools/market_data/book_ticker_fusion_config.cpp
)

target_link_libraries(book_ticker_fusion_runner_test
    PRIVATE
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
        nova
        PkgConfig::tomlplusplus
)

add_test(NAME book_ticker_fusion_runner_test
         COMMAND book_ticker_fusion_runner_test)

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
