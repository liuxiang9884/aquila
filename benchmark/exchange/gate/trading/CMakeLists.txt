add_executable(gate_submit_response_parse_benchmark
    submit_response_parse_benchmark.cpp
)

target_link_libraries(gate_submit_response_parse_benchmark
    PRIVATE
        aquila_gate
        benchmark::benchmark_main
        fmt::fmt-header-only
)
