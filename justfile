# Bitcoin Swords dev shortcuts. Run `just` to list recipes.

root := justfile_directory()
build := root / "build"
build_tsan := root / "build-tsan"
bindir := build / "bin"
bindir_tsan := build_tsan / "bin"
# Parallel cmake jobs (default all cores; just check forces SWORDS_JOBS=4)
jobs := env_var_or_default("SWORDS_JOBS", num_cpus())
datadir := env_var_or_default("SWORDS_DATADIR", env_var("HOME") + "/.bitcoin-swords")
# Profile logs live OUTSIDE the datadir so reset-datadir never touches them.
profile_logs := env_var_or_default("SWORDS_LOG_ARCHIVE", env_var("HOME") + "/.bitcoin-swords-profile-logs")
# Offline verify/check transcripts + functional test datadirs (stable; not /tmp).
check_logs_root := profile_logs / "check-runs"
functional_logs_root := profile_logs / "functional-tests"
# Keep in sync with benchstats_parse.DEFAULT_MILESTONES via default_milestones_arg().
default_milestones := `python3 contrib/swords/benchstats_parse.py default-milestones`

bitcoind := bindir / "bitcoind"
test_bitcoin := bindir / "test_bitcoin"
test_bitcoin_tsan := bindir_tsan / "test_bitcoin"
bitcoin_cli := bindir / "bitcoin-cli"
tsan_suppressions := root / "test" / "sanitizer_suppressions" / "tsan"

flush_tests := "cs_main_locking_tests,chainstate_write_tests,txdb_sync_tests,txdb_parallel_encode_tests,validation_flush_tests,dbwrapper_tests"
# IBD read-path unit tests (variable name phase_d_tests is historical; recipe: test-phase-d)
phase_d_tests := "validation_segment_equivalence_tests,block_decompress_parallel_tests,block_prefetch_queue_tests,cs_main_locking_tests,txdb_prefetch_tests,coins_prevouts_tests,blockmanager_tests,zstd_tests"
bench_bitcoin := bindir / "bench_bitcoin"

[doc("List available recipes")]
default:
    @just --list

[doc("CMake configure (first time or after deleting build/)")]
configure:
    cmake -B {{build}} \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DBUILD_GUI=OFF \
        -DWITH_ZMQ=OFF \
        -DRDTS_CONSENT=IMPLICIT

[doc("Build bitcoind and test_bitcoin")]
build: build-daemon build-tests

[doc("Build bitcoind only")]
build-daemon:
    cmake --build {{build}} --target bitcoind -j{{jobs}}

[doc("Build test_bitcoin only")]
build-tests:
    cmake --build {{build}} --target test_bitcoin -j{{jobs}}

[doc("CMake configure with ThreadSanitizer (separate build-tsan/ tree)")]
configure-tsan:
    cmake -B {{build_tsan}} \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DBUILD_GUI=OFF \
        -DWITH_ZMQ=OFF \
        -DRDTS_CONSENT=IMPLICIT \
        -DSANITIZERS=thread

[doc("Build test_bitcoin in build-tsan/ (run configure-tsan first)")]
build-tsan:
    cmake --build {{build_tsan}} --target test_bitcoin -j{{jobs}}

[doc("Build bitcoin-cli")]
build-cli:
    cmake --build {{build}} --target bitcoin-cli -j{{jobs}}

[doc("Flush / locking / txdb unit tests")]
test-flush: build-tests
    {{test_bitcoin}} --run_test={{flush_tests}}

[doc("Full unit test suite")]
test: build-tests
    {{test_bitcoin}}

[doc("Run one Boost test case, e.g. `just test-one cs_main_locking_tests`")]
test-one case: build-tests
    {{test_bitcoin}} --run_test={{case}}

[doc("Reindex chainstate only, foreground (isolated from P2P)")]
reindex-chainstate: build-daemon
    {{bitcoind}} -datadir={{datadir}} -reindex-chainstate -connect=0 -printtoconsole

