add_executable(binance_book_ticker_parser_test
    book_ticker_parser_test.cpp
)

target_link_libraries(binance_book_ticker_parser_test
    PRIVATE
        aquila_binance
        GTest::gtest_main
)

add_test(NAME binance_book_ticker_parser_test
         COMMAND binance_book_ticker_parser_test)

add_executable(binance_futures_market_data_client_test
    futures_market_data_client_test.cpp
)

target_link_libraries(binance_futures_market_data_client_test
    PRIVATE
        aquila_core
        aquila_binance
        GTest::gtest_main
)

add_test(NAME binance_futures_market_data_client_test
         COMMAND binance_futures_market_data_client_test)

add_executable(binance_data_session_test
    data_session_test.cpp
)

target_link_libraries(binance_data_session_test
    PRIVATE
        aquila_core
        aquila_binance
        GTest::gtest_main
)

add_test(NAME binance_data_session_test
         COMMAND binance_data_session_test)
