add_executable(websocket_config_test
    ${PROJECT_SOURCE_DIR}/config/websocket_config.cpp
    websocket_config_test.cpp
)

target_link_libraries(websocket_config_test
    PRIVATE
        aquila_core
        GTest::gtest_main
        PkgConfig::tomlplusplus
        nova
)

target_compile_definitions(websocket_config_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME websocket_config_test
         COMMAND websocket_config_test)
