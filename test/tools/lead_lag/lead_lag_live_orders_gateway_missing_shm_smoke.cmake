if(NOT DEFINED LEAD_LAG_STRATEGY)
  message(FATAL_ERROR "LEAD_LAG_STRATEGY is required")
endif()

if(NOT DEFINED LEAD_LAG_CONFIG)
  message(FATAL_ERROR "LEAD_LAG_CONFIG is required")
endif()

set(missing_key_env "AQUILA_TEST_MISSING_LEAD_LAG_GATEWAY_KEY")
set(missing_secret_env "AQUILA_TEST_MISSING_LEAD_LAG_GATEWAY_SECRET")

execute_process(
  COMMAND "${LEAD_LAG_STRATEGY}"
          --config "${LEAD_LAG_CONFIG}"
          --execute
          --duration-sec 1
          --api-key "${missing_key_env}"
          --api-secret "${missing_secret_env}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)

set(combined_output "${stdout}\n${stderr}")

if(NOT result EQUAL 1)
  message(FATAL_ERROR
          "expected lead_lag_strategy --execute with missing order gateway SHM "
          "to exit 1, got ${result}\n${combined_output}")
endif()

if(NOT combined_output MATCHES "lead_lag_strategy run_mode=live_orders")
  message(FATAL_ERROR
          "expected live_orders run mode before order gateway startup\n"
          "${combined_output}")
endif()

if(NOT combined_output MATCHES "order_backend=order_gateway")
  message(FATAL_ERROR
          "expected order_gateway backend in loaded config summary\n"
          "${combined_output}")
endif()

if(NOT combined_output MATCHES "runtime_create_error")
  message(FATAL_ERROR
          "expected runtime_create_error from OrderGatewayClient attach path\n"
          "${combined_output}")
endif()

if(NOT combined_output MATCHES "order_gateway_shm.shm_open")
  message(FATAL_ERROR
          "expected OrderGatewayClient missing-SHM error, not an earlier "
          "runtime dependency failure\n${combined_output}")
endif()

if(combined_output MATCHES "missing env var ${missing_key_env}")
  message(FATAL_ERROR
          "order gateway live-orders path unexpectedly requested Gate "
          "credentials\n${combined_output}")
endif()
