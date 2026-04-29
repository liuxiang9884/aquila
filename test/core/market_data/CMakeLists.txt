add_executable(core_market_data_types_test
    types_test.cpp
)

target_link_libraries(core_market_data_types_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME core_market_data_types_test
         COMMAND core_market_data_types_test)
