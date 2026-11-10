#!/usr/bin/env python3
"""
Checks of the architecture rules in CLAUDE.md that a compiler alone does not
enforce. Run by CI; both work locally too.

  check_architecture.py headers
      Rule 1: every header outside the Arduino adapters compiles on its own
      with a host compiler, without Arduino. A new header is checked by
      default; only the adapters below are exempt.

  check_architecture.py symbols FIRMWARE.ELF [--absent A,B] [--present C,D]
      Rule 3: the classes in --absent are not linked into the firmware, those
      in --present are (so the check cannot pass by looking at the wrong
      file). A class counts as linked if one of its symbols has an address of
      its own: the linker folds identical functions, and a folded one is
      only an alias of a class that is linked anyway.

  check_architecture.py size FIRMWARE.ELF [--label TEXT]
      Prints the size of the image (firmware.bin next to the ELF file) and of
      the static RAM, and adds them to the GitHub step summary in Actions.
"""

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The Arduino adapters and the facade: their headers name Arduino or ESP-IDF
# types. Everything else must stay host-compilable.
ARDUINO_HEADERS = {
    "include/iot.h",
    "include/iot_ap.h",
    "include/iot_esp_ports.h",
    "src/iot_ota_internal.h",
}


def _arduinojson_include():
    candidates = sorted(glob.glob(os.path.join(ROOT, ".pio", "libdeps", "*", "ArduinoJson", "src")))
    if not candidates:
        sys.exit("ArduinoJson not found below .pio/libdeps - run `pio test -e native` first")
    return candidates[0]


def check_headers(_args):
    compiler = os.environ.get("CXX", "g++")
    json_include = _arduinojson_include()
    headers = sorted(glob.glob(os.path.join(ROOT, "include", "*.h"))
                     + glob.glob(os.path.join(ROOT, "src", "*.h")))
    failed = []
    for path in headers:
        rel = os.path.relpath(path, ROOT)
        if rel in ARDUINO_HEADERS:
            continue
        result = subprocess.run(
            [compiler, "-std=gnu++20", "-fsyntax-only", "-I", "include", "-I", "src",
             "-I", json_include, "-x", "c++", rel],
            cwd=ROOT, capture_output=True, text=True)
        print(("ok    " if result.returncode == 0 else "FAIL  ") + rel)
        if result.returncode != 0:
            failed.append(rel)
            print(result.stderr)
    if failed:
        sys.exit(f"{len(failed)} header(s) do not compile without Arduino; see CLAUDE.md, rule 1. "
                 "If one is an Arduino adapter, add it to ARDUINO_HEADERS.")


def _tool(name):
    """The host binutils read the symbol table of any ELF file."""
    if not shutil.which(name):
        sys.exit(f"{name} not found")
    return name


def _linked_classes(elf):
    """@return {class name: number of symbols with an address of their own}."""
    output = subprocess.run([_tool("nm"), "-C", elf], capture_output=True, text=True, check=True).stdout
    by_address = {}
    for line in output.splitlines():
        parts = line.split(" ", 2)
        if len(parts) != 3:
            continue
        address, _, name = parts
        match = re.match(r"(?:vtable for |typeinfo for |typeinfo name for )?(Iot\w+)::", name)
        if match:
            by_address.setdefault(address, set()).add(match.group(1))
    linked = {}
    for classes in by_address.values():
        if len(classes) == 1:
            name = next(iter(classes))
            linked[name] = linked.get(name, 0) + 1
    return linked


def check_symbols(args):
    linked = _linked_classes(args.elf)
    errors = []
    for name in filter(None, args.absent.split(",")):
        if name in linked:
            errors.append(f"{name} is linked ({linked[name]} symbols) but must not be")
        else:
            print(f"ok    {name} not linked")
    for name in filter(None, args.present.split(",")):
        if name not in linked:
            errors.append(f"{name} is not linked but should be")
        else:
            print(f"ok    {name} linked ({linked[name]} symbols)")
    if errors:
        sys.exit("\n".join(errors) + "\nSee CLAUDE.md, rule 3: the core never names an optional adapter.")


def report_size(args):
    image = os.path.join(os.path.dirname(args.elf), "firmware.bin")
    output = subprocess.run([_tool("size"), "-A", args.elf], capture_output=True, text=True,
                            check=True).stdout
    ram = 0
    for line in output.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[1].isdigit() and parts[0].startswith((".dram0.data", ".dram0.bss")):
            ram += int(parts[1])
    line = f"{args.label}: image {os.path.getsize(image):,} bytes, static RAM {ram:,} bytes"
    print(line)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as out:
            out.write(f"- {line}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("headers").set_defaults(run=check_headers)
    symbols = sub.add_parser("symbols")
    symbols.add_argument("elf")
    symbols.add_argument("--absent", default="")
    symbols.add_argument("--present", default="")
    symbols.set_defaults(run=check_symbols)
    sizes = sub.add_parser("size")
    sizes.add_argument("elf")
    sizes.add_argument("--label", default="firmware")
    sizes.set_defaults(run=report_size)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
