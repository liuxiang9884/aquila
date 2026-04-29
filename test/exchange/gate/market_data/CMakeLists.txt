add_executable(gate_futures_market_data_client_test
    futures_market_data_client_test.cpp
)

target_link_libraries(gate_futures_market_data_client_test
    PRIVATE
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_futures_market_data_client_test
         COMMAND gate_futures_market_data_client_test)
