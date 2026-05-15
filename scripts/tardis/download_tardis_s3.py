#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import shlex
import shutil
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta
from pathlib import Path


DEFAULT_BUCKET = "tko-s3-tardis-share"
DEFAULT_PREFIX = "tardis"
DEFAULT_DOWNLOAD_DIR = Path.home()
DATE_FORMAT = "%Y%m%d"


@dataclass(frozen=True)
class DownloadCommand:
    source: str
    destination: Path
    command: list[str]


def parse_date(value: str) -> datetime:
    try:
        return datetime.strptime(value, DATE_FORMAT)
    except ValueError as exc:
        raise ValueError(f"invalid date {value!r}, expected YYYYMMDD") from exc


def expand_date_range(start_date: str, end_date: str) -> list[str]:
    start = parse_date(start_date)
    end = parse_date(end_date)
    if start > end:
        raise ValueError("start-date must be less than or equal to end-date")

    dates = []
    current = start
    while current <= end:
        dates.append(current.strftime(DATE_FORMAT))
        current += timedelta(days=1)
    return dates


def normalize_path_part(value: str, name: str) -> str:
    normalized = value.strip().strip("/")
    if not normalized:
        raise ValueError(f"{name} must not be empty")
    if ".." in normalized.split("/"):
        raise ValueError(f"{name} must not contain '..'")
    return normalized


def normalize_symbol(symbol: str) -> str:
    return symbol.strip().upper()


def aws_base_command(aws_profile: str | None) -> list[str]:
    command = ["aws"]
    if aws_profile:
        command.extend(["--profile", aws_profile])
    command.extend(["s3"])
    return command


def s3_prefix(bucket: str, prefix: str, exchange: str, data_type: str, date: str) -> str:
    prefix = normalize_path_part(prefix, "prefix")
    exchange = normalize_path_part(exchange, "exchange")
    data_type = normalize_path_part(data_type, "data-type")
    return f"s3://{bucket}/{prefix}/{exchange}/{data_type}/{date}/"


def local_date_dir(
    download_dir: Path, prefix: str, exchange: str, data_type: str, date: str
) -> Path:
    prefix = normalize_path_part(prefix, "prefix")
    exchange = normalize_path_part(exchange, "exchange")
    data_type = normalize_path_part(data_type, "data-type")
    return download_dir.expanduser() / prefix / exchange / data_type / date


def build_symbol_downloads(
    *,
    bucket: str,
    prefix: str,
    exchange: str,
    data_type: str,
    dates: list[str],
    symbols: list[str],
    download_dir: Path,
    aws_profile: str | None,
    no_overwrite: bool,
) -> list[DownloadCommand]:
    commands = []
    base_command = aws_base_command(aws_profile)
    data_type = normalize_path_part(data_type, "data-type")
    for date in dates:
        date_prefix = s3_prefix(bucket, prefix, exchange, data_type, date)
        destination_dir = local_date_dir(download_dir, prefix, exchange, data_type, date)
        for raw_symbol in symbols:
            symbol = normalize_symbol(raw_symbol)
            if not symbol:
                continue
            filename = f"{symbol}-{data_type}-{date}.csv.gz"
            source = f"{date_prefix}{filename}"
            destination = destination_dir / filename
            command = [*base_command, "cp", source, str(destination)]
            if no_overwrite:
                command.append("--no-overwrite")
            commands.append(
                DownloadCommand(source=source, destination=destination, command=command)
            )
    return commands


def build_all_symbols_downloads(
    *,
    bucket: str,
    prefix: str,
    exchange: str,
    data_type: str,
    dates: list[str],
    download_dir: Path,
    aws_profile: str | None,
    no_overwrite: bool,
    include_empty_marker: bool,
) -> list[DownloadCommand]:
    commands = []
    base_command = aws_base_command(aws_profile)
    for date in dates:
        source = s3_prefix(bucket, prefix, exchange, data_type, date)
        destination = local_date_dir(download_dir, prefix, exchange, data_type, date)
        command = [
            *base_command,
            "cp",
            source,
            str(destination),
            "--recursive",
        ]
        if not include_empty_marker:
            command.extend(["--exclude", "*", "--include", "*.csv.gz"])
        if no_overwrite:
            command.append("--no-overwrite")
        commands.append(DownloadCommand(source=source, destination=destination, command=command))
    return commands


