add_subdirectory(websocket)
add_subdirectory(exchange/gate/trading)

add_executable(hello_world_test
    hello_world_test.cpp
)

target_compile_definitions(hello_world_test
    PRIVATE HELLO_WORLD_TEST_TARGET="$<TARGET_FILE:aquila_hello_world>"
)

target_link_libraries(hello_world_test
    PRIVATE GTest::gtest_main
)
