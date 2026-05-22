add_library(aquila_monitor STATIC
    demo/symbol_workbench_demo_data.cpp
)

target_include_directories(aquila_monitor
    PUBLIC
        ${PROJECT_SOURCE_DIR}
)

target_link_libraries(aquila_monitor
    PUBLIC
        aquila_core
)

add_executable(gate_account_tui
    tui/gate_account_tui.cpp
)

target_link_libraries(gate_account_tui
    PRIVATE
        aquila_monitor
        CLI11::CLI11
        fmt::fmt-header-only
        ftxui::component
        ftxui::dom
        ftxui::screen
)
