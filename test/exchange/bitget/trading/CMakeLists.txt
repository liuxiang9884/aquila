add_executable(bitget_order_codecs_test
    order_codecs_test.cpp
)

target_link_libraries(bitget_order_codecs_test
    PRIVATE
        aquila_bitget
        GTest::gtest_main
)

add_test(NAME bitget_order_codecs_test
         COMMAND bitget_order_codecs_test)

add_executable(bitget_order_request_encoder_test
    order_request_encoder_test.cpp
)

target_link_libraries(bitget_order_request_encoder_test
    PRIVATE
        aquila_bitget
        GTest::gtest_main
)

add_test(NAME bitget_order_request_encoder_test
         COMMAND bitget_order_request_encoder_test)
