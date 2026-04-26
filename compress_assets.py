"""
compress_assets.py — PlatformIO pre-script for LittleFS asset compression.

Place in project root alongside platformio.ini and add to your env:

    extra_scripts = pre:compress_assets.py

Then just use `pio run -t uploadfs` as normal — compression runs first.

To confirm it worked:
  1. BUILD TIME: Watch the PlatformIO terminal — this script prints a table
     showing every file compressed with before/after sizes. If a file is
     already up-to-date it says "skip (up to date)".

  2. AFTER UPLOAD: Open a serial monitor. At startup the firmware prints
     a full LittleFS inventory tagged [app_srv] showing every file and
     marking .gz files with "[gz — will be served compressed]".

  3. DURING USE: Each file request logs:
       [app_srv] serve /littlefs/index.js [gz, 5706 B]
     If it says "plain" instead of "gz" the .gz file is missing from LittleFS.
"""

import gzip
import os
from pathlib import Path

Import("env")  # noqa: F821 — SCons injects this

ASSETS = [
    "index.html",
    "index.css",
    "index.js",
    "base.css",
    "settings.html",
    "settings.css",
    "settings.js",
    "setup.html",
    "setup.css",
    "setup.js",
    "favicon.svg",
]

# ANSI colours — PlatformIO terminal supports them
_GRN  = "\033[32m"
_YLW  = "\033[33m"
_RED  = "\033[31m"
_CYN  = "\033[36m"
_RST  = "\033[0m"
_BLD  = "\033[1m"


def _bar(pct, width=20):
    """ASCII progress bar showing compression ratio."""
    filled = int(width * pct / 100)
    return "[" + "█" * filled + "·" * (width - filled) + "]"


def compress_assets(source, target, env):
    data_dir = Path(env.subst("$PROJECT_DATA_DIR"))

    print()
    print(f"{_BLD}{_CYN}┌─ compress_assets ──────────────────────────────────────────┐{_RST}")

    if not data_dir.exists():
        print(f"{_RED}│  ERROR: data/ not found at {data_dir}{_RST}")
        print(f"{_CYN}└────────────────────────────────────────────────────────────┘{_RST}")
        return

    print(f"{_CYN}│{_RST}  Source: {data_dir}")
    print(f"{_CYN}│{_RST}  {'File':<24} {'Original':>9}   {'Compressed':>10}   {'Ratio':>5}  Status")
    print(f"{_CYN}│{_RST}  {'─'*24}   {'─'*9}   {'─'*10}   {'─'*5}  {'─'*14}")

    total_orig   = 0
    total_gz     = 0
    n_compressed = 0
    n_skipped    = 0
    n_missing    = 0

    for name in ASSETS:
        src = data_dir / name
        dst = data_dir / (name + ".gz")

        if not src.exists():
            print(f"{_CYN}│{_RST}  {_RED}{name:<24}{_RST}  {'—':>9}   {'—':>10}   {'—':>5}  MISSING")
            n_missing += 1
            continue

        orig_size = src.stat().st_size
        src_mtime = src.stat().st_mtime
        dst_mtime = dst.stat().st_mtime if dst.exists() else 0

        if dst_mtime >= src_mtime and dst.exists():
            gz_size = dst.stat().st_size
            pct     = gz_size * 100 // orig_size if orig_size else 0
            print(f"{_CYN}│{_RST}  {name:<24}  {orig_size:>8,} B   {gz_size:>9,} B   "
                  f"{pct:>4}%  {_YLW}skip (up to date){_RST}")
            total_orig += orig_size
            total_gz   += gz_size
            n_skipped  += 1
            continue

        raw     = src.read_bytes()
        gz_data = gzip.compress(raw, compresslevel=9)
        gz_size = len(gz_data)
        dst.write_bytes(gz_data)
        os.utime(dst, (src_mtime, src_mtime))

        pct = gz_size * 100 // orig_size if orig_size else 0
        print(f"{_CYN}│{_RST}  {name:<24}  {orig_size:>8,} B   {gz_size:>9,} B   "
              f"{pct:>4}%  {_GRN}compressed{_RST}")
        total_orig   += orig_size
        total_gz     += gz_size
        n_compressed += 1

    # Summary line
    print(f"{_CYN}│{_RST}  {'─'*24}   {'─'*9}   {'─'*10}   {'─'*5}")
    if total_orig:
        saved = total_orig - total_gz
        pct   = total_gz * 100 // total_orig
        print(f"{_CYN}│{_RST}  {'TOTAL':<24}  {total_orig:>8,} B   {total_gz:>9,} B   "
              f"{pct:>4}%  saved {saved:,} B")
        print(f"{_CYN}│{_RST}  {_bar(100 - pct)} {100-pct}% smaller")

    print(f"{_CYN}│{_RST}")
    if n_missing:
        print(f"{_CYN}│{_RST}  {_RED}⚠  {n_missing} file(s) missing from data/ — check ASSETS list{_RST}")
    print(f"{_CYN}│{_RST}  {_GRN}✓{_RST}  {n_compressed} compressed, {n_skipped} up-to-date, {n_missing} missing")
    print(f"{_CYN}│{_RST}  .gz files are in data/ and will be included in the LittleFS image.")
    print(f"{_CYN}│{_RST}  After upload, open serial monitor — firmware logs each file")
    print(f"{_CYN}│{_RST}  served as [gz] or [plain] so you can confirm compression is active.")
    print(f"{_BLD}{_CYN}└────────────────────────────────────────────────────────────┘{_RST}")
    print()


env.AddPreAction("buildfs",  compress_assets)
env.AddPreAction("uploadfs", compress_assets)
