#!/bin/sh
# usage: rs.sh OUTNAME [run_semantics.py args]
# Runs tests/arm/semantics/run_semantics.py (--arch a64) with the report in out/scratch/OUTNAME and prints the
# summary and the first line of each failing mnemonic (fails.py). Needs out/venv (a link to the venv
# of the main checkout) and out/bin/dismount_cli (see b.sh).
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../../../.."
cd "$root" || exit 1
name=$1
shift
mkdir -p out/scratch
out/venv/bin/python tests/arm/semantics/run_semantics.py --arch a64 --out out/scratch/$name "$@" 2>&1 | grep -E "^  (PASS|FAIL)|states in|error|Error|Traceback" | cut -c1-250
python3 tests/arm/tools/sve_dev/fails.py out/scratch/$name/report.json
