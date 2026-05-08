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

add_executable(gate_order_session_test
    order_session_test.cpp
)

target_link_libraries(gate_order_session_test
    PRIVATE
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_order_session_test
         COMMAND gate_order_session_test)
