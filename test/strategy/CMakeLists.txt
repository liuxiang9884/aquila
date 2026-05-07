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

add_strategy_gtest(strategy_order_store_test order_store_test.cpp)
add_strategy_gtest(strategy_test strategy_test.cpp)

add_executable(strategy_gate_order_gateway_test
    gate_order_gateway_test.cpp
)

target_link_libraries(strategy_gate_order_gateway_test
    PRIVATE
        aquila_strategy_gate
        GTest::gtest_main
)
add_test(NAME strategy_gate_order_gateway_test COMMAND strategy_gate_order_gateway_test)
