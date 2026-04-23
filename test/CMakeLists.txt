add_subdirectory(websocket)

add_test(
    NAME hello_world_smoke_test
    COMMAND $<TARGET_FILE:aquila_hello_world>
)

set_tests_properties(hello_world_smoke_test PROPERTIES
    PASS_REGULAR_EXPRESSION "hello world"
)
