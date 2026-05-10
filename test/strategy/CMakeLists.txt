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

add_strategy_gtest(lead_lag_strategy_interface_test
                   lead_lag_strategy_interface_test.cpp)
target_link_libraries(lead_lag_strategy_interface_test
    PRIVATE
        nova
)

add_strategy_gtest(strategy_order_feedback_shm_integration_test
                   strategy_order_feedback_shm_integration_test.cpp)
target_link_libraries(strategy_order_feedback_shm_integration_test
    PRIVATE
        aquila_core
        nova
)
