add_executable(strategy_order_gateway_benchmark
    order_gateway_benchmark.cpp
)

target_link_libraries(strategy_order_gateway_benchmark
    PRIVATE
        aquila_strategy
        benchmark::benchmark_main
)
