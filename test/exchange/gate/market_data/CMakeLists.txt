add_executable(gate_futures_market_data_client_test
    futures_market_data_client_test.cpp
)

target_link_libraries(gate_futures_market_data_client_test
    PRIVATE
        aquila_evaluation
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_futures_market_data_client_test
         COMMAND gate_futures_market_data_client_test)

add_executable(gate_data_session_test
    data_session_test.cpp
)

target_link_libraries(gate_data_session_test
    PRIVATE
        aquila_core
        aquila_evaluation
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_data_session_test
         COMMAND gate_data_session_test)

add_executable(gate_text_envelope_parser_test
    text_envelope_parser_test.cpp
)

target_link_libraries(gate_text_envelope_parser_test
    PRIVATE
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_text_envelope_parser_test
         COMMAND gate_text_envelope_parser_test)

add_executable(gate_subscription_controller_test
    subscription_controller_test.cpp
)

target_link_libraries(gate_subscription_controller_test
    PRIVATE
        aquila_core
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_subscription_controller_test
         COMMAND gate_subscription_controller_test)
