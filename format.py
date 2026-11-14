#!/usr/bin/env python3
"""Cross-platform C++ formatter using the project-local clang-format first, then PATH.

Only src and tests C++ files are processed. --check is read-only; failures return nonzero.
The script neither installs tools nor changes application behavior or log formats."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parent


def main():
    """Resolve source/config paths relative to this script, independent of the working directory."""
    parser = argparse.ArgumentParser(description="Format or check the project's C++ sources.")
    parser.add_argument("--check", action="store_true", help="Check without changing files")
    parser.add_argument("--formatter", help="Explicit clang-format executable")
    args = parser.parse_args()
    suffix = ".exe" if os.name == "nt" else ""
    local = ROOT / "build/format-tools/clang_format/data/bin" / f"clang-format{suffix}"
    formatter = args.formatter or (str(local) if local.is_file() else shutil.which("clang-format"))
    if not formatter:
        parser.error("Install clang-format==23.1.3 or pass --formatter PATH")
    sources = sorted(path for directory in ("src", "tests")
                     for path in (ROOT / directory).rglob("*")
                     if path.suffix in (".cpp", ".hpp", ".h"))
    if not sources:
        raise RuntimeError("No C++ sources found")
    options = ["--dry-run", "--Werror"] if args.check else ["-i"]
    subprocess.run([formatter, "--style=file", *options, *map(str, sources)], cwd=ROOT, check=True)
    print(f"{'Checked' if args.check else 'Formatted'} {len(sources)} C++ files")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
