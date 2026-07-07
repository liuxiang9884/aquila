add_executable(data_shm_benchmark
    data_shm_benchmark.cpp
)

target_link_libraries(data_shm_benchmark
    PRIVATE
        aquila_core
        benchmark::benchmark_main
        fmt::fmt-header-only
        nova
)

add_executable(data_reader_benchmark
    data_reader_benchmark.cpp
)

target_link_libraries(data_reader_benchmark
    PRIVATE
        aquila_config
        aquila_core
        benchmark::benchmark_main
        fmt::fmt-header-only
        nova
)

add_executable(book_ticker_queue_replay_benchmark
    book_ticker_queue_replay_benchmark.cpp
)

target_link_libraries(book_ticker_queue_replay_benchmark
    PRIVATE
        aquila_core
        aquila_evaluation
        CLI11::CLI11
        fmt::fmt-header-only
        nova
        Threads::Threads
)

add_executable(fastest_route_fusion_benchmark
    fastest_route_fusion_benchmark.cpp
)

target_link_libraries(fastest_route_fusion_benchmark
    PRIVATE
        aquila_core
        benchmark::benchmark_main
        fmt::fmt-header-only
        nova
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(fastest_route_fusion_benchmark
        PRIVATE
            -Wno-stringop-overflow
    )
endif()
