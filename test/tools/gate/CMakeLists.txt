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
