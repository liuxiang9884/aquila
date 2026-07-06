#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import json
from pathlib import Path
from typing import Any


def toml_quote(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def load_replayable_files(manifest_path: Path) -> list[Path]:
    files: list[Path] = []
    with manifest_path.open("r", encoding="utf-8") as manifest:
        for line_number, line in enumerate(manifest, start=1):
            stripped = line.strip()
            if not stripped:
                continue
            try:
                entry: dict[str, Any] = json.loads(stripped)
            except json.JSONDecodeError as exc:
                raise ValueError(
                    f"{manifest_path}:{line_number}: invalid JSON: {exc}"
                ) from exc
            raw_file = entry.get("file")
            if not isinstance(raw_file, str) or not raw_file:
                raise ValueError(
                    f"{manifest_path}:{line_number}: file must be a non-empty string"
                )
            file_path = Path(raw_file)
            if file_path.suffix == ".tmp":
                continue
            if not file_path.is_absolute():
                file_path = manifest_path.parent / file_path
            files.append(file_path)
    if not files:
        raise ValueError(f"{manifest_path}: no replayable .bin segments found")
    return files


def render_data_reader_config(
    *,
    manifest_path: Path,
    name: str,
    catalog_path: Path,
    feed: str = "book_ticker",
    max_events_per_drain: int = 4096,
) -> str:
    if not name:
        raise ValueError("name must not be empty")
    if feed not in {"book_ticker", "trade"}:
        raise ValueError("feed must be book_ticker or trade")
    if max_events_per_drain <= 0:
        raise ValueError("max_events_per_drain must be positive")
    files = load_replayable_files(manifest_path)
    file_lines = "\n".join(f"  {toml_quote(str(path))}," for path in files)
    source_name = f"{name}_{feed}"
    return (
        "[instrument_catalog]\n"
        f"file = {toml_quote(str(catalog_path))}\n"
        'schema = "aquila.instrument.v1"\n'
        "\n"
        "[log]\n"
        'log_level = "info"\n'
        f"file_sink_name = {toml_quote(str(manifest_path.parent / (name + '.log')))}\n"
        f"console_sink_name = {toml_quote(name + '_console')}\n"
        f"backend_thread_name = {toml_quote(name + '_log')}\n"
        "backend_cpu_affinity = 5\n"
        'format_pattern = "%(log_level_short_code)%(time) %(process_id):%(thread_id) %(file_name):%(caller_function):%(line_number)] %(message)"\n'
        'timestamp_pattern = "%Y-%m-%d %H:%M:%S.%Qns"\n'
        "\n"
        "[data_reader]\n"
        f"name = {toml_quote(name)}\n"
        f"max_events_per_drain = {max_events_per_drain}\n"
        "\n"
        "[data_reader.execution_policy]\n"
        "bind_cpu_id = -1\n"
        'idle_policy = "spin"\n'
        "\n"
        "[[data_reader.sources]]\n"
        f"name = {toml_quote(source_name)}\n"
        'type = "binary_file"\n'
        f"feed = {toml_quote(feed)}\n"
        "files = [\n"
        f"{file_lines}\n"
        "]\n"
        'start_position = "earliest_visible"\n'
        'read_mode = "drain"\n'
        "required = true\n"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate a binary_file data reader TOML from recorder manifest JSONL."
    )
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--name", default="persistent_md_replay")
    parser.add_argument(
        "--catalog", type=Path, default=Path("config/instruments/usdt_futures.csv")
    )
    parser.add_argument(
        "--feed", choices=["book_ticker", "trade"], default="book_ticker"
    )
    parser.add_argument("--max-events-per-drain", type=int, default=4096)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    config = render_data_reader_config(
        manifest_path=args.manifest,
        name=args.name,
        catalog_path=args.catalog,
        feed=args.feed,
        max_events_per_drain=args.max_events_per_drain,
    )
    if args.output.parent:
        args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(config, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
