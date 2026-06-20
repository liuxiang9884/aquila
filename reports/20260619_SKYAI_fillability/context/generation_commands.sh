#!/usr/bin/env bash
set -euo pipefail

# This file records the commands/inputs used to create this archive.
# It is intended as documentation; rerun from the repository root after the
# source run directory and symbol split files are available.

RUN_ID=20260619_095317_28symbols_no_h_30d_fusion_off_l0_live
SYMBOL=SKYAI_USDT
ROOT=reports/20260619_SKYAI_fillability
RUN_DIR=/home/liuxiang/tmp/${RUN_ID}
REPORT_DIR=${RUN_DIR}/status_reports_20260620_012105/${RUN_ID}
SPLIT_ROOT=/home/liuxiang/tmp/book_ticker_symbol_splits
PY=/home/liuxiang/dev/pyenv/lx/bin/python

mkdir -p \
  ${ROOT}/inputs \
  ${ROOT}/logs \
  ${ROOT}/market_data/split_summaries \
  ${ROOT}/analysis \
  ${ROOT}/context/configs

# Filter inputs/logs with the Python extraction script used in this session.
# The exact retained counts are documented in logs/extraction_notes.md and manifest.json.

# Compress symbol-level BookTicker splits. Do not copy 28-symbol raw bins.
zstd -q -f "${SPLIT_ROOT}/${RUN_ID}/${SYMBOL}.bin" -o "${ROOT}/market_data/canonical.bin.zst"
zstd -q -f "${SPLIT_ROOT}/${RUN_ID}_source0/${SYMBOL}.bin" -o "${ROOT}/market_data/source0.bin.zst"
zstd -q -f "${SPLIT_ROOT}/${RUN_ID}_source1/${SYMBOL}.bin" -o "${ROOT}/market_data/source1.bin.zst"
zstd -q -f "${SPLIT_ROOT}/${RUN_ID}_source2/${SYMBOL}.bin" -o "${ROOT}/market_data/source2.bin.zst"
zstd -q -f "${SPLIT_ROOT}/${RUN_ID}_source3/${SYMBOL}.bin" -o "${ROOT}/market_data/source3.bin.zst"

# Copy analysis scratch outputs with short names, then derive candidate tables.
# Candidate filters:
# - ack_full_candidates.csv: after_ack_to_cancel_full_records > 0
# - cancel_point_candidates.csv: cancel_point_any=true or cancel_point_full=true

# Verify compressed market data and checksums.
zstd -t ${ROOT}/market_data/*.zst
sha256sum -c ${ROOT}/checksums.sha256
