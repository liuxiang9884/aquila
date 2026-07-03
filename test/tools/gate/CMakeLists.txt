add_executable(gate_strategy_order_feedback_action_test
    strategy_order_feedback_action_test.cpp
)

target_link_libraries(gate_strategy_order_feedback_action_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME gate_strategy_order_feedback_action_test
         COMMAND gate_strategy_order_feedback_action_test)

add_executable(gate_strategy_order_response_conversion_test
    strategy_order_response_conversion_test.cpp
)

target_link_libraries(gate_strategy_order_response_conversion_test
    PRIVATE
        aquila_core
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_strategy_order_response_conversion_test
         COMMAND gate_strategy_order_response_conversion_test)

add_executable(gate_order_session_failure_probe_test
    order_session_failure_probe_test.cpp
)

target_link_libraries(gate_order_session_failure_probe_test
    PRIVATE
        aquila_core
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_order_session_failure_probe_test
         COMMAND gate_order_session_failure_probe_test)

add_executable(gate_demo_strategy_test
    demo_strategy_test.cpp
)

target_link_libraries(gate_demo_strategy_test
    PRIVATE
        aquila_core
        aquila_config
        GTest::gtest_main
)

add_test(NAME gate_demo_strategy_test
         COMMAND gate_demo_strategy_test)

add_executable(gate_fill_probe_test
    ../../../tools/gate/fill_probe/config.cpp
    fill_probe_test.cpp
)

target_link_libraries(gate_fill_probe_test
    PRIVATE
        aquila_core
        aquila_config
        GTest::gtest_main
        PkgConfig::tomlplusplus
)

add_test(NAME gate_fill_probe_test
         COMMAND gate_fill_probe_test)
