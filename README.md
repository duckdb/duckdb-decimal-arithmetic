# DecimalArithmetic

This repository is based on https://github.com/duckdb/extension-template, check it out if you want to build and ship your own DuckDB extension.

Adds support for the following scalar functions: 
- `decimal_div`: Allows division on `DECIMAL` data type and return `DECIMAL`
  Behaviour is similar to SQL Server. For s1 / s2 the precision and scale are calculated as follows: 
    - Result precision: p1 - s1 + s2 + max(6, s1 + p2 + 1)
    - Result scale: max(6, s1 + p2 + 1)

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
