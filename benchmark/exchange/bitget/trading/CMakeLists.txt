add_executable(bitget_order_session_benchmark
    order_session_benchmark.cpp
)

target_link_libraries(bitget_order_session_benchmark
    PRIVATE
        aquila_bitget
        benchmark::benchmark_main
)

add_executable(bitget_order_feedback_parser_benchmark
    order_feedback_parser_benchmark.cpp
)

target_link_libraries(bitget_order_feedback_parser_benchmark
    PRIVATE
        aquila_bitget
        benchmark::benchmark_main
)
