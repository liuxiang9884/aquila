add_executable(core_order_pool_test
    order_pool_test.cpp
)

target_link_libraries(core_order_pool_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME core_order_pool_test
         COMMAND core_order_pool_test)

add_executable(core_order_feedback_event_test
    order_feedback_event_test.cpp
)

target_link_libraries(core_order_feedback_event_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME core_order_feedback_event_test
         COMMAND core_order_feedback_event_test)

add_executable(core_order_decimal_test
    order_decimal_test.cpp
)

target_link_libraries(core_order_decimal_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME core_order_decimal_test
         COMMAND core_order_decimal_test)

add_executable(core_order_latency_test
    order_latency_test.cpp
)

target_link_libraries(core_order_latency_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME core_order_latency_test
         COMMAND core_order_latency_test)

add_executable(core_order_gateway_shm_types_test
    order_gateway_shm_types_test.cpp
)

target_link_libraries(core_order_gateway_shm_types_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME core_order_gateway_shm_types_test
         COMMAND core_order_gateway_shm_types_test)

add_executable(core_order_feedback_shm_test
    order_feedback_shm_test.cpp
)

target_link_libraries(core_order_feedback_shm_test
    PRIVATE
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
        nova
)

add_test(NAME core_order_feedback_shm_test
         COMMAND core_order_feedback_shm_test)

add_executable(core_trading_strategy_context_test
    strategy_context_test.cpp
)

target_link_libraries(core_trading_strategy_context_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME core_trading_strategy_context_test
         COMMAND core_trading_strategy_context_test)

add_executable(core_order_manager_route_test
    order_manager_route_test.cpp
)

target_link_libraries(core_order_manager_route_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME core_order_manager_route_test
         COMMAND core_order_manager_route_test)

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

add_executable(core_trading_contract_namespace_test
    trading_contract_namespace_test.cpp
)

target_link_libraries(core_trading_contract_namespace_test
    PRIVATE
        aquila_config
        aquila_core
        GTest::gtest_main
        nova
)

add_test(NAME core_trading_contract_namespace_test
         COMMAND core_trading_contract_namespace_test)
