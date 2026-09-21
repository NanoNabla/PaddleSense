#!/usr/bin/env python3
"""paddle_csv.py — convert paddle-meter recordings to/from a readable CSV.

The paddle-meter firmware (see ``firmware/src/storage_task.cpp``) writes compact
CSV recordings to the SD card::

    # paddle-meter v1 rate=200 arange=4g grange=500dps
    t_us,ax,ay,az,gx,gy,gz
    1234567,0.1234,-9.8012,0.4321,1.20,-0.50,3.10
    ...
    # samples=41230 dropped=0

This tool has two directions:

``to-csv``
    Expand a paddle-meter recording into a *readable* CSV: one row per sample
    with a row index, the timestamp in both microseconds and seconds, and
    derived acceleration / angular-rate magnitudes. Metadata is preserved as
    leading ``#`` comment lines so the file can be converted back losslessly.

``to-paddle``
    Take a CSV (the readable form above, or any CSV with recognisable column
    names) and emit a paddle-meter recording that the firmware / Android
    ``TimeSeriesReader`` can consume.

Both directions are pure standard library and stream row-by-row, so multi-MB
recordings convert without loading everything into memory.

Examples
--------
    # paddle recording -> readable csv
    python3 tools/paddle_csv.py to-csv pm_0001.csv -o pm_0001.readable.csv

    # readable csv -> paddle recording (metadata taken from the # header)
    python3 tools/paddle_csv.py to-paddle pm_0001.readable.csv -o pm_0001.csv

    # arbitrary csv with named columns -> paddle recording at 100 Hz
    python3 tools/paddle_csv.py to-paddle raw.csv -o pm_0002.csv --rate 100

    # auto-detect direction
    python3 tools/paddle_csv.py auto somefile.csv -o out.csv
"""

from __future__ import annotations

import argparse
import csv
import io
import math
import re
import sys
from dataclasses import dataclass, field
from typing import Iterable, Iterator, Optional, TextIO

# ---------------------------------------------------------------------------
# Format constants
# ---------------------------------------------------------------------------

PADDLE_MARKER = "paddle-meter"
COLUMNS = ("t_us", "ax", "ay", "az", "gx", "gy", "gz")

DEFAULT_VERSION = "1"
DEFAULT_RATE_HZ = 200
DEFAULT_ACCEL_RANGE_G = 4
DEFAULT_GYRO_RANGE_DPS = 500

# Readable output columns (in order).
READABLE_COLUMNS = (
    "index",
    "t_us",
    "t_s",
    "ax",
    "ay",
    "az",
    "gx",
    "gy",
    "gz",
    "accel_mag",
    "gyro_mag",
)

# Column-name aliases accepted by ``to-paddle`` (normalised: lower, _ for space).
_TIME_ALIASES = {
    "t_us": "us",
    "time_us": "us",
    "timestamp_us": "us",
    "t_ms": "ms",
    "time_ms": "ms",
    "timestamp_ms": "ms",
    "t_s": "s",
    "time_s": "s",
    "timestamp_s": "s",
    "seconds": "s",
    "secs": "s",
    "t": "auto",
    "time": "auto",
    "timestamp": "auto",
}

_AXIS_ALIASES = {
    "ax": "ax", "accel_x": "ax", "a_x": "ax", "acc_x": "ax", "acceleration_x": "ax",
    "ay": "ay", "accel_y": "ay", "a_y": "ay", "acc_y": "ay", "acceleration_y": "ay",
    "az": "az", "accel_z": "az", "a_z": "az", "acc_z": "az", "acceleration_z": "az",
    "gx": "gx", "gyro_x": "gx", "g_x": "gx", "gyr_x": "gx", "angular_x": "gx",
    "gy": "gy", "gyro_y": "gy", "g_y": "gy", "gyr_y": "gy", "angular_y": "gy",
    "gz": "gz", "gyro_z": "gz", "g_z": "gz", "gyr_z": "gz", "angular_z": "gz",
}

_META_HEADER_RE = re.compile(
    r"#\s*paddle-meter\s+v(?P<version>\S+)"
    r"(?:\s+rate=(?P<rate>\d+))?"
    r"(?:\s+arange=(?P<arange>\d+)g)?"
    r"(?:\s+grange=(?P<grange>\d+)dps)?"
)
_META_FOOTER_RE = re.compile(
    r"#\s*samples=(?P<samples>\d+)(?:\s+dropped=(?P<dropped>\d+))?"
)


