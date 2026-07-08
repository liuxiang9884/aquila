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
