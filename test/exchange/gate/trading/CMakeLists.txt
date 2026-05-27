add_executable(gate_submit_response_parser_test
    submit_response_parser_test.cpp
)

target_link_libraries(gate_submit_response_parser_test
    PRIVATE
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_submit_response_parser_test
         COMMAND gate_submit_response_parser_test)

add_executable(gate_order_feedback_parser_test
    order_feedback_parser_test.cpp
)

target_link_libraries(gate_order_feedback_parser_test
    PRIVATE
        aquila_evaluation
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_order_feedback_parser_test
         COMMAND gate_order_feedback_parser_test)

add_executable(gate_order_feedback_session_test
    order_feedback_session_test.cpp
)

target_link_libraries(gate_order_feedback_session_test
    PRIVATE
        aquila_evaluation
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_order_feedback_session_test
         COMMAND gate_order_feedback_session_test)

add_executable(gate_order_codecs_test
    order_codecs_test.cpp
)

target_link_libraries(gate_order_codecs_test
    PRIVATE
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_order_codecs_test
         COMMAND gate_order_codecs_test)

add_executable(gate_order_request_encoder_test
    order_request_encoder_test.cpp
)

target_link_libraries(gate_order_request_encoder_test
    PRIVATE
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_order_request_encoder_test
         COMMAND gate_order_request_encoder_test)

add_executable(gate_order_latency_diagnostics_test
    order_latency_diagnostics_test.cpp
)

target_link_libraries(gate_order_latency_diagnostics_test
    PRIVATE
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_order_latency_diagnostics_test
         COMMAND gate_order_latency_diagnostics_test)

add_executable(gate_order_session_test
    order_session_test.cpp
)

target_compile_definitions(gate_order_session_test
    PRIVATE
        AQUILA_GATE_ORDER_SESSION_ENABLE_TEST_HOOKS
)

target_link_libraries(gate_order_session_test
    PRIVATE
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_order_session_test
         COMMAND gate_order_session_test)

add_executable(gate_order_session_runtime_adapter_test
    order_session_runtime_adapter_test.cpp
)

target_compile_definitions(gate_order_session_runtime_adapter_test
    PRIVATE
        AQUILA_GATE_ORDER_SESSION_RUNTIME_ADAPTER_ENABLE_TEST_HOOKS
)

target_link_libraries(gate_order_session_runtime_adapter_test
    PRIVATE
        aquila_gate
        magic_enum::magic_enum
        nova
        GTest::gtest_main
)

add_test(NAME gate_order_session_runtime_adapter_test
         COMMAND gate_order_session_runtime_adapter_test)
