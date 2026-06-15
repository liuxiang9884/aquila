add_subdirectory(gate)
add_subdirectory(binance)
add_subdirectory(benchmark)
add_subdirectory(tardis)
add_subdirectory(lead_lag)
add_subdirectory(market_data)

add_test(NAME data_fusion_tools_static_review
         COMMAND bash -c [=[
if rg -n 'fmt::print|start_result_' "$1" "$2"; then
  exit 1
fi
]=] _
         ${PROJECT_SOURCE_DIR}/tools/gate/gate_data_fusion.cpp
         ${PROJECT_SOURCE_DIR}/tools/binance/binance_data_fusion.cpp)
