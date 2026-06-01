add_executable(core_exchange_test
    exchange_test.cpp
)

target_link_libraries(core_exchange_test
    PRIVATE
        aquila_core
        GTest::gtest_main
        magic_enum::magic_enum
)

add_test(NAME core_exchange_test
         COMMAND core_exchange_test)

add_executable(core_order_ack_diagnostic_level_test
    order_ack_diagnostic_level_test.cpp
)

target_link_libraries(core_order_ack_diagnostic_level_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME core_order_ack_diagnostic_level_test
         COMMAND core_order_ack_diagnostic_level_test)
