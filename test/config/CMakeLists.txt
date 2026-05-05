add_executable(websocket_config_test
    websocket_config_test.cpp
)

target_link_libraries(websocket_config_test
    PRIVATE
        aquila_config
        GTest::gtest_main
)

target_compile_definitions(websocket_config_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME websocket_config_test
         COMMAND websocket_config_test)

add_executable(data_session_config_test
    data_session_config_test.cpp
)

target_link_libraries(data_session_config_test
    PRIVATE
        aquila_config
        aquila_binance
        aquila_gate
        GTest::gtest_main
)

target_compile_definitions(data_session_config_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME data_session_config_test
         COMMAND data_session_config_test)
