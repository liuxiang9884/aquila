#!/home/liuxiang/dev/pyenv/lx/bin/python

import unittest
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
from pathlib import Path
from tempfile import TemporaryDirectory

import download_tardis_s3 as downloader


class DownloadTardisS3Test(unittest.TestCase):
    def test_expand_date_range_inclusive(self):
        self.assertEqual(
            downloader.expand_date_range("20260415", "20260417"),
            ["20260415", "20260416", "20260417"],
        )

    def test_expand_date_range_rejects_reverse_range(self):
        with self.assertRaisesRegex(ValueError, "start-date"):
            downloader.expand_date_range("20260418", "20260417")

    def test_symbol_download_plan_preserves_s3_layout(self):
        plan = downloader.build_symbol_downloads(
            bucket="tko-s3-tardis-share",
            prefix="tardis",
            exchange="binance-futures",
            data_type="book_ticker",
            dates=["20260415"],
            symbols=["btcusdc", "ETHUSDC"],
            output_dir=Path("/home/liuxiang/tardis"),
            aws_profile=None,
            no_overwrite=False,
        )

        self.assertEqual(len(plan), 2)
        self.assertEqual(
            plan[0].source,
            "s3://tko-s3-tardis-share/tardis/binance-futures/book_ticker/20260415/"
            "BTCUSDC-book_ticker-20260415.csv.gz",
        )
        self.assertEqual(
            plan[0].destination,
            Path(
                "/home/liuxiang/tardis/tardis/binance-futures/book_ticker/20260415/"
                "BTCUSDC-book_ticker-20260415.csv.gz"
            ),
        )
        self.assertEqual(
            plan[0].command,
            [
                "aws",
                "s3",
                "cp",
                "s3://tko-s3-tardis-share/tardis/binance-futures/book_ticker/20260415/"
                "BTCUSDC-book_ticker-20260415.csv.gz",
                "/home/liuxiang/tardis/tardis/binance-futures/book_ticker/20260415/"
                "BTCUSDC-book_ticker-20260415.csv.gz",
            ],
        )

    def test_symbol_download_plan_preserves_exchange_symbol_separator(self):
        plan = downloader.build_symbol_downloads(
            bucket="tko-s3-tardis-share",
            prefix="tardis",
            exchange="gate-io-futures",
            data_type="book_ticker",
            dates=["20260415"],
            symbols=["ordi_usdt"],
            output_dir=Path("/home/liuxiang/tardis"),
            aws_profile=None,
            no_overwrite=False,
        )

        self.assertEqual(
            plan[0].source,
            "s3://tko-s3-tardis-share/tardis/gate-io-futures/book_ticker/20260415/"
            "ORDI_USDT-book_ticker-20260415.csv.gz",
        )
        self.assertEqual(
            plan[0].destination,
            Path(
                "/home/liuxiang/tardis/tardis/gate-io-futures/book_ticker/20260415/"
                "ORDI_USDT-book_ticker-20260415.csv.gz"
            ),
        )

    def test_all_symbols_uses_recursive_cp_for_each_date_directory(self):
        plan = downloader.build_all_symbols_downloads(
            bucket="tko-s3-tardis-share",
            prefix="tardis",
            exchange="binance-futures",
            data_type="book_ticker",
            dates=["20260415", "20260416"],
            output_dir=Path("/tmp/tardis"),
            aws_profile="research",
            no_overwrite=True,
            include_empty_marker=False,
        )

        self.assertEqual(len(plan), 2)
        self.assertEqual(
            plan[0].command,
            [
                "aws",
                "--profile",
                "research",
                "s3",
                "cp",
                "s3://tko-s3-tardis-share/tardis/binance-futures/book_ticker/20260415/",
                "/tmp/tardis/tardis/binance-futures/book_ticker/20260415",
                "--recursive",
                "--exclude",
                "*",
                "--include",
                "*.csv.gz",
                "--no-overwrite",
            ],
        )

    def test_all_symbols_can_include_empty_marker_when_requested(self):
        plan = downloader.build_all_symbols_downloads(
            bucket="tko-s3-tardis-share",
            prefix="tardis",
            exchange="binance-futures",
            data_type="book_ticker",
            dates=["20260415"],
            output_dir=Path("/tmp/tardis"),
            aws_profile=None,
            no_overwrite=False,
            include_empty_marker=True,
        )

        self.assertEqual(
            plan[0].command,
            [
                "aws",
                "s3",
                "cp",
                "s3://tko-s3-tardis-share/tardis/binance-futures/book_ticker/20260415/",
                "/tmp/tardis/tardis/binance-futures/book_ticker/20260415",
                "--recursive",
            ],
        )

    def test_parse_args_requires_symbols_or_all_symbols(self):
        with redirect_stderr(StringIO()):
            with self.assertRaises(SystemExit):
                downloader.parse_args(
                    [
                        "--exchange",
                        "binance-futures",
                        "--data-type",
                        "book_ticker",
                        "--start-date",
                        "20260415",
                        "--end-date",
                        "20260415",
                    ]
                )

    def test_dry_run_does_not_create_destination_directory(self):
        with TemporaryDirectory() as temp_dir:
            destination = Path(temp_dir) / "tardis/binance-futures/book_ticker/20260415/BTC.csv.gz"
            command = downloader.DownloadCommand(
                source="s3://example/BTC.csv.gz",
                destination=destination,
                command=["aws", "s3", "cp", "s3://example/BTC.csv.gz", str(destination)],
            )

            with redirect_stdout(StringIO()):
                result = downloader.run_downloads([command], dry_run=True)

            self.assertEqual(result, 0)
            self.assertFalse(destination.parent.exists())


if __name__ == "__main__":
    unittest.main()
