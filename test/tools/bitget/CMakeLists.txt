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

add_test(NAME bitget_data_fusion_help_test
         COMMAND bitget_data_fusion --help)

add_test(NAME bitget_data_fusion_dry_run_test
         COMMAND bitget_data_fusion --config
                 config/market_data_fusion/bitget_data_fusion_book_ticker_4sources.toml)

set_tests_properties(bitget_data_fusion_help_test
    bitget_data_fusion_dry_run_test
    PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
