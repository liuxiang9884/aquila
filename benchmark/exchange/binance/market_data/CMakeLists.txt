add_executable(binance_futures_market_data_benchmark
    futures_market_data_benchmark.cpp
)

target_link_libraries(binance_futures_market_data_benchmark
    PRIVATE
        aquila_core
        aquila_binance
        benchmark::benchmark_main
        fmt::fmt-header-only
        nova
        yyjson::yyjson
)