# ---------------------------------------------------------------------------
# Metadata
# ---------------------------------------------------------------------------


@dataclass
class Meta:
    """Recording metadata carried in the paddle-meter header/footer comments."""

    version: str = DEFAULT_VERSION
    rate_hz: int = DEFAULT_RATE_HZ
    accel_range_g: int = DEFAULT_ACCEL_RANGE_G
    gyro_range_dps: int = DEFAULT_GYRO_RANGE_DPS
    sample_count: Optional[int] = None
    dropped: Optional[int] = None

    def header_line(self) -> str:
        return (
            f"# {PADDLE_MARKER} v{self.version} rate={self.rate_hz} "
            f"arange={self.accel_range_g}g grange={self.gyro_range_dps}dps"
        )

    def footer_line(self, samples: int, dropped: int) -> str:
        return f"# samples={samples} dropped={dropped}"


def parse_meta_line(line: str, meta: Meta) -> Meta:
    """Update *meta* from a single ``#`` comment line (header or footer)."""
    m = _META_HEADER_RE.search(line)
    if m:
        if m.group("version"):
            meta.version = m.group("version")
        if m.group("rate"):
            meta.rate_hz = int(m.group("rate"))
        if m.group("arange"):
            meta.accel_range_g = int(m.group("arange"))
        if m.group("grange"):
            meta.gyro_range_dps = int(m.group("grange"))
        return meta

    m = _META_FOOTER_RE.search(line)
    if m:
        meta.sample_count = int(m.group("samples"))
        if m.group("dropped") is not None:
            meta.dropped = int(m.group("dropped"))
    return meta


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _open_text(path: str, mode: str) -> TextIO:
    """Open *path* (or stdin/stdout for ``-``) as UTF-8 text."""
    if path == "-":
        return sys.stdin if "r" in mode else sys.stdout
    return open(path, mode, encoding="utf-8", newline="")


def _fmt(value: float, precision: int) -> str:
    """Format a float without a trailing ``-0`` and with fixed precision."""
    if value == 0:
        value = 0.0
    return f"{value:.{precision}f}"


def _is_paddle_file(path: str) -> bool:
    """Heuristically decide whether *path* is a paddle-meter recording.

    A readable CSV produced by ``to-csv`` also carries the ``# paddle-meter``
    header comment, so the presence of an ``index,`` column header takes
    precedence over the marker.
    """
    try:
        with _open_text(path, "r") as fh:
            saw_marker = False
            for _ in range(10):
                line = fh.readline()
                if not line:
                    break
                if line.strip().lower().startswith("index,"):
                    return False
                if PADDLE_MARKER in line:
                    saw_marker = True
            return saw_marker
    except OSError:
        return False


# ---------------------------------------------------------------------------
# to-csv : paddle-meter -> readable CSV
# ---------------------------------------------------------------------------


def paddle_to_readable(
    src: TextIO,
    dst: TextIO,
    *,
    precision: int = 6,
    derived: bool = True,
) -> int:
    """Convert a paddle-meter recording from *src* into a readable CSV on *dst*.

    Returns the number of samples written.
    """
    meta = Meta()
    saw_header = False
    count = 0

    out = csv.writer(dst, lineterminator="\n")
    columns = READABLE_COLUMNS if derived else READABLE_COLUMNS[:9]

    for raw in src:
        line = raw.strip()
        if not line:
            continue

        if line.startswith("#"):
            meta = parse_meta_line(line, meta)
            continue

        if not saw_header:
            # Column header row (t_us,ax,...). Emit our own header instead.
            saw_header = True
            dst.write(meta.header_line() + "\n")
            dst.write("# generated by paddle_csv.py to-csv\n")
            out.writerow(columns)
            continue

        parts = line.split(",")
        if len(parts) < 7:
            continue
        try:
            t_us = int(parts[0])
            ax, ay, az, gx, gy, gz = (float(parts[i]) for i in range(1, 7))
        except ValueError:
            continue

        row = [
            count,
            t_us,
            _fmt(t_us / 1_000_000.0, precision),
            _fmt(ax, precision),
            _fmt(ay, precision),
            _fmt(az, precision),
            _fmt(gx, precision),
            _fmt(gy, precision),
            _fmt(gz, precision),
        ]
        if derived:
            row.append(_fmt(math.sqrt(ax * ax + ay * ay + az * az), precision))
            row.append(_fmt(math.sqrt(gx * gx + gy * gy + gz * gz), precision))
        out.writerow(row)
        count += 1

    # Footer (source footer is parsed into meta as we stream past it).
    if meta.sample_count is not None:
        dropped = meta.dropped if meta.dropped is not None else 0
        dst.write(f"# samples={meta.sample_count} dropped={dropped}\n")

    return count


