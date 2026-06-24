add_executable(signal_csv_writer_test
    signal_csv_writer_test.cpp
)

target_link_libraries(signal_csv_writer_test
    PRIVATE
        aquila_lead_lag_diagnostics
        aquila_core
        aquila_strategy
        GTest::gtest_main
        magic_enum::magic_enum
        nova
        quill::quill
)

add_test(NAME signal_csv_writer_test
         COMMAND signal_csv_writer_test)

add_executable(lead_lag_live_strategy_test
    live_strategy_test.cpp
)

target_link_libraries(lead_lag_live_strategy_test
    PRIVATE
        aquila_config
        aquila_core
        aquila_strategy
        GTest::gtest_main
        nova
)

add_test(NAME lead_lag_live_strategy_test
         COMMAND lead_lag_live_strategy_test)

add_test(NAME lead_lag_live_strategy_orders_missing_credentials_smoke
         COMMAND ${CMAKE_COMMAND}
                 -DLEAD_LAG_STRATEGY=$<TARGET_FILE:lead_lag_strategy>
                 -DLEAD_LAG_CONFIG=${CMAKE_CURRENT_SOURCE_DIR}/lead_lag_live_orders_strategy.toml
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/lead_lag_live_orders_missing_credentials_smoke.cmake)
set_tests_properties(lead_lag_live_strategy_orders_missing_credentials_smoke
                     PROPERTIES
                         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_executable(lead_lag_freshness_preflight_test
    freshness_preflight_test.cpp
    ${PROJECT_SOURCE_DIR}/tools/lead_lag/freshness_preflight.cpp
)

target_link_libraries(lead_lag_freshness_preflight_test
    PRIVATE
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
        magic_enum::magic_enum
        PkgConfig::tomlplusplus
)

add_test(NAME lead_lag_freshness_preflight_test
         COMMAND lead_lag_freshness_preflight_test)

add_executable(lead_lag_freshness_preflight_cli_test
    freshness_preflight_cli_test.cpp
)

target_link_libraries(lead_lag_freshness_preflight_cli_test
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(lead_lag_freshness_preflight_cli_test
    PRIVATE
        AQUILA_LEAD_LAG_FRESHNESS_PREFLIGHT_BINARY="$<TARGET_FILE:lead_lag_freshness_preflight>"
        AQUILA_MISSING_FRESHNESS_PREFLIGHT_CONFIG="${PROJECT_BINARY_DIR}/missing_freshness_preflight_data_reader.toml"
)
add_dependencies(lead_lag_freshness_preflight_cli_test
                 lead_lag_freshness_preflight)

add_test(NAME lead_lag_freshness_preflight_cli_test
         COMMAND lead_lag_freshness_preflight_cli_test)

add_executable(lead_lag_lag_vol_guard_audit_test
    lag_vol_guard_audit_test.cpp
    ${PROJECT_SOURCE_DIR}/tools/lead_lag/lag_vol_guard_audit.cpp
)

target_link_libraries(lead_lag_lag_vol_guard_audit_test
    PRIVATE
        aquila_core
        aquila_strategy
        fmt::fmt-header-only
        GTest::gtest_main
        magic_enum::magic_enum
        nova
        quill::quill
)

add_test(NAME lead_lag_lag_vol_guard_audit_test
         COMMAND lead_lag_lag_vol_guard_audit_test)
