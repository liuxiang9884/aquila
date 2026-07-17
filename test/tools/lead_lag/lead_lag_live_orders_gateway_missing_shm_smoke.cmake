if(NOT DEFINED LEAD_LAG_STRATEGY)
  message(FATAL_ERROR "LEAD_LAG_STRATEGY is required")
endif()

if(NOT DEFINED LEAD_LAG_CONFIG)
  message(FATAL_ERROR "LEAD_LAG_CONFIG is required")
endif()

if(NOT DEFINED GATE_DATA_SESSION)
  message(FATAL_ERROR "GATE_DATA_SESSION is required")
endif()

if(NOT DEFINED BINANCE_DATA_SESSION)
  message(FATAL_ERROR "BINANCE_DATA_SESSION is required")
endif()

set(missing_key_env "AQUILA_TEST_MISSING_LEAD_LAG_GATEWAY_KEY")
set(missing_secret_env "AQUILA_TEST_MISSING_LEAD_LAG_GATEWAY_SECRET")
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef suffix)
set(temp_dir "/home/liuxiang/tmp/aquila_lead_lag_gateway_missing_shm_${suffix}")
file(MAKE_DIRECTORY "${temp_dir}")

set(gate_shm "aquila_ll_missing_gateway_gate_${suffix}")
set(binance_shm "aquila_ll_missing_gateway_binance_${suffix}")
set(gate_config "${temp_dir}/gate_data_session.toml")
set(binance_config "${temp_dir}/binance_data_session.toml")
set(data_reader_config "${temp_dir}/strategy_data_reader.toml")
set(strategy_config "${temp_dir}/lead_lag_live_orders_gateway_strategy.toml")

