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

add_executable(gate_order_session_rtt_probe_test
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
)

add_dependencies(gate_order_session_rtt_probe_test
                 gate_order_session_rtt_probe)

add_test(NAME gate_order_session_rtt_probe_test
         COMMAND gate_order_session_rtt_probe_test)

set(GATE_ORDER_SESSION_RTT_PROBE_CONFIG
    ${PROJECT_SOURCE_DIR}/config/order_session_rtt_probe/gate_order_session_rtt_probe.toml)
set(GATE_ORDER_SESSION_RTT_PROBE_CANDIDATES
    ${CMAKE_CURRENT_SOURCE_DIR}/order_session_rtt_probe/candidate_ips.txt)

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
candidate_ip_file = "$2"
EOF
output="$("$1" --config "$config" --max-candidates 1 2>&1)"
code=$?
printf '%s\n' "$output"
shopt -s nullglob
log_files=("$run_dir"/probe*.log)
[[ "$code" -eq 0 ]] &&
  [[ "${#log_files[@]}" -ge 1 ]] &&
  [[ -s "${log_files[0]}" ]] &&
  [[ "$(cat "${log_files[0]}")" == *"gate_order_session_rtt_probe_start"* ]]
]=] _ $<TARGET_FILE:gate_order_session_rtt_probe>
                 ${GATE_ORDER_SESSION_RTT_PROBE_CANDIDATES})

add_test(NAME gate_order_session_rtt_probe_execute_missing_candidate_reject
         COMMAND bash -c [=[
output="$("$1" --config "$2" --execute --candidate-ip-file /home/liuxiang/tmp/does_not_exist_for_execute_guard.txt 2>&1)"
code=$?
printf '%s\n' "$output"
[[ "$code" -eq 1 ]] &&
  [[ "$output" == *"candidate_ip_error"* ]]
]=] _ $<TARGET_FILE:gate_order_session_rtt_probe>
                 ${GATE_ORDER_SESSION_RTT_PROBE_CONFIG})

add_test(NAME gate_order_session_rtt_probe_samples_zero_reject
         COMMAND bash -c [=[
output="$("$1" --config "$2" --samples-per-ip 0 2>&1)"
code=$?
printf '%s\n' "$output"
[[ "$code" -eq 2 ]] &&
  [[ "$output" == *"--samples-per-ip must be positive"* ]]
]=] _ $<TARGET_FILE:gate_order_session_rtt_probe>
                 ${GATE_ORDER_SESSION_RTT_PROBE_CONFIG})

add_test(NAME gate_order_session_rtt_probe_active_zero_reject
         COMMAND bash -c [=[
output="$("$1" --config "$2" --active-session-count 0 2>&1)"
code=$?
printf '%s\n' "$output"
[[ "$code" -eq 2 ]] &&
  [[ "$output" == *"--active-session-count must be positive"* ]]
]=] _ $<TARGET_FILE:gate_order_session_rtt_probe>
                 ${GATE_ORDER_SESSION_RTT_PROBE_CONFIG})

add_test(NAME gate_order_session_rtt_probe_max_candidates_zero_unlimited
         COMMAND $<TARGET_FILE:gate_order_session_rtt_probe>
                 --config ${GATE_ORDER_SESSION_RTT_PROBE_CONFIG}
                 --candidate-ip-file ${GATE_ORDER_SESSION_RTT_PROBE_CANDIDATES}
                 --max-candidates 0)
set_tests_properties(gate_order_session_rtt_probe_max_candidates_zero_unlimited
    PROPERTIES
    PASS_REGULAR_EXPRESSION "candidate_ips=2")

add_test(NAME gate_order_session_rtt_probe_live_preflight
         COMMAND bash -c [=[
output="$("$1" --config "$2" --candidate-ip-file "$3" --max-candidates 1 --samples-per-ip 2 --live-preflight 2>&1)"
code=$?
printf '%s\n' "$output"
[[ "$code" -eq 0 ]] &&
  [[ "$output" == *"live_preflight=true"* ]] &&
  [[ "$output" == *"connect_ip=52.198.250.74"* ]] &&
  [[ "$output" == *"sample_count=2"* ]] &&
  [[ "$output" == *"order_session_rtt_samples.csv"* ]]
]=] _ $<TARGET_FILE:gate_order_session_rtt_probe>
                 ${GATE_ORDER_SESSION_RTT_PROBE_CONFIG}
                 ${GATE_ORDER_SESSION_RTT_PROBE_CANDIDATES})
set_tests_properties(gate_order_session_rtt_probe_live_preflight
    PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_test(NAME gate_order_session_rtt_probe_multi_live_preflight
         COMMAND bash -c [=[
run_dir="/home/liuxiang/tmp/aquila_rtt_probe_multi_preflight_ctest_$$"
mkdir -p "$run_dir"
candidates="$run_dir/candidate_ips.txt"
cat > "$candidates" <<EOF
52.198.250.74
52.199.212.24
57.181.9.46
18.180.25.199
3.112.197.248
13.115.76.214
35.73.90.116
54.95.186.57
EOF
output="$("$1" --config "$2" --candidate-ip-file "$candidates" --max-candidates 8 --active-session-count 8 --samples-per-ip 2 --live-preflight 2>&1)"
code=$?
printf '%s\n' "$output"
[[ "$code" -eq 0 ]] &&
  [[ "$output" == *"live_preflight=true"* ]] &&
  [[ "$output" == *"live_multi_session=true"* ]] &&
  [[ "$output" == *"session_count=8"* ]] &&
  [[ "$output" == *"total_sample_count=16"* ]] &&
  [[ "$output" == *"connect_ip=52.198.250.74"* ]] &&
  [[ "$output" == *"connect_ip=54.95.186.57"* ]]
]=] _ $<TARGET_FILE:gate_order_session_rtt_probe>
                 ${GATE_ORDER_SESSION_RTT_PROBE_CONFIG})
set_tests_properties(gate_order_session_rtt_probe_multi_live_preflight
    PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
