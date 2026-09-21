#!/usr/bin/env python3
"""Unit tests for tools/paddle_csv.py.

Run with either::

    python3 -m unittest discover -s tools/tests
    python3 tools/tests/test_paddle_csv.py
"""

from __future__ import annotations

import io
import os
import sys
import unittest

# Make ``tools/`` importable regardless of the working directory.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import paddle_csv  # noqa: E402

FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "fixtures", "pm_0001.csv")


def _read_fixture() -> str:
    with open(FIXTURE, encoding="utf-8") as fh:
        return fh.read()


class MetaParsingTests(unittest.TestCase):
    def test_header_parsed(self):
        meta = paddle_csv.parse_meta_line(
            "# paddle-meter v1 rate=200 arange=4g grange=500dps",
            paddle_csv.Meta(),
        )
        self.assertEqual(meta.version, "1")
        self.assertEqual(meta.rate_hz, 200)
        self.assertEqual(meta.accel_range_g, 4)
        self.assertEqual(meta.gyro_range_dps, 500)

    def test_footer_parsed(self):
        meta = paddle_csv.parse_meta_line(
            "# samples=41230 dropped=7", paddle_csv.Meta())
        self.assertEqual(meta.sample_count, 41230)
        self.assertEqual(meta.dropped, 7)

    def test_header_roundtrip(self):
        meta = paddle_csv.Meta(version="2", rate_hz=100,
                               accel_range_g=8, gyro_range_dps=1000)
        reparsed = paddle_csv.parse_meta_line(meta.header_line(),
                                              paddle_csv.Meta())
        self.assertEqual(reparsed.version, "2")
        self.assertEqual(reparsed.rate_hz, 100)
        self.assertEqual(reparsed.accel_range_g, 8)
        self.assertEqual(reparsed.gyro_range_dps, 1000)


class ToCsvTests(unittest.TestCase):
    def test_basic_conversion(self):
        out = io.StringIO()
        n = paddle_csv.paddle_to_readable(io.StringIO(_read_fixture()), out)
        self.assertEqual(n, 5)

        lines = out.getvalue().splitlines()
        # Metadata preserved as comments.
        self.assertTrue(lines[0].startswith("# paddle-meter v1 rate=200"))
        # Header row.
        self.assertEqual(lines[2], ",".join(paddle_csv.READABLE_COLUMNS))
        # Footer preserved at the end.
        self.assertEqual(lines[-1], "# samples=5 dropped=0")

        # First data row: index, t_us, t_s, ax..gz, accel_mag, gyro_mag
        first = lines[3].split(",")
        self.assertEqual(first[0], "0")
        self.assertEqual(first[1], "0")
        self.assertEqual(first[2], "0.000000")
        self.assertEqual(first[3], "0.123400")
        # accel magnitude = sqrt(0.1234^2 + 9.8012^2 + 0.4321^2)
        self.assertAlmostEqual(float(first[9]), 9.812, places=2)

    def test_no_derived_columns(self):
        out = io.StringIO()
        paddle_csv.paddle_to_readable(io.StringIO(_read_fixture()), out,
                                      derived=False)
        header = out.getvalue().splitlines()[2]
        self.assertNotIn("accel_mag", header)
        self.assertEqual(len(header.split(",")), 9)

    def test_skips_malformed_rows(self):
        data = (
            "# paddle-meter v1 rate=200 arange=4g grange=500dps\n"
            "t_us,ax,ay,az,gx,gy,gz\n"
            "0,1,2,3,4,5,6\n"
            "garbage,row\n"
            "5000,1,2,3,4,5,6\n"
        )
        out = io.StringIO()
        n = paddle_csv.paddle_to_readable(io.StringIO(data), out)
        self.assertEqual(n, 2)


class ToPaddleTests(unittest.TestCase):
    def test_readable_roundtrip(self):
        # paddle -> readable -> paddle should preserve samples and metadata.
        readable = io.StringIO()
        paddle_csv.paddle_to_readable(io.StringIO(_read_fixture()), readable)

        out = io.StringIO()
        n = paddle_csv.csv_to_paddle(io.StringIO(readable.getvalue()), out)
        self.assertEqual(n, 5)

        lines = out.getvalue().splitlines()
        self.assertEqual(lines[0],
                         "# paddle-meter v1 rate=200 arange=4g grange=500dps")
        self.assertEqual(lines[1], "t_us,ax,ay,az,gx,gy,gz")
        self.assertEqual(lines[2], "0,0.1234,-9.8012,0.4321,1.20,-0.50,3.10")
        self.assertEqual(lines[-1], "# samples=5 dropped=0")

    def test_alias_columns(self):
        data = (
            "time_s,accel_x,accel_y,accel_z,gyro_x,gyro_y,gyro_z\n"
            "0.0,1.0,2.0,3.0,4.0,5.0,6.0\n"
            "0.5,1.1,2.1,3.1,4.1,5.1,6.1\n"
        )
        out = io.StringIO()
        n = paddle_csv.csv_to_paddle(io.StringIO(data), out)
        self.assertEqual(n, 2)
        lines = out.getvalue().splitlines()
        # 0.5 s -> 500000 us
        self.assertTrue(lines[3].startswith("500000,"))

    def test_synthesised_timestamps(self):
        data = (
            "ax,ay,az,gx,gy,gz\n"
            "1,2,3,4,5,6\n"
            "1,2,3,4,5,6\n"
            "1,2,3,4,5,6\n"
        )
        out = io.StringIO()
        n = paddle_csv.csv_to_paddle(io.StringIO(data), out,
                                     meta=paddle_csv.Meta(rate_hz=100))
        self.assertEqual(n, 3)
        lines = out.getvalue().splitlines()
        # 100 Hz -> 10000 us period.
        self.assertTrue(lines[2].startswith("0,"))
        self.assertTrue(lines[3].startswith("10000,"))
        self.assertTrue(lines[4].startswith("20000,"))

    def test_missing_axis_raises(self):
        data = "t_us,ax,ay,az,gx,gy\n0,1,2,3,4,5\n"
        with self.assertRaises(ValueError):
            paddle_csv.csv_to_paddle(io.StringIO(data), io.StringIO())

    def test_cli_overrides_metadata(self):
        out = io.StringIO()
        paddle_csv.csv_to_paddle(
            io.StringIO(_read_fixture()), out,
            meta=paddle_csv.Meta(rate_hz=100, accel_range_g=8,
                                 gyro_range_dps=1000, version="9"),
        )
        self.assertEqual(
            out.getvalue().splitlines()[0],
            "# paddle-meter v9 rate=100 arange=8g grange=1000dps",
        )


class DetectionTests(unittest.TestCase):
    def test_detects_paddle_file(self):
        self.assertTrue(paddle_csv._is_paddle_file(FIXTURE))

    def test_detects_readable_file(self):
        readable = io.StringIO()
        paddle_csv.paddle_to_readable(io.StringIO(_read_fixture()), readable)
        tmp = os.path.join(os.path.dirname(FIXTURE), "_readable_tmp.csv")
        try:
            with open(tmp, "w", encoding="utf-8") as fh:
                fh.write(readable.getvalue())
            self.assertFalse(paddle_csv._is_paddle_file(tmp))
        finally:
            if os.path.exists(tmp):
                os.remove(tmp)


if __name__ == "__main__":
    unittest.main(verbosity=2)