file(WRITE "${gate_config}" [=[
[instrument_catalog]
file = "config/instruments/usdt_future_universe.csv"
schema = "aquila.instrument.v1"

[log]
log_level = "error"
file_sink_name = "/home/liuxiang/tmp/aquila_lead_lag_gateway_missing_shm_gate.log"
console_sink_name = "lead_lag_gateway_missing_shm_gate_console"
backend_thread_name = "lead_lag_gateway_missing_shm_gate_log"
backend_cpu_affinity = -1
format_pattern = "%(log_level_short_code)%(time) %(process_id):%(thread_id) %(file_name):%(caller_function):%(line_number)] %(message)"
timestamp_pattern = "%Y-%m-%d %H:%M:%S.%Qns"

[data_session]
name = "lead_lag_gateway_missing_shm_gate"
subscribe_symbols = ["BTC_USDT"]
settle = "usdt"
wire_format = "sbe"
sbe_schema_id = 1
feeds = ["book_ticker"]

[data_session.websocket.endpoint]
host = "fx-ws.gateio.ws"
port = "443"
enable_tls = true

[data_session.websocket.execution_policy]
bind_cpu_id = -1

[data_shm_sink]
enabled = true
shm_name = "]=])
file(APPEND "${gate_config}" "${gate_shm}")
file(APPEND "${gate_config}" [=["
book_ticker_channel_name = "book_ticker_channel"
trade_channel_name = "trade_channel"
create = true
remove_existing = true
]=])

file(WRITE "${binance_config}" [=[
[instrument_catalog]
file = "config/instruments/usdt_future_universe.csv"
schema = "aquila.instrument.v1"

[log]
log_level = "error"
file_sink_name = "/home/liuxiang/tmp/aquila_lead_lag_gateway_missing_shm_binance.log"
console_sink_name = "lead_lag_gateway_missing_shm_binance_console"
backend_thread_name = "lead_lag_gateway_missing_shm_binance_log"
backend_cpu_affinity = -1
format_pattern = "%(log_level_short_code)%(time) %(process_id):%(thread_id) %(file_name):%(caller_function):%(line_number)] %(message)"
timestamp_pattern = "%Y-%m-%d %H:%M:%S.%Qns"

[data_session]
name = "lead_lag_gateway_missing_shm_binance"
subscribe_symbols = ["BTC_USDT"]
market = "um_futures"
feeds = ["book_ticker"]

[data_session.websocket.endpoint]
host = "fstream.binance.com"
port = "443"
enable_tls = true

[data_session.websocket.execution_policy]
bind_cpu_id = -1

[data_shm_sink]
enabled = true
shm_name = "]=])
file(APPEND "${binance_config}" "${binance_shm}")
file(APPEND "${binance_config}" [=["
book_ticker_channel_name = "book_ticker_channel"
trade_channel_name = "trade_channel"
create = true
remove_existing = true
]=])

file(WRITE "${data_reader_config}" [=[
[instrument_catalog]
file = "config/instruments/usdt_future_universe.csv"
schema = "aquila.instrument.v1"

[log]
log_level = "error"
file_sink_name = "/home/liuxiang/tmp/aquila_lead_lag_gateway_missing_shm_reader.log"
console_sink_name = "lead_lag_gateway_missing_shm_reader_console"
backend_thread_name = "lead_lag_gateway_missing_shm_reader_log"
backend_cpu_affinity = -1
format_pattern = "%(log_level_short_code)%(time) %(process_id):%(thread_id) %(file_name):%(caller_function):%(line_number)] %(message)"
timestamp_pattern = "%Y-%m-%d %H:%M:%S.%Qns"

[data_reader]
name = "lead_lag_gateway_missing_shm_reader"
max_events_per_drain = 64

[data_reader.execution_policy]
bind_cpu_id = -1
idle_policy = "spin"

[[data_reader.sources]]
name = "gate_book_ticker"
type = "shm"
exchange = "gate"
feed = "book_ticker"
shm_name = "]=])
file(APPEND "${data_reader_config}" "${gate_shm}")
file(APPEND "${data_reader_config}" [=["
channel_name = "book_ticker_channel"
start_position = "latest"
read_mode = "latest"
required = true

[[data_reader.sources]]
name = "binance_book_ticker"
type = "shm"
exchange = "binance"
feed = "book_ticker"
shm_name = "]=])
file(APPEND "${data_reader_config}" "${binance_shm}")
file(APPEND "${data_reader_config}" [=["
channel_name = "book_ticker_channel"
start_position = "latest"
read_mode = "latest"
required = true
]=])

file(WRITE "${strategy_config}" [=[
[log]
log_level = "info"
file_sink_name = "/home/liuxiang/tmp/aquila_lead_lag_live_orders_gateway_strategy.log"
console_sink_name = "lead_lag_live_orders_gateway_strategy_console"
backend_thread_name = "lead_lag_live_orders_gateway_strategy_log"
backend_cpu_affinity = -1
format_pattern = "%(log_level_short_code)%(time) %(process_id):%(thread_id) %(file_name):%(caller_function):%(line_number)] %(message)"
timestamp_pattern = "%Y-%m-%d %H:%M:%S.%Qns"

[strategy]
name = "lead_lag"
strategy_id = 4
mode = "live"
order_capacity = 8
config = "config/strategies/lead_lag.toml"

[strategy.loop]
idle_policy = "spin"
bind_cpu_id = -1
max_loop_seconds = 0

[strategy.data_reader]
config = "]=])
file(APPEND "${strategy_config}" "${data_reader_config}")
file(APPEND "${strategy_config}" [=["

[strategy.order_gateway]
config = "test/tools/lead_lag/lead_lag_order_gateway_missing_shm.toml"

[strategy.feedback]
enabled = true
shm_name = "aquila_gate_order_feedback"
channel_name = "orders"
poll_budget = 32
force_claim = false
]=])

execute_process(
  COMMAND "${GATE_DATA_SESSION}" --config "${gate_config}"
  RESULT_VARIABLE gate_result
  OUTPUT_VARIABLE gate_stdout
  ERROR_VARIABLE gate_stderr
)

if(NOT gate_result EQUAL 0)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -f
                  "/dev/shm/${gate_shm}" "/dev/shm/${binance_shm}")
  file(REMOVE_RECURSE "${temp_dir}")
  message(FATAL_ERROR
          "failed to create Gate market-data SHM fixture\n"
          "${gate_stdout}\n${gate_stderr}")
endif()

execute_process(
  COMMAND "${BINANCE_DATA_SESSION}" --config "${binance_config}"
  RESULT_VARIABLE binance_result
  OUTPUT_VARIABLE binance_stdout
  ERROR_VARIABLE binance_stderr
)

if(NOT binance_result EQUAL 0)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -f
                  "/dev/shm/${gate_shm}" "/dev/shm/${binance_shm}")
  file(REMOVE_RECURSE "${temp_dir}")
  message(FATAL_ERROR
          "failed to create Binance market-data SHM fixture\n"
          "${binance_stdout}\n${binance_stderr}")
endif()

execute_process(
  COMMAND "${LEAD_LAG_STRATEGY}"
          --config "${strategy_config}"
          --execute
          --duration-sec 1
          --api-key "${missing_key_env}"
          --api-secret "${missing_secret_env}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)

set(combined_output "${stdout}\n${stderr}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -f
                "/dev/shm/${gate_shm}" "/dev/shm/${binance_shm}")
file(REMOVE_RECURSE "${temp_dir}")

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
