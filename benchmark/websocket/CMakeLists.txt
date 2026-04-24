set(WEBSOCKET_BENCHMARK_TARGETS
    prepared_write_benchmark
    frame_codec_benchmark
    active_spin_benchmark
    session_write_path_benchmark
    session_read_path_benchmark
    runtime_loopback_benchmark
    affinity_policy_comparison_benchmark
    cold_path_handshake_benchmark
)

foreach(target IN LISTS WEBSOCKET_BENCHMARK_TARGETS)
    add_executable(${target}
        ${target}.cpp
    )
    target_link_libraries(${target}
        PRIVATE
            aquila_core
            benchmark::benchmark_main
    )
endforeach()
