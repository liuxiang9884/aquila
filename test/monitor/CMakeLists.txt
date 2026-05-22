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

add_executable(monitor_tui_quit_events_test
    tui_quit_events_test.cpp
)

target_link_libraries(monitor_tui_quit_events_test
    PRIVATE
        aquila_monitor
        ftxui::component
        GTest::gtest_main
)

add_test(NAME monitor_tui_quit_events_test
         COMMAND monitor_tui_quit_events_test)

add_executable(monitor_symbol_workbench_view_test
    symbol_workbench_view_test.cpp
)

target_link_libraries(monitor_symbol_workbench_view_test
    PRIVATE
        aquila_monitor
        ftxui::dom
        ftxui::screen
        GTest::gtest_main
)

add_test(NAME monitor_symbol_workbench_view_test
         COMMAND monitor_symbol_workbench_view_test)
