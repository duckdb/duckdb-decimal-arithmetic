#!/usr/bin/env bash
# bench_div.sh — compare decimal_div (extension) vs built-in DOUBLE division
#
# Usage: ./scripts/bench_div.sh [rows]
#   rows: number of rows per run (default 1 000 000)

set -euo pipefail

ROWS=${1:-1000000}
DUCKDB=./build/release/duckdb
EXT=$(realpath ./build/release/extension/decimal_arithmetic/decimal_arithmetic.duckdb_extension)
RUNS=5
WARMUP=1

if [[ ! -x "$DUCKDB" ]]; then
  echo "ERROR: DuckDB binary not found at $DUCKDB" >&2; exit 1
fi
if [[ ! -f "$EXT" ]]; then
  echo "ERROR: Extension not found at $EXT" >&2; exit 1
fi

# Use a temp file database so the bench table persists across invocations.
TMPDB=$(mktemp /tmp/bench_div_XXXXXX.duckdb)
rm -f "$TMPDB"   # DuckDB must create the file itself; mktemp just reserves the name
trap 'rm -f "$TMPDB"' EXIT

now_ms() { python3 -c "import time; print(int(time.time()*1000))"; }

db() { "$DUCKDB" -unsigned -noheader -list "$TMPDB" "$@"; }

# --------------------------------------------------------------------------
echo "======================================================"
echo " DuckDB decimal division benchmark"
echo " rows=$ROWS  runs=$RUNS  warmup=$WARMUP"
echo "======================================================"

echo ""
echo "Generating $ROWS rows..."
db -c "
LOAD '$EXT';
CREATE OR REPLACE TABLE bench AS
  SELECT
    (random() * 1e11 + 1)::DECIMAL(18,6) AS a,
    (random() * 1e5  + 1)::DECIMAL(18,6) AS b
  FROM range($ROWS);
"
echo "Done."

# --------------------------------------------------------------------------
run_variant() {
  local label="$1"
  local query="$2"
  local times=()

  echo ""
  echo "[$label]"

  for ((i = 1; i <= WARMUP + RUNS; i++)); do
    t0=$(now_ms)
    db -c "$query" > /dev/null
    t1=$(now_ms)
    elapsed=$(( t1 - t0 ))

    if (( i <= WARMUP )); then
      echo "  warm-up: ${elapsed} ms"
    else
      run=$(( i - WARMUP ))
      echo "  run ${run}: ${elapsed} ms"
      times+=("$elapsed")
    fi
  done

  local total=0
  for t in "${times[@]}"; do (( total += t )); done
  LAST_AVG=$(( total / RUNS ))
  echo "  average: ${LAST_AVG} ms"
}

# --------------------------------------------------------------------------
run_variant \
  "decimal_div  (extension — banker's rounding, hugeint arithmetic)" \
  "LOAD '$EXT'; SELECT sum(decimal_div(a, b)) FROM bench;"
AVG_EXT=$LAST_AVG

run_variant \
  "a / b        (built-in — DOUBLE arithmetic)" \
  "SELECT sum(a::DOUBLE / b::DOUBLE) FROM bench;"
AVG_BUILTIN=$LAST_AVG

# --------------------------------------------------------------------------
echo ""
echo "======================================================"
echo " Summary ($ROWS rows)"
echo "======================================================"
printf "  decimal_div  : %5d ms avg\n" "$AVG_EXT"
printf "  DOUBLE /     : %5d ms avg\n" "$AVG_BUILTIN"

if (( AVG_EXT > 0 && AVG_BUILTIN > 0 )); then
  if (( AVG_EXT >= AVG_BUILTIN )); then
    ratio=$(awk "BEGIN { printf \"%.1fx\", $AVG_EXT / $AVG_BUILTIN }")
    echo "  decimal_div is $ratio slower"
  else
    ratio=$(awk "BEGIN { printf \"%.1fx\", $AVG_BUILTIN / $AVG_EXT }")
    echo "  decimal_div is $ratio faster"
  fi
fi
echo ""