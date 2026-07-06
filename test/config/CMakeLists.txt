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
        nova
)

target_compile_definitions(data_session_config_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME data_session_config_test
         COMMAND data_session_config_test)

add_executable(data_reader_config_test
    data_reader_config_test.cpp
)

target_link_libraries(data_reader_config_test
    PRIVATE
        aquila_config
        GTest::gtest_main
        nova
)

target_compile_definitions(data_reader_config_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME data_reader_config_test
         COMMAND data_reader_config_test)

add_executable(book_ticker_fusion_config_test
    book_ticker_fusion_config_test.cpp
)

target_link_libraries(book_ticker_fusion_config_test
    PRIVATE
        aquila_config
        GTest::gtest_main
        PkgConfig::tomlplusplus
)

target_compile_definitions(book_ticker_fusion_config_test
    PRIVATE
        AQUILA_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME book_ticker_fusion_config_test
         COMMAND book_ticker_fusion_config_test)

add_executable(trade_fusion_config_test
    trade_fusion_config_test.cpp
)

target_link_libraries(trade_fusion_config_test
    PRIVATE
        aquila_config
        GTest::gtest_main
        PkgConfig::tomlplusplus
)

target_compile_definitions(trade_fusion_config_test
    PRIVATE
        AQUILA_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME trade_fusion_config_test
         COMMAND trade_fusion_config_test)

add_executable(order_session_config_test
    order_session_config_test.cpp
)

target_link_libraries(order_session_config_test
    PRIVATE
        aquila_gate
        GTest::gtest_main
        nova
)

target_compile_definitions(order_session_config_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME order_session_config_test
         COMMAND order_session_config_test)

add_executable(order_gateway_config_test
    order_gateway_config_test.cpp
)

target_link_libraries(order_gateway_config_test
    PRIVATE
        aquila_config
        GTest::gtest_main
)

target_compile_definitions(order_gateway_config_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME order_gateway_config_test
         COMMAND order_gateway_config_test)

add_executable(order_feedback_shm_config_test
    order_feedback_shm_config_test.cpp
)

target_link_libraries(order_feedback_shm_config_test
    PRIVATE
        aquila_config
        GTest::gtest_main
)

target_compile_definitions(order_feedback_shm_config_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME order_feedback_shm_config_test
         COMMAND order_feedback_shm_config_test)

add_executable(order_feedback_session_config_test
    order_feedback_session_config_test.cpp
)

target_link_libraries(order_feedback_session_config_test
    PRIVATE
        aquila_gate
        GTest::gtest_main
        nova
)

target_compile_definitions(order_feedback_session_config_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME order_feedback_session_config_test
         COMMAND order_feedback_session_config_test)

add_executable(strategy_config_test
    strategy_config_test.cpp
)

target_link_libraries(strategy_config_test
    PRIVATE
        aquila_config
        GTest::gtest_main
        nova
)

target_compile_definitions(strategy_config_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME strategy_config_test
         COMMAND strategy_config_test)
