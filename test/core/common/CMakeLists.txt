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

add_executable(core_order_ack_diagnostic_level_legacy_off_test
    order_ack_diagnostic_level_test.cpp
)

target_include_directories(core_order_ack_diagnostic_level_legacy_off_test
    PRIVATE ${PROJECT_SOURCE_DIR}
)

target_compile_definitions(core_order_ack_diagnostic_level_legacy_off_test
    PRIVATE
        AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION=0
        AQUILA_TEST_EXPECT_LEGACY_SOCKET_TIMESTAMPING_OFF_MAPS_TO_L3=1
)

target_link_libraries(core_order_ack_diagnostic_level_legacy_off_test
    PRIVATE
        GTest::gtest_main
)

add_test(NAME core_order_ack_diagnostic_level_legacy_off_test
         COMMAND core_order_ack_diagnostic_level_legacy_off_test)
