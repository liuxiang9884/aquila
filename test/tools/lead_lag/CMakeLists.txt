add_executable(signal_csv_writer_test
    signal_csv_writer_test.cpp
    ${PROJECT_SOURCE_DIR}/tools/lead_lag/signal_csv_writer.cpp
)

target_link_libraries(signal_csv_writer_test
    PRIVATE
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

add_test(NAME lead_lag_live_strategy_orders_disabled_smoke
         COMMAND ${CMAKE_COMMAND}
                 -DLEAD_LAG_STRATEGY=$<TARGET_FILE:lead_lag_strategy>
                 -DLEAD_LAG_CONFIG=${CMAKE_CURRENT_SOURCE_DIR}/lead_lag_live_orders_disabled_strategy.toml
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/lead_lag_live_orders_disabled_smoke.cmake)
set_tests_properties(lead_lag_live_strategy_orders_disabled_smoke
                     PROPERTIES
                         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
