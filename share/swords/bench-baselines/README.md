# IBD read-path bench baselines

JSON baselines captured by `contrib/swords/capture-baseline.sh` (or `just baseline`).

Schema: host, git SHA, capture timestamp, and per-benchmark median nanoseconds from
`bench_bitcoin -priority-level=high -output-json`. Optional `segment_replay` from
`parse-reindex-log.py --json` (live IBD segment metrics).

## Compare exit codes (`just baseline-compare` / `baseline_report.py compare`)

| Code | Meaning |
|------|---------|
| 0 | All checks OK |
| 1 | WARN: segment or benchstats drift (|delta| > 10%), missing segment/benchstats field, or bench |delta| > 10% |
| 2 | FAIL: any bench median slower than baseline by > 5% |

Bench medians use FAIL at >5% regression (slower) and WARN at |delta| > 10%.
`segment_replay` fields (`blocks_per_hr`, `p50_load_ms`, `p50_connect_ms`) WARN at |delta| > 10%.

When a baseline includes `segment_replay.benchstats_summary` (from expanded
`parse-reindex-log.py`), compare also checks `p50_disk_per_blk_ms`,
`p50_connect_cs_per_blk_ms`, `p50_flush_lmdb_per_blk_ms`, and `implied_blk_per_s`
with the same WARN rule (exit 1). This is an expansion vs the original shell-only
segment compare. `rollup_count` is recorded in `show` output but not compared.