# Back-compat alias
alias reindex := reindex-chainstate

[doc("Stop the node")]
stop:
    {{bitcoin_cli}} -datadir={{datadir}} stop

[doc("CMake configure with BUILD_BENCH=ON")]
configure-bench:
    cmake -B {{build}} \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DBUILD_GUI=OFF \
        -DBUILD_BENCH=ON \
        -DWITH_ZMQ=OFF \
        -DRDTS_CONSENT=IMPLICIT

[doc("Build bench_bitcoin")]
build-bench:
    cmake --build {{build}} --target bench_bitcoin -j{{jobs}}

[doc("Run all HIGH-priority microbenches")]
bench: build-bench
    {{bench_bitcoin}} -priority-level=high

[doc("Run block-read IBD microbenches only")]
bench-block: build-bench
    {{bench_bitcoin}} -priority-level=high -filter='DecompressBlockPayload|ReadRawBlock'

[doc("Run coin-prefetch IBD microbenches only")]
bench-coin: build-bench
    {{bench_bitcoin}} -priority-level=high -filter='PrefetchCoins'

[doc("Capture bench baseline JSON under share/swords/bench-baselines/")]
baseline: build-bench
    bash {{root}}/contrib/swords/capture-baseline.sh

[doc("Compare current benches to latest baseline (or pass BASELINE=path)")]
baseline-compare baseline="": build-bench
    bash {{root}}/contrib/swords/compare-baseline.sh {{baseline}}

[doc("List captured bench baseline JSON files (newest first)")]
list-baselines:
    python3 {{root}}/contrib/swords/baseline_report.py list

[doc("Phase 2 operator gate: test-phase-d then baseline-compare (single bench run, fail fast). Run configure-bench first if bench_bitcoin is missing.")]
phase2:
    #!/usr/bin/env bash
    set -euo pipefail
    just test-phase-d
    just baseline-compare

[doc("Phase 2 capture: test-phase-d then baseline (re-runs benches; hints configure-bench if needed)")]
phase2-capture:
    #!/usr/bin/env bash
    set -euo pipefail
    just test-phase-d
    just baseline

[doc("Baseline capture/compare unit tests")]
test-baseline:
    python3 {{root}}/contrib/swords/test_baseline_lib.py -v

[doc("Parse reindex debug.log (default: SWORDS_DATADIR)")]
parse-log DATADIR=datadir:
    python3 {{root}}/contrib/swords/parse-reindex-log.py {{DATADIR}}

[doc("Parse archived log (default: profile-logs/latest)")]
parse-archived-log ARCHIVE="latest":
    #!/usr/bin/env bash
    set -euo pipefail
    archive_root="{{profile_logs}}"
    target="{{ARCHIVE}}"
    if [[ "$target" == "latest" ]]; then
        if [[ -L "$archive_root/latest" ]]; then
            target="$(readlink -f "$archive_root/latest")"
        elif [[ -f "$archive_root/LATEST.txt" ]]; then
            target="$(cat "$archive_root/LATEST.txt")"
        else
            echo "No archived logs under $archive_root" >&2
            exit 1
        fi
    fi
    log="$target/debug.log"
    [[ -f "$log" ]] || { echo "Missing $log" >&2; exit 1; }
    python3 {{root}}/contrib/swords/parse-reindex-log.py --log "$log"

