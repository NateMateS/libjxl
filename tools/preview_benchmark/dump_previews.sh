#!/usr/bin/env bash
# Copyright (c) the JPEG XL Project Authors. All rights reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file.
#
# Convert every .jxl in a directory to PNG at preview downsampling factors
# 2, 4, and 8 using `preview_bench --save-previews` (to PGM/PPM/PAM if
# preview_bench was built without PNG support).
#
# With --iterations=0 the tool skips timing and just decodes+saves, so this
# script is fast even over large image sets.
#
# Exits with a non-zero status if any preview could not be saved, or if
# preview_bench failed.
#
# Run from the libjxl repo root:
#   ./tools/preview_benchmark/dump_previews.sh
#   ./tools/preview_benchmark/dump_previews.sh --factors 1,2,4,8
#   ./tools/preview_benchmark/dump_previews.sh --input-dir test_images \
#       --tool ./build/tools/preview_benchmark/preview_bench

set -euo pipefail

INPUT_DIR="test_images"
OUTPUT_DIR="test_images/previews"
FACTORS="2,4,8"
TOOL=""

usage() {
  echo "Usage: $0 [--input-dir DIR] [--output-dir DIR] [--factors F,F,...] [--tool PATH]"
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --input-dir)  INPUT_DIR="$2";  shift 2 ;;
    --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
    --factors)    FACTORS="$2";    shift 2 ;;
    --tool)       TOOL="$2";       shift 2 ;;
    *) usage ;;
  esac
done

# ---------------------------------------------------------------------------
# Locate preview_bench
# ---------------------------------------------------------------------------
resolve_tool() {
  local candidates=(
    "$TOOL"
    "build/tools/preview_benchmark/preview_bench"
  )
  for c in "${candidates[@]}"; do
    [[ -n "$c" && -x "$c" ]] && { echo "$c"; return; }
  done
  echo "ERROR: preview_bench not found. Build it first:" >&2
  echo "  cmake --build <builddir> --target preview_bench" >&2
  echo "or pass --tool <path>." >&2
  exit 1
}

TOOL=$(resolve_tool)
echo "Using preview_bench: $TOOL"

# ---------------------------------------------------------------------------
# Collect inputs
# ---------------------------------------------------------------------------
[[ -d "$INPUT_DIR" ]] || { echo "ERROR: input directory not found: $INPUT_DIR" >&2; exit 1; }
mapfile -t INPUTS < <(find "$INPUT_DIR" -maxdepth 1 -name '*.jxl' -type f | sort)
[[ ${#INPUTS[@]} -gt 0 ]] || { echo "ERROR: no .jxl files in '$INPUT_DIR'" >&2; exit 1; }
echo "Found ${#INPUTS[@]} .jxl input(s)."

mkdir -p "$OUTPUT_DIR"

# ---------------------------------------------------------------------------
# CSV accumulator (Factor,File,Out,Width,Height,Channels,Backend,Status)
# ---------------------------------------------------------------------------
CSV="$OUTPUT_DIR/summary.csv"
echo "Factor,File,Out,Width,Height,Channels,Backend,Status" > "$CSV"

# ---------------------------------------------------------------------------
# Per-factor loop
# ---------------------------------------------------------------------------

# preview_bench prints one line per input (no literal ) in bracket
# expressions):
# "[ OK ] dsN <file> -> <outpath>  (WxH, ch=C, backend=NAME)" to stdout
# "[FAIL] dsN <file>: <stage>: <error>" to stderr
OK_RE='^\[ OK \] ds([0-9]+) (.+) -> (.+)  \(([0-9]+)x([0-9]+), ch=([0-9]+), backend=([^)]+)\)$'
FAIL_RE='^\[FAIL\] ds([0-9]+) (.+): (decode|encode|write): (.*)$'

IFS=',' read -ra FACTOR_LIST <<< "$FACTORS"

declare -A OK_COUNT FAIL_COUNT
status=0

for FACTOR in "${FACTOR_LIST[@]}"; do
  DST="$OUTPUT_DIR/ds${FACTOR}"
  mkdir -p "$DST"
  echo ""
  echo "=== preview_downsampling = ${FACTOR} ==="

  SECONDS=0
  ok=0; fail=0

  # Run preview_bench; capture combined stdout+stderr. A non-zero exit status
  # is expected when some inputs fail, which their [FAIL] lines report.
  tool_status=0
  output=$("$TOOL" \
    "--preview_downsampling=${FACTOR}" \
    "--iterations=0" \
    "--save-previews=${DST}" \
    "${INPUTS[@]}" 2>&1) || tool_status=$?

  while IFS= read -r line; do
    # A Windows build prints CRLF line ends.
    line=${line%$'\r'}
    if [[ "$line" =~ $OK_RE ]]; then
      echo "  $line"
      ds="${BASH_REMATCH[1]}"
      f="${BASH_REMATCH[2]}"
      out="${BASH_REMATCH[3]}"
      w="${BASH_REMATCH[4]}"
      h="${BASH_REMATCH[5]}"
      ch="${BASH_REMATCH[6]}"
      be="${BASH_REMATCH[7]}"
      echo "${ds},${f},${out},${w},${h},${ch},${be},OK" >> "$CSV"
      ok=$((ok + 1))
    elif [[ "$line" =~ $FAIL_RE ]]; then
      echo "  $line" >&2
      echo "${BASH_REMATCH[1]},${BASH_REMATCH[2]},<failed>,0,0,0,,FAIL" >> "$CSV"
      fail=$((fail + 1))
    elif [[ -n "$line" ]]; then
      echo "  $line"
    fi
  done <<< "$output"

  echo "  -> ${ok} OK, ${fail} failed in ${SECONDS}s"
  OK_COUNT[$FACTOR]=$ok
  FAIL_COUNT[$FACTOR]=$fail
  if (( fail > 0 )); then
    status=1
  fi
  if (( tool_status != 0 && fail == 0 )); then
    # preview_bench failed without reporting a failed input.
    echo "ERROR: preview_bench exited with status ${tool_status}" >&2
    status=1
  fi
  if (( ok + fail != ${#INPUTS[@]} )); then
    echo "ERROR: ${#INPUTS[@]} input(s), but ${ok} OK and ${fail} failed" \
      "lines were recognized" >&2
    status=1
  fi
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "Summary CSV: $CSV"
echo ""

# Per-factor OK/total counts
echo "Results by factor:"
for FACTOR in "${FACTOR_LIST[@]}"; do
  total=$(( ${OK_COUNT[$FACTOR]} + ${FAIL_COUNT[$FACTOR]} ))
  echo "  ds${FACTOR}: ${OK_COUNT[$FACTOR]}/${total} OK"
done

# Backend distribution
echo ""
echo "Backend distribution:"
for FACTOR in "${FACTOR_LIST[@]}"; do
  echo "  ds${FACTOR}:"
  awk -F',' -v factor="$FACTOR" '$1 == factor && $NF == "OK" {print $7}' "$CSV" \
    | sort | uniq -c | sort -rn \
    | awk '{printf "    %-26s %s\n", $2, $1}'
done

exit "$status"
