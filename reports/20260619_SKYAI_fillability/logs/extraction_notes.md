# Log Extraction Notes

- Run id: `20260619_095317_28symbols_no_h_30d_fusion_off_l0_live`
- Symbol: `SKYAI_USDT`
- Strategy log source: `/home/liuxiang/tmp/20260619_095317_28symbols_no_h_30d_fusion_off_l0_live/logs/lead_lag_strategy_28symbols_no_h_fusion_live_20260619_095558.log`
- Feedback log source: `/home/liuxiang/tmp/20260619_095317_28symbols_no_h_30d_fusion_off_l0_live/logs/gate_order_feedback_session_28symbols_no_h_no_ton_private_plain_20260619_095457.log`
- Filter rule: retain lines containing `SKYAI_USDT`, any SKYAI `local_order_id`, any non-zero `exchange_order_id`, or any `text_order_id` from `inputs/orders.csv`.
- `request_sequence` values are listed in `inputs/orders.csv`; logs were not filtered by bare sequence number to avoid false matches.

## Extracted Identifiers

- Local order ids: 113
- Non-zero exchange order ids: 113
- Text order ids: 113
- Strategy log lines retained: 1382
- Feedback log lines retained: 113

## Sample Counts

- SKYAI orders: 113
- SKYAI signals: 237
- SKYAI positions: 9
- SKYAI latency rows: 113
- Entry cancelled no-fill IOC orders: 93
- Filled or partial control orders: 17
- Cancelled no-fill orders with non-zero `accepted_lag_id`: 67
- Cancelled no-fill orders without `accepted_lag_id`: 26
