add_executable(monitor_symbol_workbench_demo_data_test
    symbol_workbench_demo_data_test.cpp
)

target_link_libraries(monitor_symbol_workbench_demo_data_test
    PRIVATE
        aquila_monitor
        GTest::gtest_main
)

add_test(NAME monitor_symbol_workbench_demo_data_test
         COMMAND monitor_symbol_workbench_demo_data_test)
