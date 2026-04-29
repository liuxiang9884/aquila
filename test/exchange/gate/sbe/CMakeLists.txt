add_executable(gate_sbe_book_ticker_decoder_test
    book_ticker_decoder_test.cpp
)

target_link_libraries(gate_sbe_book_ticker_decoder_test
    PRIVATE
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_sbe_book_ticker_decoder_test
         COMMAND gate_sbe_book_ticker_decoder_test)

add_executable(gate_sbe_message_dispatcher_test
    message_dispatcher_test.cpp
)

target_link_libraries(gate_sbe_message_dispatcher_test
    PRIVATE
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_sbe_message_dispatcher_test
         COMMAND gate_sbe_message_dispatcher_test)
