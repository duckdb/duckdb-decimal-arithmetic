# DecimalArithmetic

This repository is based on https://github.com/duckdb/extension-template, check it out if you want to build and ship your own DuckDB extension.

Adds support for the following functions:

### `decimal_div(a DECIMAL, b DECIMAL) -> DECIMAL`

Scalar division on `DECIMAL` that returns a `DECIMAL` result instead of casting to `DOUBLE`.
Precision and scale follow SQL Server semantics. For `a / b` with input types `DECIMAL(p1, s1)` and `DECIMAL(p2, s2)`:

- Result scale: `max(6, s1 + p2 + 1)`
- Result precision: `p1 - s1 + s2 + result_scale`

Both are capped at 38. If the computed precision exceeds 38, the scale is reduced by the excess (floored at 6) and precision is set to 38.

### `decimal_avg(x DECIMAL) -> DECIMAL`

Aggregate average on `DECIMAL` that returns a `DECIMAL` result instead of casting to `DOUBLE`.
Uses banker's rounding (round-half-to-even) when the exact average is not representable at the input scale.

- Result scale: same as the input scale
- Result precision: 38

### Rounding functions

All rounding functions take `(value DECIMAL, scale INTEGER)` and return `DECIMAL` with the given scale.
The `scale` argument must be a constant and cannot exceed the input scale (use an explicit cast to a higher-precision `DECIMAL` first if needed).

| Function | Rounding mode |
|---|---|
| `round_ceil(x, s)` | Towards +infinity (ceiling) |
| `round_floor(x, s)` | Towards -infinity (floor) |
| `round_up(x, s)` | Away from zero |
| `round_down(x, s)` | Towards zero (truncate) |

### Benchmarks
To reproduce: `./scripts/bench_div [number of rows]`
```
======================================================
DuckDB decimal division benchmark
runs=5  warmup=1
======================================================
 Summary (50_000 rows)
======================================================
  decimal_div  :    42 ms avg
  DOUBLE /     :    33 ms avg
  decimal_div is 1.3x slower
======================================================
 Summary (500_000 rows)
======================================================
  decimal_div  :    60 ms avg
  DOUBLE /     :    35 ms avg
  decimal_div is 1.7x slower
======================================================
Summary (5_000_000 rows)
======================================================
decimal_div  :   214 ms avg
DOUBLE /     :    45 ms avg
decimal_div is 4.8x slower
```
