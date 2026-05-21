if(NOT DEFINED LEAD_LAG_STRATEGY)
  message(FATAL_ERROR "LEAD_LAG_STRATEGY is required")
endif()

if(NOT DEFINED LEAD_LAG_CONFIG)
  message(FATAL_ERROR "LEAD_LAG_CONFIG is required")
endif()

execute_process(
  COMMAND "${LEAD_LAG_STRATEGY}" --config "${LEAD_LAG_CONFIG}" --execute --duration-sec 1
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)

set(combined_output "${stdout}\n${stderr}")
set(expected_error
    "lead_lag live order mode remains disabled until REST reconcile, feedback recovery, and live smoke guardrails are complete")

if(NOT result EQUAL 3)
  message(FATAL_ERROR
          "expected lead_lag_strategy --execute to exit 3, got ${result}\n${combined_output}")
endif()

if(NOT combined_output MATCHES "${expected_error}")
  message(FATAL_ERROR
          "expected disabled live order error in output\nexpected: ${expected_error}\nactual:\n${combined_output}")
endif()

if(combined_output MATCHES "missing env var|runtime_create_error|lead_lag_strategy_signal_only_summary")
  message(FATAL_ERROR
          "live order disabled smoke reached an unexpected runtime path\n${combined_output}")
endif()
