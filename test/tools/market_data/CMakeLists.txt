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

target_compile_definitions(data_reader_recorder_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
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

add_executable(data_fusion_tool_support_test
    data_fusion_tool_support_test.cpp
)

target_link_libraries(data_fusion_tool_support_test
    PRIVATE
        aquila_core
        aquila_gate
        GTest::gtest_main
        nova
)

add_test(NAME data_fusion_tool_support_test
         COMMAND data_fusion_tool_support_test)

add_executable(fusion_cli_traits_test
    fusion_cli_traits_test.cpp
)

target_link_libraries(fusion_cli_traits_test
    PRIVATE
        aquila_config
        aquila_core
        CLI11::CLI11
        GTest::gtest_main
        nova
)

add_test(NAME fusion_cli_traits_test
         COMMAND fusion_cli_traits_test)

add_executable(book_ticker_fusion_cli_test
    book_ticker_fusion_cli_test.cpp
)

target_link_libraries(book_ticker_fusion_cli_test
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(book_ticker_fusion_cli_test
    PRIVATE
        AQUILA_GATE_BOOK_TICKER_FUSION_BINARY="$<TARGET_FILE:gate_book_ticker_fusion>"
        AQUILA_MISSING_BOOK_TICKER_FUSION_CONFIG="${PROJECT_BINARY_DIR}/missing_book_ticker_fusion_config.toml"
)
add_dependencies(book_ticker_fusion_cli_test gate_book_ticker_fusion)

add_test(NAME book_ticker_fusion_cli_test
         COMMAND book_ticker_fusion_cli_test)

add_executable(trade_fusion_cli_test
    trade_fusion_cli_test.cpp
)

target_link_libraries(trade_fusion_cli_test
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(trade_fusion_cli_test
    PRIVATE
        AQUILA_GATE_TRADE_FUSION_BINARY="$<TARGET_FILE:gate_trade_fusion>"
        AQUILA_MISSING_TRADE_FUSION_CONFIG="${PROJECT_BINARY_DIR}/missing_trade_fusion_config.toml"
)
add_dependencies(trade_fusion_cli_test gate_trade_fusion)

add_test(NAME trade_fusion_cli_test
         COMMAND trade_fusion_cli_test)

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

add_executable(data_reader_probe_cli_test
    data_reader_probe_cli_test.cpp
)

target_link_libraries(data_reader_probe_cli_test
    PRIVATE
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
)

target_compile_definitions(data_reader_probe_cli_test
    PRIVATE
        AQUILA_DATA_READER_PROBE_BINARY="$<TARGET_FILE:data_reader_probe>"
        AQUILA_DATA_READER_PROBE_CATALOG="${PROJECT_SOURCE_DIR}/config/instruments/usdt_futures.csv"
)
add_dependencies(data_reader_probe_cli_test data_reader_probe)

add_test(NAME data_reader_probe_cli_test
         COMMAND data_reader_probe_cli_test)
