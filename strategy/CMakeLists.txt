add_library(aquila_strategy STATIC
    lead_lag/alignment.h
    lead_lag/config.cpp
    lead_lag/config.h
    lead_lag/cost_model.h
    lead_lag/execution_state.h
    lead_lag/recorders.h
    lead_lag/raw_market_state.h
    lead_lag/signal.h
    lead_lag/strategy.h
    lead_lag/threshold.h
    lead_lag/types.h
    lead_lag/window_stats.h
)

target_include_directories(aquila_strategy
    PUBLIC
        ${PROJECT_SOURCE_DIR}
)

target_link_libraries(aquila_strategy
    PUBLIC
        aquila_core
        aquila_config
        PkgConfig::tomlplusplus
        fmt::fmt-header-only
        magic_enum::magic_enum
        nova
)
