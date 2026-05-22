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

add_executable(monitor_market_data_view_model_test
    market_data_view_model_test.cpp
)

target_link_libraries(monitor_market_data_view_model_test
    PRIVATE
        aquila_monitor
        GTest::gtest_main
)

add_test(NAME monitor_market_data_view_model_test
         COMMAND monitor_market_data_view_model_test)

add_executable(monitor_market_data_store_test
    market_data_store_test.cpp
)

target_link_libraries(monitor_market_data_store_test
    PRIVATE
        aquila_monitor
        GTest::gtest_main
)

add_test(NAME monitor_market_data_store_test
         COMMAND monitor_market_data_store_test)

add_executable(monitor_spsc_queue_test
    monitor_spsc_queue_test.cpp
)

target_link_libraries(monitor_spsc_queue_test
    PRIVATE
        aquila_monitor
        GTest::gtest_main
)

add_test(NAME monitor_spsc_queue_test
         COMMAND monitor_spsc_queue_test)

add_executable(monitor_market_data_thread_test
    market_data_thread_test.cpp
)

target_compile_definitions(monitor_market_data_thread_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

target_link_libraries(monitor_market_data_thread_test
    PRIVATE
        aquila_monitor
        nova
        GTest::gtest_main
)

add_test(NAME monitor_market_data_thread_test
         COMMAND monitor_market_data_thread_test)

add_test(NAME monitor_gate_account_tui_help_test
         COMMAND gate_account_tui --help)

set_tests_properties(monitor_gate_account_tui_help_test
    PROPERTIES
        PASS_REGULAR_EXPRESSION "Aquila Gate account TUI monitor"
)

add_test(NAME monitor_gate_account_tui_live_dump_missing_config_smoke
         COMMAND gate_account_tui --dump --live-market-data
                 --market-data-config config/monitors/missing_gate_account_tui_market_data.toml
                 --width 260 --height 60)

set_tests_properties(monitor_gate_account_tui_live_dump_missing_config_smoke
    PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        PASS_REGULAR_EXPRESSION "market data unavailable"
)