[doc("List preserved profile logs (SWORDS_LOG_ARCHIVE, default ~/.bitcoin-swords-profile-logs)")]
list-profile-logs:
    #!/usr/bin/env bash
    set -euo pipefail
    archive_root="{{profile_logs}}"
    if [[ ! -d "$archive_root" ]]; then
        echo "No log-archive at $archive_root"
        exit 0
    fi
    echo "Profile logs: $archive_root"
    [[ -f "$archive_root/LATEST.txt" ]] && echo "IBD archive latest: $(cat "$archive_root/LATEST.txt")"
    ls -1dt "$archive_root"/*/ 2>/dev/null | grep -v '/check-runs$' | grep -v '/functional-tests$' | head -20 || true
    echo "--- check runs (just check / verify transcripts) ---"
    just check-logs

[doc("IBD read-path unit tests (equivalence + decompress + locking); recipe name test-phase-d is historical")]
test-phase-d: build-tests
    {{test_bitcoin}} --run_test={{phase_d_tests}}

[doc("Dictionary bootstrap unit tests")]
test-bootstrap: build-tests
    {{test_bitcoin}} --run_test=dict_bootstrap_tests

[doc("Parse-reindex-log.py unit tests")]
test-parse-log:
    python3 {{root}}/contrib/swords/test_parse_reindex_log.py -v

[doc("Benchstats parser unit tests")]
test-parse-benchstats:
    python3 {{root}}/contrib/swords/test_benchstats_parse.py -v

[doc("Compare-benchstats unit tests")]
test-compare-benchstats:
    python3 {{root}}/contrib/swords/test_compare_benchstats.py -v

[doc("Parse benchstats rollups from debug.log (default: SWORDS_DATADIR)")]
parse-benchstats LOG=datadir:
    python3 {{root}}/contrib/swords/parse-benchstats.py {{LOG}}

[doc("Compare current debug.log benchstats vs archived log (default: latest)")]
compare-benchstats ARCHIVE="latest":
    #!/usr/bin/env bash
    set -euo pipefail
    python3 {{root}}/contrib/swords/compare-benchstats.py --archive "{{ARCHIVE}}"

[doc("Phase 3 unified benchstats report (current debug.log)")]
phase3:
    python3 {{root}}/contrib/swords/phase3_report.py

[doc("Phase 3 report as JSON")]
phase3-json:
    python3 {{root}}/contrib/swords/phase3_report.py --json

[doc("Export benchstats JSON+CSV from current debug.log (OUT= or profile_logs/<stamp>/)")]
export-benchstats OUT="":
    #!/usr/bin/env bash
    set -euo pipefail
    datadir="{{datadir}}"
    log="$datadir/debug.log"
    [[ -f "$log" ]] || { echo "Missing $log" >&2; exit 1; }
    out="{{OUT}}"
    if [[ -z "$out" ]]; then
        stamp="$(date -u +%Y%m%dT%H%M%SZ)"
        out="{{profile_logs}}/$stamp"
    fi
    mkdir -p "$out"
    python3 {{root}}/contrib/swords/parse-benchstats.py --log "$log" --json > "$out/benchstats.json"
    OUT="$out/benchstats.csv" python3 {{root}}/contrib/swords/parse-benchstats.py --log "$log" --csv
    echo "Exported benchstats -> $out"

[doc("Phase 3 report unit tests")]
test-phase3:
    python3 {{root}}/contrib/swords/test_phase3_report.py -v

[doc("Phase 0 live health profiling unit tests (includes phase0-check CLI/shell gates)")]
test-phase0:
    python3 {{root}}/contrib/swords/test_phase0_live_health.py -v

[doc("Alias for test-phase0 (check CLI covered by test_phase0_live_health.py)")]
test-phase0-check: test-phase0

[doc("Phase 0: loop pidstat + block height during IBD (Ctrl+C or DURATION= to stop)")]
watch-cpu INTERVAL="10" DURATION="":
    #!/usr/bin/env bash
    set -euo pipefail
    export DATADIR="{{datadir}}"
    export PROFILE_LOGS="{{profile_logs}}"
    export BITCOIN_CLI="{{bitcoin_cli}}"
    export INTERVAL="{{INTERVAL}}"
    export DURATION="{{DURATION}}"
    exec bash {{root}}/contrib/swords/phase0-live-health.sh watch

[doc("Phase 0: perf record -g on bitcoind during IBD (default 60s)")]
profile-ibd DURATION="60":
    #!/usr/bin/env bash
    set -euo pipefail
    export DATADIR="{{datadir}}"
    export PROFILE_LOGS="{{profile_logs}}"
    export BITCOIN_CLI="{{bitcoin_cli}}"
    export DURATION="{{DURATION}}"
    exec bash {{root}}/contrib/swords/phase0-live-health.sh profile

[doc("Phase 0: benchstats health check (exit 1 on readers_full / MDB_READERS_FULL)")]
phase0-check TXINDEX_WARN_MS="500" PREFETCH_ROLLUPS="3":
    #!/usr/bin/env bash
    set -euo pipefail
    export DATADIR="{{datadir}}"
    export PROFILE_LOGS="{{profile_logs}}"
    exec python3 {{root}}/contrib/swords/phase0_live_health.py check \
        --datadir "{{datadir}}" \
        --txindex-warn-ms "{{TXINDEX_WARN_MS}}" \
        --prefetch-rollups "{{PREFETCH_ROLLUPS}}"

[doc("Phase 0: one-shot pidstat/mpstat/iostat snapshot")]
phase0-snapshot:
    #!/usr/bin/env bash
    set -euo pipefail
    export DATADIR="{{datadir}}"
    export PROFILE_LOGS="{{profile_logs}}"
    export BITCOIN_CLI="{{bitcoin_cli}}"
    exec bash {{root}}/contrib/swords/phase0-live-health.sh snapshot

[doc("Phase 0 live health profiling usage")]
phase0:
    bash {{root}}/contrib/swords/phase0-live-health.sh help

[doc("Phase 5 USDT/kernel tracing unit tests")]
test-phase5:
    python3 {{root}}/contrib/swords/test_phase5_usdt.py -v

[doc("Phase 5: verify bitcoind has USDT tracepoints (running node or BITCOIND)")]
check-usdt:
    #!/usr/bin/env bash
    set -euo pipefail
    export DATADIR="{{datadir}}"
    export PROFILE_LOGS="{{profile_logs}}"
    export BITCOIND="{{bitcoind}}"
    export BITCOIN_CLI="{{bitcoin_cli}}"
    exec bash {{root}}/contrib/swords/phase5-usdt.sh check-usdt

[doc("Phase 5: bpftrace connectblock latency during IBD (sudo -E just profile-connectblock)")]
profile-connectblock THRESHOLD_MS="25" START="" END="0":
    #!/usr/bin/env bash
    set -euo pipefail
    export DATADIR="{{datadir}}"
    export PROFILE_LOGS="{{profile_logs}}"
    export BITCOIND="{{bitcoind}}"
    export BITCOIN_CLI="{{bitcoin_cli}}"
    export THRESHOLD_MS="{{THRESHOLD_MS}}"
    export START="{{START}}"
    export END="{{END}}"
    exec bash {{root}}/contrib/swords/phase5-usdt.sh connectblock

[doc("Phase 5: BCC utxocache flush logger during IBD (sudo -E just profile-utxo-flush)")]
profile-utxo-flush DURATION="60":
    #!/usr/bin/env bash
    set -euo pipefail
    export DATADIR="{{datadir}}"
    export PROFILE_LOGS="{{profile_logs}}"
    export BITCOIND="{{bitcoind}}"
    export BITCOIN_CLI="{{bitcoin_cli}}"
    export DURATION="{{DURATION}}"
    exec bash {{root}}/contrib/swords/phase5-usdt.sh utxo-flush

[doc("Phase 5 USDT/kernel tracing usage")]
phase5:
    bash {{root}}/contrib/swords/phase5-usdt.sh help

[doc("Phase 6 parameter sweep plan")]
phase6:
    python3 {{root}}/contrib/swords/phase6_sweep.py plan --profile-logs {{profile_logs}}

[doc("Phase 6 sweep state (human-readable)")]
sweep-status:
    python3 {{root}}/contrib/swords/phase6_sweep.py status --profile-logs {{profile_logs}}

[doc("Phase 6 sweep campaign gates + next recommended variant")]
sweep-campaign:
    python3 {{root}}/contrib/swords/phase6_sweep.py campaign --profile-logs {{profile_logs}}

[doc("Record completed sweep variant (default ARCHIVE=latest)")]
sweep-record VARIANT ARCHIVE="latest":
    python3 {{root}}/contrib/swords/phase6_sweep.py record --variant {{VARIANT}} --archive {{ARCHIVE}} --profile-logs {{profile_logs}}

[doc("Compare milestone matrix across recorded sweep variants")]
sweep-compare MILESTONES=default_milestones:
    python3 {{root}}/contrib/swords/phase6_sweep.py compare --milestones {{MILESTONES}} --profile-logs {{profile_logs}}

[doc("Emit bitcoin.conf overlay + start args for a sweep variant")]
sweep-conf VARIANT:
    python3 {{root}}/contrib/swords/phase6_sweep.py conf --variant {{VARIANT}} --profile-logs {{profile_logs}} --datadir {{datadir}}

[doc("Write merged sweep variant settings into datadir/bitcoin.conf")]
sweep-apply VARIANT:
    python3 {{root}}/contrib/swords/phase6_sweep.py conf --variant {{VARIANT}} --profile-logs {{profile_logs}} --datadir {{datadir}} --out {{datadir}}/bitcoin.conf

[doc("Print bitcoind CLI args for sweep variant")]
sweep-args VARIANT:
    python3 {{root}}/contrib/swords/phase6_sweep.py args --variant {{VARIANT}} --profile-logs {{profile_logs}}

[doc("Start bitcoind with sweep variant CLI args")]
sweep-start VARIANT:
    #!/usr/bin/env bash
    set -euo pipefail
    daemon="{{bitcoind}}"
    [[ -x "$daemon" ]] || { echo "bitcoind not found at $daemon — run: just build-daemon"; exit 1; }
    mapfile -t sweep_args < <(python3 {{root}}/contrib/swords/phase6_sweep.py args --variant {{VARIANT}} --profile-logs {{profile_logs}})
    exec "$daemon" -datadir="{{datadir}}" -printtoconsole "${sweep_args[@]}"

[doc("Post-IBD reset (archive debug.log) + record sweep variant")]
sweep-finish VARIANT ARCHIVE="latest":
    #!/usr/bin/env bash
    set -euo pipefail
    export SWORDS_SWEEP_VARIANT="{{VARIANT}}"
    just reset-datadir
    just sweep-record {{VARIANT}} {{ARCHIVE}}

[doc("Phase 6 parameter sweep unit tests")]
test-phase6:
    python3 {{root}}/contrib/swords/test_phase6_sweep.py -v

[doc("DEBUG_ONLY promotion gate unit tests")]
test-promotion-gates:
    python3 {{root}}/contrib/swords/test_promotion_gates.py -v

[doc("Sweep campaign exit gates before DEBUG_ONLY flag promotion (exit 1 if premature)")]
promotion-gates:
    python3 {{root}}/contrib/swords/promotion_gates.py --profile-logs {{profile_logs}}

[doc("ThreadSanitizer locking gates (cs_main + chainstate manager + txdb prefetch)")]
test-tsan: configure-tsan build-tsan
    #!/usr/bin/env bash
    set -euo pipefail
    export TSAN_OPTIONS="suppressions={{tsan_suppressions}}:halt_on_error=1:second_deadlock_stack=1"
    {{test_bitcoin_tsan}} --run_test=cs_main_locking_tests
    {{test_bitcoin_tsan}} --run_test=validation_chainstatemanager_tests
    {{test_bitcoin_tsan}} --run_test=txdb_prefetch_tests

[doc("TSan build+run for just check (uses SWORDS_JOBS, default 4; skips reconfigure if build-tsan exists)")]
check-tsan:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -f {{build_tsan}}/CMakeCache.txt ]]; then
        just configure-tsan
    fi
    cmake --build {{build_tsan}} --target test_bitcoin -j{{jobs}}
    export TSAN_OPTIONS="suppressions={{tsan_suppressions}}:halt_on_error=1:second_deadlock_stack=1"
    echo "=== TSan: cs_main_locking_tests ==="
    {{test_bitcoin_tsan}} --run_test=cs_main_locking_tests
    echo "=== TSan: validation_chainstatemanager_tests ==="
    {{test_bitcoin_tsan}} --run_test=validation_chainstatemanager_tests
    echo "=== TSan: txdb_prefetch_tests ==="
    {{test_bitcoin_tsan}} --run_test=txdb_prefetch_tests

[doc("Functional 4-pack (assumeutxo, dbcrash, coinstatsindex, index_prune); logs under profile_logs/functional-tests/ or SWORDS_FUNCTIONAL_TMPDIR")]
test-functional:
    #!/usr/bin/env bash
    set -euo pipefail
    func_root="${SWORDS_FUNCTIONAL_TMPDIR:-{{functional_logs_root}}}"
    mkdir -p "$func_root"
    # Avoid /tmp — full tmpfs causes LMDB sync + debug.log writes to hang mid-flush.
    export TMPDIR="$func_root"
    avail_kb="$(df -Pk "$func_root" | awk 'NR==2 {print $4}')"
    min_kb=$((2 * 1024 * 1024))  # 2 GiB
    if [[ "$avail_kb" -lt "$min_kb" ]]; then
        echo "ERROR: need >=2GiB free under $func_root for functional tests (have $((avail_kb / 1024))MiB)" >&2
        exit 1
    fi
    stamp="$(date -u +%Y%m%d_%H%M%S)"
    results="$func_root/results_$stamp.csv"
    echo "Functional test logs: $func_root (results -> $results)"
    echo "TMPDIR=$TMPDIR (sequential -j1 to avoid LMDB contention with dbcrash)"
    {{root}}/test/functional/test_runner.py --combinedlogslen=4000 --jobs 1 \
        --tmpdirprefix "$func_root" --resultsfile "$results" \
        feature_assumeutxo.py feature_dbcrash.py \
        feature_coinstatsindex.py feature_index_prune.py
    runner_dir="$(ls -1dt "$func_root"/test_runner_* 2>/dev/null | head -1 || true)"
    if [[ -n "$runner_dir" ]]; then
        ln -sfn "$(basename "$runner_dir")" "$func_root/latest"
        echo "$runner_dir" > "$func_root/LATEST.txt"
    fi

[doc("Show latest check/functional log paths (for agents and post-mortems)")]
check-logs:
    #!/usr/bin/env bash
    set -euo pipefail
    archive="{{check_logs_root}}"
    func="{{functional_logs_root}}"
    echo "Check runs:    $archive"
    if [[ -L "$archive/latest" ]]; then
        echo "  latest -> $(readlink -f "$archive/latest")"
        echo "  transcript: $(readlink -f "$archive/latest")/check.log"
    elif [[ -f "$archive/LATEST.txt" ]]; then
        echo "  latest: $(cat "$archive/LATEST.txt")"
    else
        echo "  (no check runs yet)"
    fi
    echo "Functional:    $func"
    if [[ -L "$func/latest" ]]; then
        echo "  latest -> $(readlink -f "$func/latest")"
    elif [[ -f "$func/LATEST.txt" ]]; then
        echo "  latest: $(cat "$func/LATEST.txt")"
    else
        echo "  (no functional runs yet)"
    fi
    echo "Combine logs:  python3 {{root}}/test/functional/combine_logs.py <test_tmpdir>"
    echo "Profile logs:  just list-profile-logs"

[doc("Offline Swords gates: build + curated C++ tests + Python tool tests + functional 4-pack (no TSan, no flush pack, no live IBD)")]
verify: build test-bootstrap test-phase-d test-parse-log test-parse-benchstats test-compare-benchstats test-phase3 test-baseline test-phase0 test-phase5 test-phase6 test-promotion-gates test-functional

[doc("All offline tiers in one shot: verify + flush/locking pack + TSan (use: just check; live IBD: just check live)")]
check live="":
    #!/usr/bin/env bash
    set -euo pipefail
    set -o pipefail
    export SWORDS_JOBS="${SWORDS_JOBS:-4}"
    stamp="$(date -u +%Y%m%d_%H%M%S)"
    run_dir="{{check_logs_root}}/$stamp"
    mkdir -p "$run_dir/functional"
    ln -sfn "$stamp" "{{check_logs_root}}/latest"
    echo "$run_dir" > "{{check_logs_root}}/LATEST.txt"
    {
        echo "check_run_dir=$run_dir"
        echo "started_at_utc=$stamp"
        echo "swords_jobs=$SWORDS_JOBS"
        echo "git_head=$(git -C {{root}} rev-parse --short HEAD 2>/dev/null || echo unknown)"
        echo "git_branch=$(git -C {{root}} rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
        echo "live_tier={{live}}"
    } > "$run_dir/meta.txt"
    export SWORDS_FUNCTIONAL_TMPDIR="$run_dir/functional"
    {
    echo "=== check: SWORDS_JOBS=$SWORDS_JOBS ==="
    echo "=== check logs: $run_dir/check.log ==="
    echo "=== functional datadirs: $run_dir/functional/ ==="
    echo "=== Tier 1+2: verify (build, phase-d, Python gates, functional 4-pack) ==="
    just verify
    echo "=== Tier 1: flush / locking pack (not in verify) ==="
    just test-flush
    echo "=== Tier 1: ThreadSanitizer gates ==="
    just check-tsan
    if [[ -n "{{live}}" ]]; then
        echo "=== Tier 3: live IBD gates (need syncing node + sweep archives) ==="
        just phase0-check
        just sweep-status
        just promotion-gates
        echo "=== Tier 3: sweep milestone compare ==="
        just sweep-compare
    else
        echo "=== Tier 3 skipped (offline). After IBD sweep: just check live ==="
    fi
    echo "=== check: all requested tiers passed ==="
    echo "finished_at_utc=$(date -u +%Y%m%d_%H%M%S)" >> "$run_dir/meta.txt"
    } 2>&1 | tee "$run_dir/check.log"

[doc("Wipe chain data; keeps bitcoin.conf (SWORDS_DATADIR, default ~/.bitcoin-swords)")]
reset-datadir:
    #!/usr/bin/env bash
    set -euo pipefail
    datadir="{{datadir}}"
    if [[ ! -d "$datadir" ]]; then
        echo "Creating $datadir"
        mkdir -p "$datadir"
        exit 0
    fi
    echo "Stopping node on $datadir (if running)..."
    if pgrep -f "bitcoind.*-datadir=$datadir" >/dev/null 2>&1; then
        for cli in "{{bitcoin_cli}}" "$(command -v bitcoin-cli 2>/dev/null || true)"; do
            [[ -n "$cli" && -x "$cli" ]] || continue
            "$cli" -datadir="$datadir" stop 2>/dev/null && break
        done
        sleep 5
        pkill -TERM -f "bitcoind.*-datadir=$datadir" 2>/dev/null || true
        sleep 1
    fi
    archive_root="{{profile_logs}}"
    mkdir -p "$archive_root"
    chmod 700 "$archive_root" 2>/dev/null || true
    if [[ -f "$datadir/debug.log" ]]; then
        stamp="$(date -u +%Y-%m-%dT%H-%MZ)"
        dest="$archive_root/$stamp"
        mkdir -p "$dest"
        chmod 700 "$dest" 2>/dev/null || true
        cp -a "$datadir/debug.log" "$dest/debug.log"
        {
            echo "archived_at_utc=$stamp"
            echo "source_datadir=$datadir"
            echo "archive_root=$archive_root"
            echo "bytes=$(wc -c < "$dest/debug.log" | tr -d ' ')"
            if [[ -n "${SWORDS_SWEEP_VARIANT:-}" ]]; then
                echo "sweep_variant=$SWORDS_SWEEP_VARIANT"
            fi
        } > "$dest/meta.txt"
        if python3 {{root}}/contrib/swords/parse-reindex-log.py --log "$dest/debug.log" --json \
            > "$dest/summary.json" 2>/dev/null; then
            echo "summary_json=$dest/summary.json" >> "$dest/meta.txt"
        fi
        benchstats_json_err="$(mktemp)"
        if python3 {{root}}/contrib/swords/parse-benchstats.py --log "$dest/debug.log" --json \
            > "$dest/benchstats.json" 2>"$benchstats_json_err"; then
            echo "benchstats_json=$dest/benchstats.json" >> "$dest/meta.txt"
        else
            echo "Warning: failed to archive benchstats.json from $dest/debug.log" >&2
            cat "$benchstats_json_err" >&2
            rm -f "$dest/benchstats.json"
        fi
        rm -f "$benchstats_json_err"
        benchstats_csv_err="$(mktemp)"
        if OUT="$dest/benchstats.csv" python3 {{root}}/contrib/swords/parse-benchstats.py \
            --log "$dest/debug.log" --csv 2>"$benchstats_csv_err"; then
            echo "benchstats_csv=$dest/benchstats.csv" >> "$dest/meta.txt"
        else
            echo "Warning: failed to archive benchstats.csv from $dest/debug.log" >&2
            cat "$benchstats_csv_err" >&2
            rm -f "$dest/benchstats.csv"
        fi
        rm -f "$benchstats_csv_err"
        ln -sfn "$stamp" "$archive_root/latest"
        echo "$dest" > "$archive_root/LATEST.txt"
        echo "Archived debug.log -> $dest"
        echo "  benchstats.json/csv are convenience caches; compare-benchstats re-parses debug.log"
        echo "  Compare runs: just parse-archived-log ARCHIVE=$dest"
        echo "  After next IBD: just compare-benchstats ARCHIVE=latest"
        echo "  List archives:  just list-profile-logs"
    fi
    echo "Resetting chain data under $datadir (preserving bitcoin.conf; profile logs at $archive_root)..."
    rm -rf \
        "$datadir/blocks" \
        "$datadir/chainstate" \
        "$datadir/indexes" \
        "$datadir/swords" \
        "$datadir/chainstate.leveldb.bak" \
        "$datadir/blocks.leveldb.bak" \
        "$datadir/debug.log" \
        "$datadir/peers.dat" \
        "$datadir/mempool.dat" \
        "$datadir/fee_estimates.dat" \
        "$datadir/banlist.json" \
        "$datadir/anchors.dat" \
        "$datadir/settings.json" \
        "$datadir/.lock"
    echo "Done. $datadir is ready for a fresh mainnet IBD."

[doc("Mainnet node in foreground (pass 1 IBD when no bootstrap state exists)")]
start:
    #!/usr/bin/env bash
    set -euo pipefail
    daemon="{{bitcoind}}"
    [[ -x "$daemon" ]] || { echo "bitcoind not found at $daemon — run: just build-daemon"; exit 1; }
    exec "$daemon" -datadir="{{datadir}}" -printtoconsole

[doc("Pass 2: full -reindex in foreground (compress blocks/UTXO with typed dicts)")]
pass2:
    #!/usr/bin/env bash
    set -euo pipefail
    daemon="{{bitcoind}}"
    [[ -x "$daemon" ]] || { echo "bitcoind not found at $daemon — run: just build-daemon"; exit 1; }
    exec "$daemon" -datadir="{{datadir}}" -reindex -dictbootstrap=auto -connect=0 -printtoconsole
