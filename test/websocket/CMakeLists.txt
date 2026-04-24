function(add_websocket_gtest target source_file)
    add_executable(${target}
        ${source_file}
    )

    target_link_libraries(${target}
        PRIVATE aquila_core GTest::gtest_main
    )
endfunction()

add_websocket_gtest(websocket_types_test types_test.cpp)
add_websocket_gtest(websocket_runtime_policy_test runtime_policy_test.cpp)
add_websocket_gtest(websocket_prepared_write_test prepared_write_test.cpp)
add_websocket_gtest(websocket_handshake_test handshake_test.cpp)
add_websocket_gtest(websocket_frame_codec_test frame_codec_test.cpp)
add_websocket_gtest(websocket_tls_socket_test tls_socket_test.cpp)
add_websocket_gtest(websocket_critical_session_test critical_session_test.cpp)
add_websocket_gtest(websocket_loopback_integration_test
                    websocket_loopback_integration_test.cpp)
