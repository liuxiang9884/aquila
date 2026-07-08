add_executable(bitget_sbe_book_ticker_decoder_test
    book_ticker_decoder_test.cpp
)

target_link_libraries(bitget_sbe_book_ticker_decoder_test
    PRIVATE
        aquila_bitget
        aquila_evaluation
        GTest::gtest_main
)

add_test(NAME bitget_sbe_book_ticker_decoder_test
         COMMAND bitget_sbe_book_ticker_decoder_test)

add_executable(bitget_sbe_trade_decoder_test
    trade_decoder_test.cpp
)

target_link_libraries(bitget_sbe_trade_decoder_test
    PRIVATE
        aquila_bitget
        aquila_evaluation
        GTest::gtest_main
)

add_test(NAME bitget_sbe_trade_decoder_test
         COMMAND bitget_sbe_trade_decoder_test)

add_executable(bitget_sbe_message_dispatcher_test
    message_dispatcher_test.cpp
)

target_link_libraries(bitget_sbe_message_dispatcher_test
    PRIVATE
        aquila_bitget
        GTest::gtest_main
)

add_test(NAME bitget_sbe_message_dispatcher_test
         COMMAND bitget_sbe_message_dispatcher_test)
