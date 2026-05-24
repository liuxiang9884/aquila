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

add_executable(gate_strategy_order_response_conversion_test
    strategy_order_response_conversion_test.cpp
)

target_link_libraries(gate_strategy_order_response_conversion_test
    PRIVATE
        aquila_core
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_strategy_order_response_conversion_test
         COMMAND gate_strategy_order_response_conversion_test)

add_executable(gate_order_session_failure_probe_test
    order_session_failure_probe_test.cpp
)

target_link_libraries(gate_order_session_failure_probe_test
    PRIVATE
        aquila_core
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_order_session_failure_probe_test
         COMMAND gate_order_session_failure_probe_test)

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
