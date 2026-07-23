add_executable(strategy_order_gateway_benchmark
    order_gateway_benchmark.cpp
)

target_link_libraries(strategy_order_gateway_benchmark
    PRIVATE
        aquila_strategy
        benchmark::benchmark_main
)

add_executable(lead_lag_strategy_benchmark
    lead_lag_strategy_benchmark.cpp
)

target_link_libraries(lead_lag_strategy_benchmark
    PRIVATE
        aquila_strategy
        benchmark::benchmark_main
)

target_compile_definitions(lead_lag_strategy_benchmark
    PRIVATE
        AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS
)

add_executable(lead_lag_strategy_production_benchmark
    lead_lag_strategy_benchmark.cpp
)

target_link_libraries(lead_lag_strategy_production_benchmark
    PRIVATE
        aquila_strategy
        benchmark::benchmark_main
)

target_compile_definitions(lead_lag_strategy_production_benchmark
    PRIVATE
        AQUILA_LEAD_LAG_STRATEGY_ENABLE_BENCHMARK_SEED_HOOKS
)

add_executable(lead_lag_runtime_benchmark
    lead_lag_runtime_benchmark.cpp
)

target_link_libraries(lead_lag_runtime_benchmark
    PRIVATE
        aquila_strategy
        benchmark::benchmark_main
        nova
)

add_executable(lead_lag_submit_breakdown_benchmark
    lead_lag_submit_breakdown_benchmark.cpp
)

target_link_libraries(lead_lag_submit_breakdown_benchmark
    PRIVATE
        aquila_bitget
        aquila_core
        aquila_gate
        aquila_strategy
        benchmark::benchmark_main
        nova
)

target_compile_definitions(lead_lag_submit_breakdown_benchmark
    PRIVATE
        AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS
)

add_executable(lead_lag_feedback_runtime_benchmark
    lead_lag_feedback_runtime_benchmark.cpp
)

target_link_libraries(lead_lag_feedback_runtime_benchmark
    PRIVATE
        aquila_bitget
        aquila_evaluation
        aquila_gate
        aquila_strategy
        benchmark::benchmark_main
        nova
)

target_compile_definitions(lead_lag_feedback_runtime_benchmark
    PRIVATE
        AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS
)

add_executable(lead_lag_feedback_production_benchmark
    lead_lag_feedback_runtime_benchmark.cpp
)

target_link_libraries(lead_lag_feedback_production_benchmark
    PRIVATE
        aquila_bitget
        aquila_evaluation
        aquila_gate
        aquila_strategy
        benchmark::benchmark_main
        nova
)

target_compile_definitions(lead_lag_feedback_production_benchmark
    PRIVATE
        AQUILA_LEAD_LAG_STRATEGY_ENABLE_BENCHMARK_SEED_HOOKS
)

add_executable(lead_lag_group_container_benchmark
    lead_lag_group_container_benchmark.cpp
)

target_link_libraries(lead_lag_group_container_benchmark
    PRIVATE
        aquila_strategy
        benchmark::benchmark_main
)