set_tests_properties(gate_fill_probe_test
    PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_executable(gate_order_session_rtt_probe_test
    ../../../tools/gate/order_session_rtt_probe/connection_plan.cpp
    ../../../tools/gate/order_session_rtt_probe/config.cpp
    ../../../tools/gate/order_session_rtt_probe/sample_csv_writer.cpp
    order_session_rtt_probe/order_session_rtt_probe_test.cpp
)

target_link_libraries(gate_order_session_rtt_probe_test
    PRIVATE
        aquila_core
        aquila_config
        aquila_gate
        absl::flat_hash_set
        GTest::gtest_main
        nova
        quill::quill
        unofficial::vincentlaucsb-csv-parser::csv
)

add_dependencies(gate_order_session_rtt_probe_test
                 gate_order_session_rtt_probe)

add_test(NAME gate_order_session_rtt_probe_test
         COMMAND gate_order_session_rtt_probe_test)

set(GATE_ORDER_SESSION_RTT_PROBE_CONFIG
    ${PROJECT_SOURCE_DIR}/config/order_session_rtt_probe/gate_order_session_rtt_probe.toml)
set(GATE_ORDER_SESSION_RTT_PROBE_CONNECTIONS
    ${PROJECT_SOURCE_DIR}/config/order_session_rtt_probe/gate_order_session_rtt_connections.csv)

add_test(NAME gate_order_session_rtt_probe_initializes_logging
         COMMAND bash -c [=[
run_dir="/home/liuxiang/tmp/aquila_rtt_probe_logging_ctest_$$"
mkdir -p "$run_dir"
config="$run_dir/probe.toml"
log_file="$run_dir/probe.log"
cat > "$config" <<EOF
[log]
log_level = "info"
file_sink_name = "$log_file"
console_sink_name = ""
backend_thread_name = "rtt_probe_ctest_log"

[probe]
name = "gate_order_session_rtt_probe"
execute = false
run_id = "logging_ctest"

[probe.inputs]
connections_file = "$2"
EOF
output="$("$1" --config "$config" 2>&1)"
code=$?
printf '%s\n' "$output"
shopt -s nullglob
log_files=("$run_dir"/probe*.log)
[[ "$code" -eq 0 ]] &&
  [[ "${#log_files[@]}" -ge 1 ]] &&
  [[ -s "${log_files[0]}" ]] &&
  [[ "$(cat "${log_files[0]}")" == *"gate_order_session_rtt_probe_start"* ]]
]=] _ $<TARGET_FILE:gate_order_session_rtt_probe>
                 ${GATE_ORDER_SESSION_RTT_PROBE_CONNECTIONS})

add_test(NAME gate_order_session_rtt_probe_missing_connections_reject
         COMMAND bash -c [=[
output="$("$1" --config "$2" --connections-file /home/liuxiang/tmp/does_not_exist_for_execute_guard.csv 2>&1)"
code=$?
printf '%s\n' "$output"
[[ "$code" -eq 1 ]] &&
  [[ "$output" == *"connections_csv_error"* ]]
]=] _ $<TARGET_FILE:gate_order_session_rtt_probe>
                 ${GATE_ORDER_SESSION_RTT_PROBE_CONFIG})

add_test(NAME gate_order_session_rtt_probe_samples_zero_reject
         COMMAND bash -c [=[
output="$("$1" --config "$2" --samples-per-session 0 2>&1)"
code=$?
printf '%s\n' "$output"
[[ "$code" -eq 2 ]] &&
  [[ "$output" == *"--samples-per-session must be positive"* ]]
]=] _ $<TARGET_FILE:gate_order_session_rtt_probe>
                 ${GATE_ORDER_SESSION_RTT_PROBE_CONFIG})

add_test(NAME gate_order_session_rtt_probe_preserves_duplicate_connections
         COMMAND bash -c [=[
run_dir="/home/liuxiang/tmp/aquila_rtt_probe_duplicate_connections_ctest_$$"
mkdir -p "$run_dir"
connections="$run_dir/connections.csv"
cat > "$connections" <<EOF
name,group,host,connect_ip,port,enable_tls,worker_cpu_id
private-a,private-10.0.1.154,fxws-private.gateapi.io,10.0.1.154,80,false,6
private-b,private-10.0.1.154,fxws-private.gateapi.io,10.0.1.154,80,false,7
EOF
"$1" --config "$2" --connections-file "$connections"
]=] _ $<TARGET_FILE:gate_order_session_rtt_probe>
                 ${GATE_ORDER_SESSION_RTT_PROBE_CONFIG})
set_tests_properties(gate_order_session_rtt_probe_preserves_duplicate_connections
    PROPERTIES
    PASS_REGULAR_EXPRESSION "connections=2")

add_test(NAME gate_order_gateway_worker_cpu_overrides_session_cpu
         COMMAND bash -c [=[
run_dir="/home/liuxiang/tmp/aquila_order_gateway_cpu_override_ctest_$$"
mkdir -p "$run_dir"
session="$run_dir/order_session.toml"
gateway="$run_dir/order_gateway.toml"
cat > "$session" <<EOF
[order_session]
name = "gate_order_gateway_cpu_override_session"
settle = "usdt"

[order_session.credentials]
api_key_env = "TEST_KEY"
api_secret_env = "TEST_SECRET"

[order_session.websocket.endpoint]
host = "fxws-private.gateapi.io"
connect_ip = "10.0.1.154"
port = "80"
enable_tls = false

[order_session.websocket.execution_policy]
bind_cpu_id = 4
EOF
cat > "$gateway" <<EOF
[log]
log_level = "info"
file_sink_name = "$run_dir/gateway.log"
console_sink_name = "gate_order_gateway_cpu_override_console"
backend_thread_name = "gate_order_gateway_cpu_override_log"

[order_gateway]
name = "gate_order_gateway_cpu_override"
route_count = 2
command_queue_capacity = 64
event_queue_capacity = 64
startup_ready_timeout_s = 30

[[order_gateway.routes]]
name = "route0"
order_session_config = "$session"
worker_cpu_id = 21

[[order_gateway.routes]]
name = "route1"
order_session_config = "$session"
worker_cpu_id = 22
EOF
output="$("$1" --config "$gateway" 2>&1)"
code=$?
printf '%s\n' "$output"
[[ "$code" -eq 0 ]] &&
  [[ "$output" == *"route_id=0"* ]] &&
  [[ "$output" == *"worker_cpu_id=21"* ]] &&
  [[ "$output" == *"order_session_cpu_id=21"* ]] &&
  [[ "$output" == *"route_id=1"* ]] &&
  [[ "$output" == *"worker_cpu_id=22"* ]] &&
  [[ "$output" == *"order_session_cpu_id=22"* ]]
]=] _ $<TARGET_FILE:gate_order_gateway>)
set_tests_properties(gate_order_gateway_worker_cpu_overrides_session_cpu
    PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_test(NAME gate_order_session_rtt_probe_live_preflight
         COMMAND bash -c [=[
run_dir="/home/liuxiang/tmp/aquila_rtt_probe_single_preflight_ctest_$$"
mkdir -p "$run_dir"
connections="$run_dir/connections.csv"
cat > "$connections" <<EOF
name,group,host,connect_ip,port,enable_tls,worker_cpu_id
public-0,public-52.198.250.74,fx-ws.gateio.ws,52.198.250.74,443,true,6
EOF
output="$("$1" --config "$2" --connections-file "$connections" --samples-per-session 2 --live-preflight 2>&1)"
code=$?
printf '%s\n' "$output"
[[ "$code" -eq 0 ]] &&
  [[ "$output" == *"live_preflight=true"* ]] &&
  [[ "$output" == *"session=public-0"* ]] &&
  [[ "$output" == *"connect_ip=52.198.250.74"* ]] &&
  [[ "$output" == *"sample_count=2"* ]] &&
  [[ "$output" == *"order_session_rtt_samples.csv"* ]]
]=] _ $<TARGET_FILE:gate_order_session_rtt_probe>
                 ${GATE_ORDER_SESSION_RTT_PROBE_CONFIG})
set_tests_properties(gate_order_session_rtt_probe_live_preflight
    PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_test(NAME gate_order_session_rtt_probe_multi_live_preflight
         COMMAND bash -c [=[
run_dir="/home/liuxiang/tmp/aquila_rtt_probe_multi_preflight_ctest_$$"
mkdir -p "$run_dir"
connections="$run_dir/connections.csv"
cat > "$connections" <<EOF
name,group,host,connect_ip,port,enable_tls,worker_cpu_id
public-0,public,fx-ws.gateio.ws,52.198.250.74,443,true,6
public-1,public,fx-ws.gateio.ws,52.199.212.24,443,true,7
public-2,public,fx-ws.gateio.ws,57.181.9.46,443,true,8
public-3,public,fx-ws.gateio.ws,18.180.25.199,443,true,9
public-4,public,fx-ws.gateio.ws,3.112.197.248,443,true,10
public-5,public,fx-ws.gateio.ws,13.115.76.214,443,true,11
public-6,public,fx-ws.gateio.ws,35.73.90.116,443,true,12
public-7,public,fx-ws.gateio.ws,54.95.186.57,443,true,13
EOF
output="$("$1" --config "$2" --connections-file "$connections" --samples-per-session 2 --live-preflight 2>&1)"
code=$?
printf '%s\n' "$output"
[[ "$code" -eq 0 ]] &&
  [[ "$output" == *"live_preflight=true"* ]] &&
  [[ "$output" == *"live_multi_session=true"* ]] &&
  [[ "$output" == *"session_count=8"* ]] &&
  [[ "$output" == *"total_sample_count=16"* ]] &&
  [[ "$output" == *"session=public-0"* ]] &&
  [[ "$output" == *"connect_ip=52.198.250.74"* ]] &&
  [[ "$output" == *"connect_ip=54.95.186.57"* ]]
]=] _ $<TARGET_FILE:gate_order_session_rtt_probe>
                 ${GATE_ORDER_SESSION_RTT_PROBE_CONFIG})
set_tests_properties(gate_order_session_rtt_probe_multi_live_preflight
    PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_executable(gate_data_fusion_config_test
    gate_data_fusion_config_test.cpp
    ${PROJECT_SOURCE_DIR}/tools/gate/gate_data_fusion_config.cpp
)

target_link_libraries(gate_data_fusion_config_test
    PRIVATE
        aquila_core
        GTest::gtest_main
        PkgConfig::tomlplusplus
)

add_test(NAME gate_data_fusion_config_test
         COMMAND gate_data_fusion_config_test)