def build_downloads(args: argparse.Namespace) -> list[DownloadCommand]:
    dates = expand_date_range(args.start_date, args.end_date)
    download_dir = Path(args.download_dir).expanduser()
    if args.all_symbols:
        return build_all_symbols_downloads(
            bucket=args.bucket,
            prefix=args.prefix,
            exchange=args.exchange,
            data_type=args.data_type,
            dates=dates,
            download_dir=download_dir,
            aws_profile=args.aws_profile,
            no_overwrite=args.no_overwrite,
            include_empty_marker=args.include_empty_marker,
        )
    return build_symbol_downloads(
        bucket=args.bucket,
        prefix=args.prefix,
        exchange=args.exchange,
        data_type=args.data_type,
        dates=dates,
        symbols=args.symbols,
        download_dir=download_dir,
        aws_profile=args.aws_profile,
        no_overwrite=args.no_overwrite,
    )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Download Tardis S3 data while preserving "
            "tardis/<exchange>/<data_type>/<YYYYMMDD>/... locally."
        )
    )
    parser.add_argument("--bucket", default=DEFAULT_BUCKET, help="S3 bucket name.")
    parser.add_argument("--prefix", default=DEFAULT_PREFIX, help="Root S3 prefix.")
    parser.add_argument("--exchange", required=True, help="Exchange layer, e.g. binance-futures.")
    parser.add_argument("--data-type", required=True, help="Data type layer, e.g. book_ticker.")
    parser.add_argument("--start-date", required=True, help="Inclusive start date, YYYYMMDD.")
    parser.add_argument("--end-date", required=True, help="Inclusive end date, YYYYMMDD.")
    parser.add_argument(
        "--download-dir",
        default=str(DEFAULT_DOWNLOAD_DIR),
        help=f"Local download root directory. Default: {DEFAULT_DOWNLOAD_DIR}",
    )
    parser.add_argument("--aws-profile", help="Optional AWS CLI profile name.")
    parser.add_argument("--dry-run", action="store_true", help="Print AWS CLI commands only.")
    parser.add_argument(
        "--no-overwrite",
        action="store_true",
        help="Pass --no-overwrite to AWS CLI downloads.",
    )
    parser.add_argument(
        "--include-empty-marker",
        action="store_true",
        help="For --all-symbols, include non-CSV marker objects instead of filtering to *.csv.gz.",
    )

    symbol_group = parser.add_mutually_exclusive_group(required=True)
    symbol_group.add_argument(
        "--symbols",
        nargs="+",
        help="Symbols to download, e.g. BTCUSDC ETHUSDC.",
    )
    symbol_group.add_argument(
        "--all-symbols",
        action="store_true",
        help="Download all *.csv.gz files for each selected date.",
    )
    return parser.parse_args(argv)


def print_command(command: list[str]) -> None:
    print(" ".join(shlex.quote(part) for part in command))


def run_downloads(downloads: list[DownloadCommand], dry_run: bool) -> int:
    for download in downloads:
        if dry_run:
            print_command(download.command)
            continue
        download.destination.parent.mkdir(parents=True, exist_ok=True)
        completed = subprocess.run(download.command, check=False)
        if completed.returncode != 0:
            return completed.returncode
    return 0


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if shutil.which("aws") is None:
        print("missing dependency: aws CLI is not available in PATH", file=sys.stderr)
        return 127

    try:
        downloads = build_downloads(args)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    if not downloads:
        print("no download commands generated", file=sys.stderr)
        return 2

    return run_downloads(downloads, dry_run=args.dry_run)


if __name__ == "__main__":
    raise SystemExit(main())
