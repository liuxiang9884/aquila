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
