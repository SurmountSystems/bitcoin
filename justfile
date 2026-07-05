# Bitcoin Swords dev shortcuts. Run `just` to list recipes.

root := justfile_directory()
build := root / "build"
bindir := build / "bin"
jobs := num_cpus()
datadir := env_var_or_default("SWORDS_DATADIR", env_var("HOME") + "/.bitcoin-swords")

bitcoind := bindir / "bitcoind"
test_bitcoin := bindir / "test_bitcoin"
bitcoin_cli := bindir / "bitcoin-cli"

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

[doc("Start reindex-chainstate (isolated from P2P)")]
reindex: build-daemon
    {{bitcoind}} -datadir={{datadir}} -reindex-chainstate -connect=0

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

[doc("Parse reindex debug.log (default: SWORDS_DATADIR)")]
parse-log DATADIR=datadir:
    python3 {{root}}/contrib/swords/parse-reindex-log.py {{DATADIR}}

[doc("IBD read-path unit tests (equivalence + decompress + locking); recipe name test-phase-d is historical")]
test-phase-d: build-tests
    {{test_bitcoin}} --run_test={{phase_d_tests}}
