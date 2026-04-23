add_executable(websocket_types_test
    types_test.cpp
)

target_link_libraries(websocket_types_test
    PRIVATE aquila_gate_ws_core
)

add_test(
    NAME websocket_types_test
    COMMAND $<TARGET_FILE:websocket_types_test>
)

add_executable(websocket_runtime_policy_test
    runtime_policy_test.cpp
)

target_link_libraries(websocket_runtime_policy_test
    PRIVATE aquila_gate_ws_core
)

add_test(
    NAME websocket_runtime_policy_test
    COMMAND $<TARGET_FILE:websocket_runtime_policy_test>
)

add_executable(websocket_prepared_write_test
    prepared_write_test.cpp
)

target_link_libraries(websocket_prepared_write_test
    PRIVATE aquila_gate_ws_core
)

add_test(
    NAME websocket_prepared_write_test
    COMMAND $<TARGET_FILE:websocket_prepared_write_test>
)
