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

add_executable(core_numeric_test
    numeric_test.cpp
)

target_link_libraries(core_numeric_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME core_numeric_test
         COMMAND core_numeric_test)