# ---------------------------------------------------------------------------
# to-paddle : CSV -> paddle-meter
# ---------------------------------------------------------------------------


@dataclass
class _ColumnMap:
    time: Optional[int] = None
    time_unit: str = "us"  # "us" | "ms" | "s"
    axes: dict = field(default_factory=dict)  # canonical name -> column index


def _normalise(name: str) -> str:
    return name.strip().lower().replace(" ", "_").replace("-", "_")


def _resolve_columns(header: Iterable[str], time_unit: str) -> _ColumnMap:
    """Map a CSV header row to paddle-meter fields."""
    cmap = _ColumnMap()
    for idx, raw in enumerate(header):
        name = _normalise(raw)
        if name in _TIME_ALIASES:
            cmap.time = idx
            unit = _TIME_ALIASES[name]
            cmap.time_unit = time_unit if unit == "auto" else unit
        elif name in _AXIS_ALIASES:
            cmap.axes[_AXIS_ALIASES[name]] = idx
    return cmap


def _to_micros(value: float, unit: str) -> int:
    if unit == "s":
        return int(round(value * 1_000_000))
    if unit == "ms":
        return int(round(value * 1_000))
    return int(round(value))


def csv_to_paddle(
    src: TextIO,
    dst: TextIO,
    *,
    meta: Optional[Meta] = None,
    time_unit: str = "us",
    precision: int = 4,
    gyro_precision: int = 2,
) -> int:
    """Convert a CSV from *src* into a paddle-meter recording on *dst*.

    Column names are matched case-insensitively against a set of aliases
    (``t_us``/``time_s``/``ax``/``accel_x``/``gx``/``gyro_x``/…). If no time
    column is present, timestamps are synthesised from ``meta.rate_hz``.

    Values supplied in *meta* take precedence over metadata found in the
    source file's comment lines (so CLI overrides win).

    Returns the number of samples written.
    """
    overrides = meta or Meta()
    meta = Meta()

    reader = csv.reader(src)
    header: Optional[list[str]] = None
    cmap = _ColumnMap()

    # Skip leading comment lines, capture the first real row as the header.
    for row in reader:
        if not row:
            continue
        if row[0].lstrip().startswith("#"):
            meta = parse_meta_line(row[0], meta)
            continue
        header = row
        break

    # Apply explicit overrides on top of whatever the source declared.
    if overrides.version != DEFAULT_VERSION:
        meta.version = overrides.version
    if overrides.rate_hz != DEFAULT_RATE_HZ:
        meta.rate_hz = overrides.rate_hz
    if overrides.accel_range_g != DEFAULT_ACCEL_RANGE_G:
        meta.accel_range_g = overrides.accel_range_g
    if overrides.gyro_range_dps != DEFAULT_GYRO_RANGE_DPS:
        meta.gyro_range_dps = overrides.gyro_range_dps
    if overrides.dropped is not None:
        meta.dropped = overrides.dropped

    if header is None:
        raise ValueError("input CSV has no header row")

    cmap = _resolve_columns(header, time_unit)

    missing = [c for c in ("ax", "ay", "az", "gx", "gy", "gz") if c not in cmap.axes]
    if missing:
        raise ValueError(
            "could not find column(s) for: " + ", ".join(missing)
            + f"\n  header was: {','.join(header)}"
        )

    # Write the paddle header.
    dst.write(meta.header_line() + "\n")
    dst.write(",".join(COLUMNS) + "\n")

    count = 0
    synth_period_us = 1_000_000.0 / max(meta.rate_hz, 1)

    for row in reader:
        if not row or row[0].lstrip().startswith("#"):
            continue
        try:
            if cmap.time is not None:
                t_us = _to_micros(float(row[cmap.time]), cmap.time_unit)
            else:
                t_us = int(round(count * synth_period_us))
            ax = float(row[cmap.axes["ax"]])
            ay = float(row[cmap.axes["ay"]])
            az = float(row[cmap.axes["az"]])
            gx = float(row[cmap.axes["gx"]])
            gy = float(row[cmap.axes["gy"]])
            gz = float(row[cmap.axes["gz"]])
        except (ValueError, IndexError):
            continue

        dst.write(
            f"{t_us},{_fmt(ax, precision)},{_fmt(ay, precision)},"
            f"{_fmt(az, precision)},{_fmt(gx, gyro_precision)},"
            f"{_fmt(gy, gyro_precision)},{_fmt(gz, gyro_precision)}\n"
        )
        count += 1

    dropped = meta.dropped if meta.dropped is not None else 0
    dst.write(meta.footer_line(count, dropped) + "\n")
    return count


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="paddle_csv.py",
        description="Convert paddle-meter recordings to/from a readable CSV.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = parser.add_subparsers(dest="command", required=True)

    def add_common(p: argparse.ArgumentParser) -> None:
        p.add_argument("input", help="input file, or '-' for stdin")
        p.add_argument("-o", "--output", default="-",
                       help="output file, or '-' for stdout (default: stdout)")

    p_to = sub.add_parser(
        "to-csv", help="paddle-meter recording -> readable CSV")
    add_common(p_to)
    p_to.add_argument("--precision", type=int, default=6,
                      help="decimal places for values (default: 6)")
    p_to.add_argument("--no-derived", action="store_true",
                      help="omit accel_mag / gyro_mag columns")

    p_from = sub.add_parser(
        "to-paddle", help="CSV -> paddle-meter recording")
    add_common(p_from)
    p_from.add_argument("--rate", type=int, default=None,
                        help="sample rate in Hz (default: from header or 200)")
    p_from.add_argument("--arange", type=int, default=None,
                        help="accel full-scale in g (default: from header or 4)")
    p_from.add_argument("--grange", type=int, default=None,
                        help="gyro full-scale in deg/s (default: from header or 500)")
    p_from.add_argument("--version", default=None,
                        help="firmware version string (default: from header or 1)")
    p_from.add_argument("--dropped", type=int, default=None,
                        help="dropped-sample count for the footer (default: 0)")
    p_from.add_argument("--time-unit", choices=("us", "ms", "s"), default="us",
                        help="unit for a plain 't'/'time' column (default: us)")
    p_from.add_argument("--precision", type=int, default=4,
                        help="decimal places for accel (default: 4)")
    p_from.add_argument("--gyro-precision", type=int, default=2,
                        help="decimal places for gyro (default: 2)")

    p_auto = sub.add_parser(
        "auto", help="detect direction from the input and convert")
    add_common(p_auto)
    p_auto.add_argument("--precision", type=int, default=6)
    p_auto.add_argument("--rate", type=int, default=None)
    p_auto.add_argument("--time-unit", choices=("us", "ms", "s"), default="us")

    return parser


def main(argv: Optional[list[str]] = None) -> int:
    args = _build_parser().parse_args(argv)

    if args.command == "auto":
        args.command = "to-csv" if _is_paddle_file(args.input) else "to-paddle"

    with _open_text(args.input, "r") as src, _open_text(args.output, "w") as dst:
        if args.command == "to-csv":
            n = paddle_to_readable(
                src, dst,
                precision=args.precision,
                derived=not getattr(args, "no_derived", False),
            )
            verb = "samples -> readable CSV"
        else:
            meta = Meta()
            if args.rate is not None:
                meta.rate_hz = args.rate
            if getattr(args, "arange", None) is not None:
                meta.accel_range_g = args.arange
            if getattr(args, "grange", None) is not None:
                meta.gyro_range_dps = args.grange
            if getattr(args, "version", None) is not None:
                meta.version = args.version
            if getattr(args, "dropped", None) is not None:
                meta.dropped = args.dropped
            n = csv_to_paddle(
                src, dst,
                meta=meta,
                time_unit=args.time_unit,
                precision=args.precision,
                gyro_precision=getattr(args, "gyro_precision", 2),
            )
            verb = "rows -> paddle-meter recording"

    if args.output != "-":
        print(f"wrote {n} {verb} to {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
