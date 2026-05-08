add_executable(gate_submit_response_parse_benchmark
    submit_response_parse_benchmark.cpp
)

target_link_libraries(gate_submit_response_parse_benchmark
    PRIVATE
        aquila_gate
        benchmark::benchmark_main
        fmt::fmt-header-only
        yyjson::yyjson
)

add_executable(gate_order_session_benchmark
    order_session_benchmark.cpp
)

target_link_libraries(gate_order_session_benchmark
    PRIVATE
        aquila_gate
        benchmark::benchmark_main
)

add_executable(gate_order_feedback_parser_benchmark
    order_feedback_parser_benchmark.cpp
)

target_link_libraries(gate_order_feedback_parser_benchmark
    PRIVATE
        aquila_core
        aquila_gate
        benchmark::benchmark_main
        nova
)
