add_library(aquila_strategy STATIC
    lead_lag/alignment.h
    lead_lag/config.cpp
    lead_lag/config.h
    lead_lag/cost_model.h
    lead_lag/execution_state.h
    lead_lag/recorders.h
    lead_lag/raw_market_state.h
    lead_lag/shadow_evaluation.h
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
if(AQUILA_ENABLE_LEAD_LAG_MARKET_CALC_CSV)
    target_compile_definitions(aquila_strategy
        PUBLIC
            AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV
    )
endif()

set(AQUILA_LEAD_LAG_DIAGNOSTIC_SOURCES
    lead_lag/signal_csv_writer.cpp
)
if(AQUILA_ENABLE_LEAD_LAG_MARKET_CALC_CSV)
    list(APPEND AQUILA_LEAD_LAG_DIAGNOSTIC_SOURCES
        lead_lag/market_calc_csv_writer.cpp
    )
endif()

add_library(aquila_lead_lag_diagnostics STATIC
    ${AQUILA_LEAD_LAG_DIAGNOSTIC_SOURCES}
)

target_include_directories(aquila_lead_lag_diagnostics
    PUBLIC
        ${PROJECT_SOURCE_DIR}
)

target_link_libraries(aquila_lead_lag_diagnostics
    PUBLIC
        aquila_strategy
        magic_enum::magic_enum
        nova
        quill::quill
)
