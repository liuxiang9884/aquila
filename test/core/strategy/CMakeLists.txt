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

add_executable(core_trading_runtime_test
    trading_runtime_test.cpp
)

target_link_libraries(core_trading_runtime_test
    PRIVATE
        aquila_config
        aquila_core
        GTest::gtest_main
        nova
)

add_test(NAME core_trading_runtime_test
         COMMAND core_trading_runtime_test)
