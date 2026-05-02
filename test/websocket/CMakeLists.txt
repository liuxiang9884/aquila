function(add_websocket_gtest target source_file)
    add_executable(${target}
        ${source_file}
    )

    target_link_libraries(${target}
        PRIVATE aquila_core GTest::gtest_main magic_enum::magic_enum
    )
    add_test(NAME ${target} COMMAND ${target})
endfunction()

add_websocket_gtest(websocket_types_test types_test.cpp)
add_websocket_gtest(websocket_runtime_policy_test runtime_policy_test.cpp)
add_websocket_gtest(websocket_prepared_write_test prepared_write_test.cpp)
add_websocket_gtest(websocket_handshake_test handshake_test.cpp)
add_websocket_gtest(websocket_frame_codec_test frame_codec_test.cpp)
target_link_libraries(websocket_frame_codec_test
    PRIVATE
        aquila_evaluation
)
add_websocket_gtest(websocket_tls_socket_test tls_socket_test.cpp)
add_websocket_gtest(websocket_critical_session_test critical_session_test.cpp)
add_websocket_gtest(websocket_reconnect_classifier_test
                    reconnect_classifier_test.cpp)
add_websocket_gtest(websocket_backoff_compute_test backoff_compute_test.cpp)
add_websocket_gtest(websocket_degraded_evaluator_test
                    degraded_evaluator_test.cpp)
add_websocket_gtest(websocket_cold_path_loop_test cold_path_loop_test.cpp)
set_tests_properties(websocket_cold_path_loop_test PROPERTIES TIMEOUT 5)
add_websocket_gtest(websocket_loopback_integration_test
                    websocket_loopback_integration_test.cpp)
add_websocket_gtest(websocket_client_reconnect_test
                    websocket_client_reconnect_test.cpp)
set_tests_properties(websocket_client_reconnect_test PROPERTIES TIMEOUT 30)
add_websocket_gtest(websocket_client_degraded_test
                    websocket_client_degraded_test.cpp)
set_tests_properties(websocket_client_degraded_test PROPERTIES TIMEOUT 10)
add_websocket_gtest(websocket_latency_compare_support_test
                    latency_compare_support_test.cpp)
target_link_libraries(websocket_latency_compare_support_test
    PRIVATE
        simdjson::simdjson
)
add_websocket_gtest(websocket_plain_client_test
                    plain_websocket_client_test.cpp)
set_tests_properties(websocket_plain_client_test PROPERTIES TIMEOUT 10)
