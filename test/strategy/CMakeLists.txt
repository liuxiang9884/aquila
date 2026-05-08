function(add_strategy_gtest target source_file)
    add_executable(${target}
        ${source_file}
    )

    target_link_libraries(${target}
        PRIVATE
            aquila_strategy
            GTest::gtest_main
    )
    add_test(NAME ${target} COMMAND ${target})
endfunction()

add_strategy_gtest(strategy_test strategy_test.cpp)

add_strategy_gtest(strategy_order_feedback_shm_integration_test
                   strategy_order_feedback_shm_integration_test.cpp)
target_link_libraries(strategy_order_feedback_shm_integration_test
    PRIVATE
        aquila_core
        nova
)
