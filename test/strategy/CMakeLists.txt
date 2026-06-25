function(add_strategy_gtest target source_file)
    add_executable(${target}
        ${source_file}
    )

    target_link_libraries(${target}
        PRIVATE
            aquila_strategy
            GTest::gtest_main
    )
    target_compile_definitions(${target}
        PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    add_test(NAME ${target} COMMAND ${target})
endfunction()

add_strategy_gtest(strategy_test strategy_test.cpp)

add_strategy_gtest(lead_lag_config_test lead_lag_config_test.cpp)

add_strategy_gtest(lead_lag_raw_market_state_test
                   lead_lag_raw_market_state_test.cpp)

add_strategy_gtest(lead_lag_recorders_test lead_lag_recorders_test.cpp)

add_strategy_gtest(lead_lag_alignment_test lead_lag_alignment_test.cpp)

add_strategy_gtest(lead_lag_drift_guard_test lead_lag_drift_guard_test.cpp)

add_strategy_gtest(lead_lag_threshold_test lead_lag_threshold_test.cpp)

add_strategy_gtest(lead_lag_signal_test lead_lag_signal_test.cpp)

add_strategy_gtest(lead_lag_feedback_state_test
                   lead_lag_feedback_state_test.cpp)

add_strategy_gtest(lead_lag_strategy_interface_test
                   lead_lag_strategy_interface_test.cpp)
target_link_libraries(lead_lag_strategy_interface_test
    PRIVATE
        nova
)
target_compile_definitions(lead_lag_strategy_interface_test
    PRIVATE
        AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS
)
if(AQUILA_ENABLE_LEAD_LAG_MARKET_CALC_CSV)
    target_compile_definitions(lead_lag_strategy_interface_test
        PRIVATE
            AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV
    )

    add_executable(lead_lag_market_calc_csv_writer_test
        lead_lag_market_calc_csv_writer_test.cpp
    )
    target_link_libraries(lead_lag_market_calc_csv_writer_test
        PRIVATE
            aquila_lead_lag_diagnostics
            aquila_strategy
            GTest::gtest_main
            magic_enum::magic_enum
            nova
            quill::quill
    )
    add_test(NAME lead_lag_market_calc_csv_writer_test
             COMMAND lead_lag_market_calc_csv_writer_test)
endif()

add_strategy_gtest(strategy_order_feedback_shm_integration_test
                   strategy_order_feedback_shm_integration_test.cpp)
target_link_libraries(strategy_order_feedback_shm_integration_test
    PRIVATE
        aquila_core
        nova
)
