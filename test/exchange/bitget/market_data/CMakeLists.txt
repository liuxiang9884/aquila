add_executable(bitget_futures_market_data_client_test
    futures_market_data_client_test.cpp
)

target_link_libraries(bitget_futures_market_data_client_test
    PRIVATE
        aquila_bitget
        aquila_evaluation
        GTest::gtest_main
)

add_test(NAME bitget_futures_market_data_client_test
         COMMAND bitget_futures_market_data_client_test)

add_executable(bitget_data_session_test
    data_session_test.cpp
)

target_link_libraries(bitget_data_session_test
    PRIVATE
        aquila_bitget
        aquila_evaluation
        GTest::gtest_main
)

add_test(NAME bitget_data_session_test
         COMMAND bitget_data_session_test)

add_executable(bitget_data_session_config_test
    data_session_config_test.cpp
)

target_link_libraries(bitget_data_session_config_test
    PRIVATE
        aquila_bitget
        GTest::gtest_main
        nova
)

target_compile_definitions(bitget_data_session_config_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME bitget_data_session_config_test
         COMMAND bitget_data_session_config_test)
