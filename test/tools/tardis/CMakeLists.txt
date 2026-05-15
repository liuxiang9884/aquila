add_executable(book_ticker_binary_converter_test
    book_ticker_binary_converter_test.cpp
)

target_link_libraries(book_ticker_binary_converter_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME book_ticker_binary_converter_test
         COMMAND book_ticker_binary_converter_test)
