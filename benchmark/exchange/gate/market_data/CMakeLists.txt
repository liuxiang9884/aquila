add_executable(gate_futures_market_data_benchmark
    futures_market_data_benchmark.cpp
)

target_link_libraries(gate_futures_market_data_benchmark
    PRIVATE
        aquila_core
        aquila_evaluation
        aquila_gate
        benchmark::benchmark_main
)
