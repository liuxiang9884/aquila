add_executable(bitget_order_session_benchmark
    order_session_benchmark.cpp
)

target_link_libraries(bitget_order_session_benchmark
    PRIVATE
        aquila_bitget
        benchmark::benchmark_main
)
