add_executable(bitget_data_fusion_config_test
    bitget_data_fusion_config_test.cpp
    ${PROJECT_SOURCE_DIR}/tools/bitget/bitget_data_fusion_config.cpp
)

target_link_libraries(bitget_data_fusion_config_test
    PRIVATE
        aquila_core
        GTest::gtest_main
        PkgConfig::tomlplusplus
)

add_test(NAME bitget_data_fusion_config_test
         COMMAND bitget_data_fusion_config_test)

add_test(NAME bitget_data_session_help_test
         COMMAND bitget_data_session --help)

add_test(NAME bitget_data_session_dry_run_test
         COMMAND bitget_data_session --config
                 config/data_sessions/bitget_data_session.toml)

set_tests_properties(bitget_data_session_help_test
    bitget_data_session_dry_run_test
    PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_executable(bitget_private_session_probe_outcome_test
    private_session_probe_outcome_test.cpp
)

target_include_directories(bitget_private_session_probe_outcome_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}
)

target_link_libraries(bitget_private_session_probe_outcome_test
    PRIVATE
        GTest::gtest_main
)

add_test(NAME bitget_private_session_probe_outcome_test
         COMMAND bitget_private_session_probe_outcome_test)

add_executable(bitget_order_session_rtt_probe_test
    order_session_rtt_probe/order_session_rtt_probe_test.cpp
    ${PROJECT_SOURCE_DIR}/tools/bitget/order_session_rtt_probe/config.cpp
    ${PROJECT_SOURCE_DIR}/tools/bitget/order_session_rtt_probe/connection_plan.cpp
    ${PROJECT_SOURCE_DIR}/tools/bitget/order_session_rtt_probe/sample_csv_writer.cpp
)

target_include_directories(bitget_order_session_rtt_probe_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}
)

target_link_libraries(bitget_order_session_rtt_probe_test
    PRIVATE
        aquila_bitget
        aquila_config
        absl::flat_hash_map
        absl::flat_hash_set
        fmt::fmt-header-only
        GTest::gtest_main
        PkgConfig::tomlplusplus
        unofficial::vincentlaucsb-csv-parser::csv
)

add_test(NAME bitget_order_session_rtt_probe_test
         COMMAND bitget_order_session_rtt_probe_test)

add_test(NAME bitget_order_session_rtt_probe_help_test
         COMMAND bitget_order_session_rtt_probe --help)

add_test(NAME bitget_order_session_rtt_probe_dry_run_test
         COMMAND bitget_order_session_rtt_probe --config
                 config/order_session_rtt_probe/bitget_order_session_rtt_probe.toml)

add_test(NAME bitget_order_session_rtt_probe_preflight_test
         COMMAND bitget_order_session_rtt_probe --config
                 config/order_session_rtt_probe/bitget_order_session_rtt_probe.toml
                 --live-preflight)

set_tests_properties(bitget_order_session_rtt_probe_help_test
    bitget_order_session_rtt_probe_dry_run_test
    bitget_order_session_rtt_probe_preflight_test
    PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_test(NAME bitget_data_fusion_help_test
         COMMAND bitget_data_fusion --help)

add_test(NAME bitget_data_fusion_dry_run_test
         COMMAND bitget_data_fusion --config
                 config/market_data_fusion/bitget_data_fusion_book_ticker_4sources.toml)

add_test(NAME bitget_data_fusion_trade_dry_run_test
         COMMAND bitget_data_fusion --config
                 config/market_data_fusion/bitget_data_fusion_trade_4sources.toml)

add_test(NAME bitget_data_fusion_book_ticker_trade_dry_run_test
         COMMAND bitget_data_fusion --config
                 config/market_data_fusion/bitget_data_fusion_book_ticker_trade_4sources.toml)

set_tests_properties(bitget_data_fusion_help_test
    bitget_data_fusion_dry_run_test
    bitget_data_fusion_trade_dry_run_test
    bitget_data_fusion_book_ticker_trade_dry_run_test
    PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_test(NAME bitget_order_feedback_session_help_test
         COMMAND bitget_order_feedback_session --help)

add_test(NAME bitget_order_feedback_session_dry_run_test
         COMMAND bitget_order_feedback_session --config
                 config/order_feedback/bitget_order_feedback_session.toml)

set_tests_properties(bitget_order_feedback_session_help_test
    bitget_order_feedback_session_dry_run_test
    PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_test(NAME bitget_order_gateway_help_test
         COMMAND bitget_order_gateway --help)

add_test(NAME bitget_order_gateway_validate_only_test
         COMMAND bitget_order_gateway --config
                 config/order_gateways/bitget_order_gateway.toml
                 --validate-only)

add_test(NAME bitget_order_gateway_dry_run_test
         COMMAND bitget_order_gateway --config
                 config/order_gateways/bitget_order_gateway.toml)

set_tests_properties(bitget_order_gateway_help_test
    bitget_order_gateway_validate_only_test
    bitget_order_gateway_dry_run_test
    PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
