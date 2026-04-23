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
