add_executable(gate_strategy_order_feedback_action_test
    strategy_order_feedback_action_test.cpp
)

target_link_libraries(gate_strategy_order_feedback_action_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME gate_strategy_order_feedback_action_test
         COMMAND gate_strategy_order_feedback_action_test)

add_executable(gate_demo_strategy_test
    demo_strategy_test.cpp
)

target_link_libraries(gate_demo_strategy_test
    PRIVATE
        aquila_core
        aquila_config
        GTest::gtest_main
)

add_test(NAME gate_demo_strategy_test
         COMMAND gate_demo_strategy_test)

add_executable(gate_strategy_runtime_adapter_test
    strategy_runtime_adapter_test.cpp
)

target_compile_definitions(gate_strategy_runtime_adapter_test
    PRIVATE
        AQUILA_GATE_STRATEGY_RUNTIME_ADAPTER_ENABLE_TEST_HOOKS
)

target_link_libraries(gate_strategy_runtime_adapter_test
    PRIVATE
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_strategy_runtime_adapter_test
         COMMAND gate_strategy_runtime_adapter_test)
