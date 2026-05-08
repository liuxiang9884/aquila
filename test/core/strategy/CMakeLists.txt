add_executable(core_strategy_context_test
    strategy_context_test.cpp
)

target_link_libraries(core_strategy_context_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME core_strategy_context_test
         COMMAND core_strategy_context_test)
