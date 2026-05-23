add_executable(core_order_pool_benchmark
    order_pool_benchmark.cpp
)

target_link_libraries(core_order_pool_benchmark
    PRIVATE
        aquila_core
        benchmark::benchmark_main
)

add_executable(core_order_decimal_benchmark
    order_decimal_benchmark.cpp
)

target_link_libraries(core_order_decimal_benchmark
    PRIVATE
        aquila_core
        benchmark::benchmark_main
)

add_executable(order_feedback_shm_benchmark
    order_feedback_shm_benchmark.cpp
)

target_link_libraries(order_feedback_shm_benchmark
    PRIVATE
        aquila_core
        benchmark::benchmark_main
        nova
)
