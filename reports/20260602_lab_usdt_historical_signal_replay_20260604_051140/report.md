# LAB_USDT 20260602 Signal Replay Summary

- report_dir: `reports/20260602_lab_usdt_historical_signal_replay_20260604_051140`
- binary: `/home/liuxiang/tardis/merged_book_ticker/LAB_USDT/20260602.bin`
- data_reader_config: `/home/liuxiang/tmp/aquila_lab_usdt_binary_replay_20260602.toml`
- replay_config: `reports/20260602_lab_usdt_historical_signal_replay_20260604_051140/lead_lag_lab_usdt_replay.toml`
- signal_csv: `reports/20260602_lab_usdt_historical_signal_replay_20260604_051140/signal.csv`
- input_book_tickers: `50085299`
- signals: `3098`
- open: `1549`
- close: `1504`
- stoploss: `45`
- first_signal_exchange_ns: `1780358516042000000` (2026-06-02T00:01:56.042000Z)
- last_signal_exchange_ns: `1780444642589000000` (2026-06-02T23:57:22.589000Z)
- exchange_ns_monotonic: `true`
- exchange_ns_order_violations: `0`

## By Action

| action | count |
| --- | ---: |
| `kCloseLong` | 744 |
| `kCloseShort` | 760 |
| `kOpenLong` | 770 |
| `kOpenShort` | 779 |
| `kStoplossLong` | 26 |
| `kStoplossShort` | 19 |

## By Exchange / Role / Side

- exchange: `kBinance`=2496, `kGate`=602
- role: `kLag`=602, `kLead`=2496
- side: `kBuy`=1549, `kSell`=1549
- reduce_only: `false`=1549, `true`=1549

## From 2026-06-02 16:00:00 UTC

- csv: `reports/20260602_lab_usdt_historical_signal_replay_20260604_051140/signal_from_20260602_160000_utc.csv`
- threshold_exchange_ns: `1780416000000000000`
- signals: `1458`
- open: `729`
- `kOpenLong`: `354`
- `kOpenShort`: `375`
- first_kept_exchange_ns: `1780416000098000000`
- last_kept_exchange_ns: `1780444642589000000`

## Generated Files

- `reports/20260602_lab_usdt_historical_signal_replay_20260604_051140/report.md`
- `reports/20260602_lab_usdt_historical_signal_replay_20260604_051140/signal.csv`
- `reports/20260602_lab_usdt_historical_signal_replay_20260604_051140/signal_from_20260602_160000_utc.csv`
- `reports/20260602_lab_usdt_historical_signal_replay_20260604_051140/signal_by_action.csv`
- `reports/20260602_lab_usdt_historical_signal_replay_20260604_051140/signal_by_hour.csv`
- `reports/20260602_lab_usdt_historical_signal_replay_20260604_051140/signal_first_last.csv`
- `reports/20260602_lab_usdt_historical_signal_replay_20260604_051140/lead_lag_lab_usdt_replay.toml`
- `reports/20260602_lab_usdt_historical_signal_replay_20260604_051140/lead_lag_replay.stdout.log`
- `reports/20260602_lab_usdt_historical_signal_replay_20260604_051140/lead_lag_replay.stderr.log`
