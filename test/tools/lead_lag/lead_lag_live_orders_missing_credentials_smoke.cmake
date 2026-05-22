if(NOT DEFINED LEAD_LAG_STRATEGY)
  message(FATAL_ERROR "LEAD_LAG_STRATEGY is required")
endif()

if(NOT DEFINED LEAD_LAG_CONFIG)
  message(FATAL_ERROR "LEAD_LAG_CONFIG is required")
endif()

set(missing_key_env "AQUILA_TEST_MISSING_LEAD_LAG_LIVE_KEY")
set(missing_secret_env "AQUILA_TEST_MISSING_LEAD_LAG_LIVE_SECRET")

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

if(NOT result EQUAL 2)
  message(FATAL_ERROR
          "expected lead_lag_strategy --execute without credentials to exit 2, "
          "got ${result}\n${combined_output}")
endif()

if(NOT combined_output MATCHES "missing env var ${missing_key_env}")
  message(FATAL_ERROR
          "expected missing credential error in output\nactual:\n${combined_output}")
endif()

if(NOT combined_output MATCHES "lead_lag_strategy run_mode=live_orders")
  message(FATAL_ERROR
          "expected live_orders run mode before credential check\n${combined_output}")
endif()

if(combined_output MATCHES
   "runtime_create_error|lead_lag_strategy_signal_only_summary|lead_lag_strategy_live_orders_summary")
  message(FATAL_ERROR
          "live order missing-credentials smoke reached an unexpected "
          "runtime path\n${combined_output}")
endif()
