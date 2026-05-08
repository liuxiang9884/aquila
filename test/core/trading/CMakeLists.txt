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
