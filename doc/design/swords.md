# Bitcoin Swords — Design Document

This document is the authoritative reference for **Bitcoin Swords**, a fork of [Bitcoin Knots](https://bitcoinknots.org) focused on performance and storage for high-RAM full nodes. It records scope, architecture, design rationale, and implementation status.

## Project identity

| Item | Value |
|------|-------|
| Name | Bitcoin Swords |
| Base | Bitcoin Knots (`29.x-knots` branch) |
| Consensus | Retain full Knots consensus, including [BIP-110](https://github.com/bitcoin/bips/blob/master/bip-0110.mediawiki) (Reduced Data Temporary Softfork / RDTS) |
| Goal | Performance and storage improvements for high-RAM, high-throughput full nodes without compromising validation correctness |

Bitcoin Swords is **not a consensus fork**. All changes are local node implementation details (storage format, database backend, caching, locking). Blocks relayed on the network and chainstate hashes must remain identical to Knots.

## Implementation status

| # | Feature | Status |
|---|---------|--------|
| 1 | zstd dictionary compression for `blk*.dat` block files | **Implemented** |
| 2 | Replace LevelDB with LMDB | **Implemented** |
| 3 | Expanded, configurable caches with zstd-compressed UTXO storage | **Implemented** |
| 4 | `cs_main` locking improvements | **Implemented** |

---

## 1. zstd dictionary compression for block files

**Status: Implemented**

### Rationale

Individual blocks are highly repetitive in structure (headers, witness serialization patterns, transaction layout). A zstd **trained dictionary** captures those patterns well. At ~100–128 MiB per `blk*.dat` file, dictionary compression is a natural fit: enough sample data to train a good dictionary, while still operating at a granularity that preserves random access.

**Per-block, not per-file compression** was chosen because whole-file compression would break the existing `FlatFilePos` random-access model unless we added a secondary offset index. Pruning, block serving, and reindex all depend on efficient random access within a file.

Expected benefits:

- Reduced disk usage for the blocks directory (often hundreds of GB)
- Less I/O bandwidth during block reads (decompression is typically faster than reading extra bytes from disk)
- Configurable CPU/disk trade-off via compression level

### Design decisions (implementation)

| Decision | Choice | Reasoning |
|----------|--------|-----------|
| Compression unit | Per-block payload | Preserves `FlatFilePos` random access |
| On-disk header | Extended 9-byte header (magic + flags + stored_size) | Legacy 8-byte header still readable via probe parsing |
| Compression gate | Only store compressed when smaller than plaintext | Avoids expanding blocks that do not compress well |
| XOR obfuscation | Compress plaintext, then XOR via `AutoFile` | Default key `0x77` per byte (`7777777777777777` hex for the 8-byte key) in `blocks/xor.dat` on fresh blocksdirs; existing `xor.dat` is preserved |
| Dictionary | Install-prefix `share/swords/blk.dict` | Bootstrap placeholder; not in datadir; mainnet-trained dict recommended |
| Extended header use | Written when `-blockzstd=1` even if compression skipped; `-blockzstd=0` writes legacy 8-byte headers | Simplifies format detection; uncompressed payloads use flags=0 |

#### On-disk format

**Legacy (8 bytes):**

```
[magic: 4 bytes][payload_size: 4 bytes][payload]
```

**Extended (9 bytes, Swords):**

```
[magic: 4 bytes][flags: 1 byte][stored_size: 4 bytes][payload]
```

- Bit 0 of `flags`: payload is zstd-compressed (`BLOCK_SERIALIZATION_FLAG_COMPRESSED`)
- `stored_size` is compressed size when flag set, else uncompressed size
- `FlatFilePos.nPos` points to the first payload byte after the header

Parsing is implemented in `src/node/blockfile_format.cpp` and probes backward from a payload offset to detect legacy vs extended headers.

#### Configuration options

| Option | Default | Description |
|--------|---------|-------------|
| `-blockzstd` | `1` | Enable zstd compression for new block writes |
| `-blockzstdlevel=<n>` | `20` | zstd compression level (1–22) |
| `-blockzstddict=<path>` | install-prefix `share/swords/blk.dict` | Path to dictionary file (resolved from install prefix or source tree) |
| `-blockzstddecompress` | `1` | Allow reading zstd-compressed blocks (disable only for debugging) |

Setting `-blockzstd=0` disables the extended header and writes new blocks with the legacy 8-byte format. If compression is enabled but no dictionary loads, new blocks are written with extended headers without compression and a warning is logged.

#### Code touchpoints

| File | Role |
|------|------|
| `src/node/blockstorage.cpp` | `WriteBlock`, `ReadRawBlock`, dictionary init |
| `src/node/blockfile_format.h` | Header constants and parsing helpers |
| `src/compress/zstd.cpp` | Dictionary loading, compress/decompress |
| `src/node/blockmanager_args.cpp` | Option wiring |
| `share/swords/blk.dict` | Bundled bootstrap dictionary |

#### Migration / compatibility

- **Reading**: Both legacy uncompressed and new compressed blocks are supported indefinitely.
- **Writing**: New blocks use compression when `-blockzstd=1` and dictionary is loaded.
- **Reindex**: `-reindex` reads legacy format and may rewrite in compressed format.
- **No network impact**: Peers still exchange raw blocks; compression is local storage only.

#### Typed dictionary bootstrap (mainnet, two-pass)

**Status: Pass 1 and Pass 2 implemented** (sampling, incremental training, compression reindex, effectiveness report).

On **mainnet only**, Swords can bootstrap **per-bucket typed dictionaries** (scriptSig / P2WPKH / P2WSH / P2TR pre/post height 767430 for blocks; script-type UTXO buckets) via a two-pass workflow:

| Pass | State (`<datadir>/swords/bootstrap_state.json`) | Writes | Background work |
|------|--------------------------------------------------|--------|-----------------|
| 1 | `pass1_in_progress` | Extended block headers + typed UTXO headers, **uncompressed** payloads (overrides `-blockzstd` / `-utxozstd` on write) | Stratified reservoir sampling to `<datadir>/swords/samples/`; incremental `DictionaryTrainer` writes `*.dict.provisional` |
| 1 end | `pass1_complete` | — | Final capacity search train; promote `*.dict`; write `<datadir>/swords/bootstrap_baseline.json` |
| 2 | `pass2_in_progress` → `complete` | Per-bucket zstd compression on `-reindex` | Effectiveness report + `compression_report.json` |

**Datadir state files**

- **`bootstrap_state.json`** — live bootstrap state machine (phase, steady-clock timestamps, per-bucket plaintext byte counters, reservoir `*_samples_seen`). Updated during pass 1 and on pass 1 completion.
- **`bootstrap_baseline.json`** — immutable pass 1 completion snapshot for Pass 2 comparison. Written once at pass 1 end (atomic tmp + rename) with ISO `pass1_start` / `pass1_end`, `pass1_wall_seconds`, per-bucket `block_plaintext_bytes` / `utxo_plaintext_bytes`, `trained_dicts` entries from `<datadir>/swords/dict_manifest.json` (name, `chosen_size`, `holdout_ratio`, `sample_bytes` for buckets that trained), `state: pass1_complete`, and `next_step: "restart with -reindex for compression pass 2"`.
- **`compression_report.json`** — written at pass 2 completion (atomic tmp + rename) with per-bucket `plaintext_bytes` (from baseline), `stored_bytes` (pass 2), `compression_ratio`, `savings_percent`, `dict_size`, `holdout_ratio`, plus global totals, `pass1_wall_seconds` / `pass2_wall_seconds`, and optional `recommendations` (when holdout ratio was high at near-max dict capacity but pass 2 savings &lt; 50%).

- **Inscription boundary:** `InscriptionZeroHeight()` → **767430** on mainnet only (`src/compress/dict_classify.cpp`).
- **On-disk bucket ids:** block flags bits 1–4; UTXO type byte `0x80|bucket_id` (`0x01` compressed remains bucket 0 compat).
- **Manifest:** `share/swords/dict_manifest.json`; trained dicts resolve from `<datadir>/swords/dicts/` then bundled `share/swords/dicts/`.
- **Dictionary load cap:** `LoadDictionaryFile` accepts up to **4 MiB** (warns above 1 MiB).
- **Option:** `-dictbootstrap=<mode>` — `auto` (default on mainnet; starts pass 1 only during IBD when no state file exists), `off` (ignores any on-disk bootstrap state).
- **Already-synced mainnet:** pass 1 is skipped; normal compression remains enabled. Run a fresh IBD or `-reindex` on a new datadir to collect training samples.
- **Block classification:** non-coinbase transactions only for scriptSig/witness totals; equal scriptSig/witness bytes use dominant output bucket (P2WPKH wins output-type ties).
- **Test chains:** bootstrap and typed compression disabled; `InitWarning` if `-dictbootstrap` is set.

**Pass 2 workflow:** When `bootstrap_state.json` is `pass1_complete` and the node starts with `-reindex` (`-dictbootstrap=auto` on mainnet), bootstrap enters `pass2_in_progress`, loads typed `DictSet` from `<datadir>/swords/dicts/`, and keeps metrics active until the full reindex finishes:

1. **Block scan:** `LoadExternalBlockFile` reads pass-1 typed uncompressed blocks; `AcceptBlock` rewrites each via `WriteBlock` with per-bucket compression (instead of reusing old disk positions).
2. **Chain activation:** `ActivateBestChain` rebuilds chainstate; UTXO entries are re-encoded with typed compression on flush.
3. **Completion:** After all chainstates finish `ActivateBestChain`, `OnReindexComplete()` writes `compression_report.json` and transitions to `complete`.

Missing per-bucket dictionary files fall back to the monolithic bundled dictionary when available, otherwise write uncompressed (warning logged). Legacy mono-dict blocks (compressed flag only, no bucket bits) always use `share/swords/blk.dict` / `utxo.dict`. Typed-bucket decompression is serial; the parallel decompress pool applies only to legacy mono-dict blocks.

At pass 1 completion the node logs a summary block (grep / parse with `contrib/swords/parse-reindex-log.py` `dict_bootstrap` section). Exact line shapes from `DictBootstrapManager::CompletePass1()` / `SaveBaseline()`:

```
=== Dictionary bootstrap pass 1 complete (IBD wall time: <N> seconds) ===
  block <BUCKET> plaintext_bytes=<N>
  utxo <BUCKET> plaintext_bytes=<N>
Dictionary bootstrap pass 1 complete; restart with -reindex for compression pass 2
Dictionary bootstrap baseline written to <datadir>/swords/bootstrap_baseline.json
```

- Banner: `=== Dictionary bootstrap pass 1 complete (IBD wall time: %d seconds) ===` (no trailing space before `===`).
- Per-bucket lines: two leading spaces, then `block` or `utxo`, bucket name, `plaintext_bytes=%llu`.
- Next-step line: `Dictionary bootstrap pass 1 complete; restart with -reindex for compression pass 2` (semicolon before *restart*).
- Baseline path line is emitted only when `bootstrap_baseline.json` is written successfully (after `SaveState()` succeeds).

At pass 2 completion the node logs an effectiveness report (grep / parse with `contrib/swords/parse-reindex-log.py` `compression_report` section). Exact line shapes from `DictBootstrapManager::CompletePass2()` / `SaveCompressionReport()`:

```
=== Swords dictionary effectiveness report (pass 2) ===
Block buckets:
  <BUCKET>: dict=<N>KiB ratio=<R> holdout=<H> plaintext=<SIZE> stored=<SIZE> saved=<N>%
UTXO buckets:
  <BUCKET>: dict=<N>KiB ratio=<R> holdout=<H> plaintext=<SIZE> stored=<SIZE> saved=<N>%
Global:
  blocks_plaintext_pass1=<SIZE> blocks_stored_pass2=<SIZE> savings=<N>%
  utxo_plaintext_pass1=<SIZE> utxo_stored_pass2=<SIZE> savings=<N>%
  pass1_ibd_hours=<H> pass2_reindex_hours=<H>
Recommendations:
  <BUCKET>: holdout=<H> at <N>KiB (<P>% of 4096KiB max); pass 2 savings=<S>% < 50% — rerun pass 1 IBD for more samples
Dictionary bootstrap pass 2 complete; typed dictionary compression active
```

- Per-bucket lines use `holdout=<H>` when manifest has `holdout_ratio`; otherwise `holdout=n/a` (no training holdout for that bucket).
- `Recommendations:` is emitted only when at least one bucket has high holdout ratio (≥ 2.0), `chosen_size` ≥ 90% of the 4096 KiB capacity-search maximum, and pass 2 `savings_percent` &lt; 50%.
- `contrib/swords/parse-reindex-log.py` prints `compression_report` (and `dict_bootstrap`) even when `blocks==0` (reindex-only logs).

**Pass 2 operator workflow**

```bash
# After pass 1: bootstrap_state.json is pass1_complete; restart with -reindex
bitcoind -datadir=~/.bitcoin-swords -reindex -dictbootstrap=auto -connect=0

# When reindex finishes, parse effectiveness report from debug.log
python3 contrib/swords/parse-reindex-log.py ~/.bitcoin-swords
# or: just parse-log DATADIR=~/.bitcoin-swords

# Inspect on-disk report
jq . ~/.bitcoin-swords/swords/compression_report.json
```

#### Remaining work
- [ ] `contrib/swords/train-dict` — deferred; use two-pass bootstrap on mainnet IBD instead.
- [ ] Commit production typed dictionaries to `share/swords/dicts/` after a reference mainnet bootstrap run (bundled dicts remain placeholders until then).
- [ ] Benchmark decompression overhead during parallel validation at scale.
- [ ] Evaluate `rev*.dat` undo file compression (out of scope for v1).

---

## 2. LevelDB → LMDB migration

**Status: Implemented**

### Rationale

LevelDB allows only one concurrent writer and serializes writers with readers in practice. LMDB provides **MVCC**: many readers proceed without blocking a single writer (and readers do not block each other). For a node that is simultaneously validating, serving RPC, and flushing UTXO batches, LMDB is a better fit for maximizing read concurrency in C++.

### Design decisions (implementation)

| Decision | Choice | Reasoning |
|----------|--------|-----------|
| API surface | Retain `CDBWrapper` public API | Minimizes churn at `BlockTreeDB`, `CCoinsViewDB`, index DBs |
| Environment layout | One `MDB_env` per database directory | `chainstate/`, `blocks/index/`, each index path get separate envs |
| LevelDB retention | Vendored LevelDB kept as `leveldb_migration` lib only | One-time migration reads; not linked into normal runtime |
| Migration default | `-migrateleveldb=1` (automatic) | Seamless upgrade from Knots/Core LevelDB datadirs |
| Backup | Rename old dir to `*.leveldb.bak` | Manual rollback: delete LMDB dir, rename backup back |
| Map sizing | Derive from `-dbcache` or override with `-dbmapsize` | LMDB uses mmap; auto-grow on `MDB_MAP_FULL` |
| Obfuscation | XOR at LMDB value layer (application-level, not native LMDB) | Same `Obfuscation` helper as chainstate; default `0x77` key on new empty DBs; migrated LevelDB keeps its stored `obfuscate_key` |
| Sync mode | `MDB_NOSYNC` for steady-state; `mdb_env_sync` on final chainstate batch of `FlushStateMode::ALWAYS` | Matches prior LevelDB durability trade-off; shutdown is the durability boundary |

#### Per-database XOR policy (v1)

| Database | `f_obfuscate` | Notes |
|----------|---------------|-------|
| `chainstate/` | true | application-layer XOR; [`src/validation.cpp`](../src/validation.cpp) |
| `blocks/index/` | true | [`src/init.cpp`](../src/init.cpp) |
| `indexes/txindex/` | true | [`src/index/txindex.cpp`](../src/index/txindex.cpp) |
| `indexes/blockfilter/.../db/` | false | upstream default; [`src/index/blockfilterindex.cpp`](../src/index/blockfilterindex.cpp) |
| `indexes/coinstats/db/` | false | upstream default; [`src/index/coinstatsindex.cpp`](../src/index/coinstatsindex.cpp) |

v1 documents this intentional asymmetry. User deployments using txindex only are unaffected.

#### LMDB environment layout

| Current (LevelDB) | Swords (LMDB) |
|-------------------|---------------|
| Directory of SSTables and logs | `data.mdb` + `lock.mdb` per database directory |
| `cache_bytes` for table cache | mmap + OS page cache; `-dbmapsize` for explicit sizing |

Each existing database directory (`chainstate/`, `blocks/index/`, `indexes/...`) is one LMDB environment.

#### Concurrency model

- **Readers**: `MDB_RDONLY` transactions; `maxreaders` scaled from cache budget.
- **Writer**: Serialized via `write_mutex`; one write transaction at a time.
- **No long-lived write transactions**: Write txn scope matches today's `WriteBatch` commits.

#### Configuration options

| Option | Default | Description |
|--------|---------|-------------|
| `-migrateleveldb` | `1` | Automatically migrate legacy LevelDB directories to LMDB on startup |
| `-dbmapsize=<n>` | `0` (derive) | Explicit LMDB map size for chainstate databases, in MiB |
| `-lmdbsync` | `0` | Sync every chainstate LMDB write (`mdb_env_sync`); paranoid/debug only |
| `-dbbatchsize` | — | Chainstate batch size; see §3 (Cache limits) for defaults and IBD tuning |

#### Code touchpoints

| Area | Files |
|------|-------|
| Core wrapper | `src/dbwrapper.h`, `src/dbwrapper.cpp` |
| Migration | `src/dbwrapper_leveldb_migrate.cpp`, `cmake/leveldb_migration.cmake` |
| Embedded LMDB | `src/lmdb/` (0.9.35) |
| Tests | `src/test/dbwrapper_tests.cpp` |

#### Migration smoke procedure

Use a dedicated datadir (e.g. `~/.bitcoin-swords`); never run migration smoke against production `~/.bitcoin`.

```bash
# Migrate only — no peers, no IBD during smoke
# Use build/bin/bitcoind from the Swords tree, or an installed bitcoind on PATH
build/bin/bitcoind -datadir=$HOME/.bitcoin-swords -connect=0 -daemon=0

# After "Done loading", verify RPC then stop before any catch-up:
build/bin/bitcoin-cli -datadir=$HOME/.bitcoin-swords getblockchaininfo
build/bin/bitcoin-cli -datadir=$HOME/.bitcoin-swords gettxoutsetinfo   # optional; slow on large UTXO sets
build/bin/bitcoin-cli -datadir=$HOME/.bitcoin-swords stop
```

Confirm in `debug.log`: three `Finished LevelDB -> LMDB migration` lines, `Opened LMDB successfully` for each DB, no `MDB_MAP_FULL`. On second start, expect direct `Opening LMDB` / `Opened LMDB successfully` with **no** new `Migrating LevelDB` lines.

**Do not** leave the node on the open network during migration smoke. A prior smoke run synced ~600 blocks between migration completion and `stop`; shutdown then raced with `UpdateTip`, leaving chainstate tip ahead of flushed UTXOs (recoverable with `-reindex-chainstate`, not a migration defect).

#### Known operational caveats

| Caveat | Notes |
|--------|-------|
| **Shutdown during IBD** | Steady-state uses `MDB_NOSYNC`; the final chainstate batch of each `FlushStateMode::ALWAYS` flush calls `mdb_env_sync`. After `Interrupt()`, `m_chain` / `UpdateTip` no longer advance; in-flight `ConnectTip` checks `m_interrupt` before `view.Flush()` so `CoinsTip` is not updated either. `Shutdown()` order: drain ABC → pre-flush (`IF_NEEDED` or `PERIODIC` if cache LARGE) → `connman` stop → `ALWAYS` flush. Still prefer `-connect=0` during smoke, or wait for IBD to quiesce before `stop`. Never `kill -9` while `Flushing chainstate to disk on shutdown...` is in progress. Recovery after unclean kill: `-reindex-chainstate`. |
| **Large txindex migration logs** | Progress logs every 1M entries (`dbwrapper_leveldb_migrate.cpp`). Older builds logged every 100k entries and could trigger `Excessive logging detected` suppression for ~1B-entry txindex (~3 min of suppressed disk logs); cosmetic only. |
| **Shutdown flush observability** | Shutdown logs `Pre-flushing chainstate to disk on shutdown interrupt...` (incremental write, no `mdb_env_sync`; durability boundary is the subsequent `ALWAYS` passes), then `Flushing chainstate to disk on shutdown...` before `FlushStateMode::ALWAYS` (final batch calls `mdb_env_sync`). Large flushes also emit the existing `Flushing large (N GiB) UTXO set` warning. With `debug=bench`, `BatchWrite` splits encode vs LMDB write timers (`encode coins for db batch`, `write coins partial/final batch to LMDB`). |
| **Large IBD cache vs flush latency** | With `dbcache-ibd=49152` (~31 GiB coinstip + ~286 MiB unused mempool slack), periodic flush during IBD uses `min(-flushutxo-ibd-mib, 15% of coinstip+mempool slack)` (default ~4 GiB), not the synced 90% threshold — so replay triggers periodic flushes much sooner. Shutdown `FlushStateMode::ALWAYS` still flushes whatever dirty set has accumulated and can take many minutes on large caches. Prefer letting `-reindex-chainstate` run to completion over frequent stops. |
| **Stop / lock workflow** | Use `build/bin/bitcoin-cli -datadir=$HOME/.bitcoin-swords stop` (or `bitcoin-cli` on PATH) and wait for `Shutdown: done` in `debug.log` before restarting. Do not launch a second instance if startup reports a datadir lock error — the prior process may still be flushing. |

<a id="reindex-chainstate-operations"></a>

#### `-reindex-chainstate` operations

Use a dedicated datadir and isolate from the open network during recovery replay (`build/bin/bitcoind` from the Swords tree, or installed `bitcoind` on PATH):

```bash
build/bin/bitcoind -datadir=$HOME/.bitcoin-swords -reindex-chainstate -connect=0
```

| Practice | Guidance |
|----------|----------|
| **Minimize stops** | Let the node run through replay. Each `stop` triggers a full `ALWAYS` flush of the dirty UTXO cache. Enable `debug=bench` / `debug=coindb` only in short profiling windows; comment them out for long unattended runs to reduce log volume. |
| **Frequent-stop workflow** | When you expect repeated `stop` or Ctrl+C (debugging, profiling), temporarily lower `dbcache-ibd` (e.g. `8192`–`16384` in `bitcoin.conf`) or `-flushutxo-ibd-mib` (e.g. `1024`–`2048`) so periodic IBD flushes stay smaller → smaller shutdown flushes at the cost of slower block replay. Restore `49152` / default `4096` for an unattended run. |
| **Verify cache after restart** | Confirm IBD profile and coinstip sizing in `debug.log`: |

```bash
grep -iE 'Cache configuration|in-memory UTXO|resized coinstip|Automatically selected cache profiles|dbcache' ~/.bitcoin-swords/debug.log | head -30
grep -A6 'Cache configuration:' ~/.bitcoin-swords/debug.log | tail -20
grep -i 'cache=' ~/.bitcoin-swords/debug.log | tail -5
```

Expect startup lines such as `Automatically selected cache profiles … IBD=49152 MiB (~62.5%)` (substring `IBD=49152 MiB` is sufficient) and `* Using 31584.0 MiB for in-memory UTXO set`. When explicit `dbcache-ibd` equals the auto IBD cap, rely on these coinstip split lines rather than a `Config file arg: dbcache-ibd` line. During sustained replay, `UpdateTip` `cache=` should grow toward ~30+ GiB as the coinstip fills; early replay heights may show much smaller `cache=` until the UTXO set warms the cache.

#### Remaining work

- [ ] Document production map-size guidance for very large UTXO sets.
- [ ] Concurrency stress tests beyond existing unit tests.

---

## 3. Cache limits and UTXO zstd compression

**Status: Implemented**

### Rationale

On a 96 GiB DDR5 machine, the upstream 2 GiB auto cap leaves most RAM unused. Keeping more of the UTXO set in the coins tip cache reduces LMDB reads during block validation. Compressing UTXO values before LMDB storage reduces disk footprint and can improve effective cache hit rate (more entries fit in the same RAM).

**Aggressive IBD caches then shrink on exit**: during initial block download, validation is the dominant workload and benefits from a large in-memory UTXO cache. After sync, RPC, mempool, and P2P share RAM; automatically reducing cache avoids requiring a restart with lower `-dbcache`.

### Design decisions (implementation)

| Decision | Choice | Reasoning |
|----------|--------|-----------|
| Auto cap (64-bit) | 48 GiB (`MAX_DEFAULT_DBCACHE`) | Allows high-RAM nodes to use available memory (32-bit: 2 GiB cap) |
| IBD default | ~62.5% of `(total_ram - reservedram)` | Maximizes validation throughput during sync |
| Synced default | ~25% of `(total_ram - reservedram)` | Leaves headroom for OS, wallet, mempool |
| Auto-shrink | On IBD exit when IBD profile > synced profile and `-dbcache` unset | User override via explicit `-dbcache` disables shrink |
| UTXO compression boundary | `CCoinsViewDB` / LMDB value layer | `CCoinsViewCache` holds deserialized `Coin` objects; compression cost on cache miss and flush only |
| Stored value format | Legacy raw `Coin` or `0x01 0x01` + zstd | Uncompressed writes use legacy format; `0x01 0x00` prefix is read-only |
| Compression gate | Only store when compressed size < legacy serialized size | Avoids expanding entries |

#### Expanded cache configuration

| Option | Default | Description |
|--------|---------|-------------|
| `-dbcache=<n>` | platform auto (IBD profile) | Total cache budget in MiB; explicit value disables auto-shrink |
| `-dbcache-ibd=<n>` | `0` (auto ~62.5%) | Total cache while IBD is active |
| `-dbcache-synced=<n>` | `0` (auto ~25%) | Total cache after IBD completes |
| `-coinscache=<n>` | `0` (computed) | Override in-memory UTXO tip cache directly |
| `-coinsdbcache=<n>` | `0` (computed) | Override chainstate DB / LMDB reader budget |
| `-blocktreecache=<n>` | `0` (computed) | Override block index DB cache |
| `-reservedram=<n>` | `2048` | MiB reserved for non-dbcache usage in auto formulas |
| `-flushutxo-ibd-mib=<n>` | `4096` | During IBD, periodic UTXO flush when dirty cache exceeds `min(<n> MiB, 15% of coinstip+mempool slack)`; `0` flushes on any dirty cache |
| `-dbbatchsize=<n>` | `67108864` (64 MiB) | Max bytes per chainstate LMDB `WriteBatch` during `CCoinsViewDB::BatchWrite` |

**`-dbbatchsize` for IBD:** On high-RAM nodes, raise to `134217728`–`268435456` (128–256 MiB) to reduce LMDB commit overhead during large flushes. Larger batches use more transient encode RAM; pair with `debug=bench` and `debug=coindb` to compare `encode coins for db batch` vs `write coins … batch to LMDB` timings and count `Writing partial batch` lines (fewer partial batches → fewer LMDB commits).

**Example (96 GiB RAM, `reservedram=4096`):** IBD auto targets ~57 GiB dbcache; synced auto targets ~23 GiB. Setting explicit `dbcache-ibd=49152` and `dbcache-synced=16384` overrides the auto formulas.

#### IBD → synced transition

When `IsInitialBlockDownload()` flips from `true` → `false`, `ChainstateManager::ApplySyncedCacheProfile()`:

1. Shrinks coins tip and coins DB caches toward synced profile values.
2. Logs: `IBD complete; reducing cache from X MiB to Y MiB` with per-component breakdown (coinstip, coinsdb, blocktree).
3. Flushes chainstate before shrinking (`FlushStateToDisk`); defers on failure.

`MaybeRebalanceCaches()` remains for snapshot/IBD dual-chainstate scenarios.

#### UTXO zstd configuration

| Option | Default | Description |
|--------|---------|-------------|
| `-utxozstd` | `1` | Enable zstd dictionary compression for UTXO values |
| `-utxozstdlevel=<n>` | `20` | zstd compression level (1–22) |
| `-utxozstddict=<path>` | install-prefix `share/swords/utxo.dict` | Path to dictionary file (resolved from install prefix or source tree) |
| `-utxoencodepar=<n>` | `0` (auto) | Parallel UTXO pre-encode workers during `CCoinsViewDB::BatchWrite` (`1` = serial only, `2`–`8` = explicit workers; requires `-utxozstd=1` and ≥256 dirty entries) |
| `-flushsnapshot` | `1` | Snapshot dirty UTXOs under `cs_main` and release the lock during LMDB writes (`0` = legacy in-lock `Flush`/`Sync`) |

Bundled `share/swords/utxo.dict` (install prefix, not datadir) is a bootstrap placeholder; a mainnet-trained dictionary is recommended for production.

#### Parallel UTXO encode

During `FlushStateToDisk`, dirty UTXO entries are serialized and optionally zstd-compressed before LMDB `WriteBatch` commits. When the dirty count is at least 256 and `-utxozstd=1`, `CCoinsViewDB::BatchWrite` pre-encodes coin values in a bounded worker pool (`-utxoencodepar`, default auto from `-par`, capped at 8) and then performs single-threaded LMDB writes. The on-disk format is identical to the serial path; bench timers still split encode vs LMDB (`debug=bench`). Below the threshold, with `-utxozstd=0`, or with `-utxoencodepar=1`, the serial encode path is used.

#### Immutable flush snapshot

`FlushStateToDiskLocked` captures a `CoinsFlushSnapshot` (best block, dirty count, entry payloads) under `cs_main`, releases the lock for `CCoinsViewDB::BatchWriteFromSnapshot` (parallel encode + LMDB writes from the immutable snapshot) while holding per-chainstate `m_coins_flush_mutex` to serialize overlapping LMDB writes, then re-acquires `cs_main` to validate the snapshot (`hashBlock`, dirty count, per-entry coin data) before `CommitFlushSnapshot` clears or unflags the live cache. If the cache mutated during out-of-lock I/O, the flush retries until validate succeeds (logging every 16 stale attempts) instead of applying a stale batch to the live cache. `FlushStateMode::ALWAYS` re-asserts `sync_final_batch` before each write attempt. Sync-path cache finalization remains deferred until after a successful durable write. `-flushsnapshot=0` restores the legacy in-lock `Flush`/`Sync` path for debugging.

#### Code touchpoints

| File | Role |
|------|------|
| `src/node/dbcache.h`, `src/node/dbcache.cpp` | Limits, IBD/synced profiles |
| `src/node/caches.cpp` | Option wiring, shrink logic |
| `src/validation.cpp` | `ApplySyncedCacheProfile`, IBD transition hook |
| `src/txdb.cpp` | Compress/decompress at rest; parallel pre-encode before LMDB writes |
| `share/swords/utxo.dict` | Bundled bootstrap dictionary |

#### Remaining work

- [ ] Train production UTXO dictionary from mainnet chainstate sample.
- [ ] Measure CPU overhead on large `FlushStateToDisk` with compression enabled (compare serial vs `-utxoencodepar` on production hardware).

---

## 4. `cs_main` locking improvements

**Status: Implemented**

### Rationale

`cs_main` is a global `RecursiveMutex` guarding chainstate, block index, and much of validation. It is a primary bottleneck for concurrent RPC, P2P block processing, and background tasks during IBD. The goal is **more parallelism without changing validation outcomes**.

### Design (incremental)

#### Invariants

See [cs_main_invariants.md](cs_main_invariants.md) for the invariants checklist.

#### Short critical sections

| Opportunity | Implementation |
|-------------|----------------|
| `ReadBlock` / `ReadRawBlock` | `BlockReadLoc` / `UndoReadLoc` snapshot `FlatFilePos` + hash under brief `cs_main`; I/O and zstd decompress outside lock |
| `FlushStateToDisk` | `BlockIndexWriteBatch` collected under `cs_main`; block file flush, LMDB block-index write, and prune unlink run outside lock; UTXO flush snapshots dirty entries under `cs_main`, releases lock during encode+LMDB (`-flushsnapshot=1`, default), re-acquires to validate and finalize cache |
| RPC `getblock` | Lookup under lock; `GetRawBlockChecked` / `GetBlockChecked` use `BlockReadLoc` and read outside lock |
| P2P block serving | `getdata` / REST already read outside lock; cmpctblock announcements defer disk read until after the send loop releases `cs_main` |
| Validation connect/disconnect | `ConnectTip`, `DisconnectTip`, `RollforwardBlock`, `VerifyDB` release `cs_main` (and `mempool.cs` before `cs_main`) during block I/O via `ReleaseLocksForBlockIo` |

**Code touchpoints:** `src/node/blockstorage.{h,cpp}`, `src/validation.cpp`, `src/net_processing.cpp`, `src/rpc/blockchain.cpp`, `src/init.cpp` (ZMQ).

#### Finer-grained locks (deferred)

Introduce sub-locks (`cs_block_index`, `cs_chainstate`) only where ordering is provably safe. Requires formal lock ordering and TSan runs.

#### IBD read parallelism

Measurement pyramid (L0 unit equivalence → L1 microbench → L2 segment replay → L3 mainnet A/B):

| Layer | Gate |
|-------|------|
| L0 | Byte-identical decompress; `WarmCache` matches serial `FetchCoin` |
| L1 | `just bench` / `bench_bitcoin -priority-level=high` (IBD read-path microbenches; CMake target `swords_phase_d.cpp`) |
| L2 | `validation_segment_equivalence_tests` |
| L3 | Same `~/.bitcoin-swords` height segment before/after; `just parse-log` |

**Implementation status:**

| Feature | Status |
|---------|--------|
| Benchstats instrumentation (`-benchstats=1`) | **Implemented** |
| IBD read-path microbenches and baseline tooling | **Implemented** |
| Segment equivalence and parallel decompress tests | **Implemented** |
| `BlockDecompressPool` + `-blockdecompresspar` (DEBUG_ONLY; IBD/import gating, ≥32 KiB payloads) | **Implemented** |
| `BlockPrefetchQueue` depth-1 prefetch in `ActivateBestChainStep` / `ConnectTip` | **Implemented** |
| `ParallelPrefetchCoins` + `CCoinsViewCache::WarmCache` + `-coinprefetchpar` (DEBUG_ONLY; ≥64 uncached prevouts) | **Implemented** |
| LMDB reader-slot budgeting for coin prefetch (`ComputeCoinPrefetchWorkers`; reserve headroom for validation/RPC/txindex) | **Implemented** |
| Parallel UTXO pre-encode (`-utxoencodepar`) | **Implemented** (see §3) |
| Immutable flush snapshot (`-flushsnapshot=1`) | **Implemented** (see §3) |
| Ordered `LoadExternalBlockFile` decompress queue | **Deferred** |
| Promote DEBUG_ONLY flags after mainnet A/B validation | **Gated** — run `just promotion-gates` after sweep campaign |

| Option | Default | Purpose |
|--------|---------|---------|
| `-benchstats=1` | 0 | IBD read-path atomic counters (DEBUG_ONLY) |
| `-blockdecompresspar=<n>` | 0 (auto) | Parallel block zstd decompress workers (DEBUG_ONLY) |
| `-coinprefetchpar=<n>` | 0 (auto) | Parallel LMDB coin prefetch workers (DEBUG_ONLY) |

**MDB_READERS_FULL / reader budgeting:** Each prefetch worker holds a thread-local LMDB read txn (`MDB_NOTLS`). Under IBD + RPC + txindex concurrency the reader table can fill; uncapped workers previously logged thousands of `MDB_READERS_FULL` errors per block and disabled prefetch for the rest of the run. Prefetch workers are now capped by `ComputeCoinPrefetchWorkers` (reserves 96 slots for non-prefetch readers; budgets 2 slots per worker for the thread-local txn plus begin overlap). Parallel prefetch requires `chainstate_maxreaders >= 100` (i.e. `>= COIN_PREFETCH_MIN_MAXREADERS`, or `> 99`); at or below that threshold startup logs `Coin prefetch parallel disabled` and validation uses serial `FetchCoin` only. On `MDB_READERS_FULL`, prefetch calls `mdb_reader_check` to reclaim stale slots, logs once per batch (rate-limited elsewhere), arms a 60s process-wide cooldown, and validation continues with serial `FetchCoin`. Operator workarounds: raise `-dbcache` (scales `maxreaders`), or `-coinprefetchpar=1` to force serial prefetch.

Tooling: [IBD profiling and parameter sweeps (Phases 0–6)](#ibd-profiling-and-parameter-sweeps-phases-06); microbench capture via `just baseline` / `just baseline-compare`; `just test-phase-d` (IBD read-path unit tests; recipe name is historical).

#### Verification gates

- [x] Unit tests: `blockmanager_tests`, `blockchain_tests`, `cs_main_locking_tests`, `caches_tests`, `utxo_zstd_tests`, `zstd_tests`, `dbwrapper_tests`, `dict_bootstrap_tests` — must pass
- [x] Operator log parser: `python3 contrib/swords/test_parse_reindex_log.py -v` — must pass
- [x] IBD read-path unit tests: `validation_segment_equivalence_tests`, `block_decompress_parallel_tests`, `block_prefetch_queue_tests`, `txdb_prefetch_tests`, `coins_prevouts_tests` (plus overlapping `cs_main_locking_tests`, `blockmanager_tests`, `zstd_tests`) — must pass via `just test-phase-d`
- [x] `txdb_prefetch_tests` concurrency stress: `ReaderSlotHog` (deterministic reader-slot exhaustion), `MDB_READERS_FULL` cooldown/fallback, partial-budget overlap, repeated-success leak check — covered in `just test-phase-d` (Workstream A3)
- [x] Benchstats parser `readers_full` / `prefetch_hit` milestones: `python3 contrib/swords/test_benchstats_parse.py -v` and `test_compare_benchstats.py` — must pass via `just test-parse-benchstats` / `just test-compare-benchstats`
- [x] Phase 0 `check` CLI and shell gate: `run_health_check`, `just phase0-check` exit codes, `readers_full` / `MDB_READERS_FULL` fail-closed — `python3 contrib/swords/test_phase0_live_health.py -v` (`just test-phase0` / `just test-phase0-check`)
- [x] Phase 6 sweep matrix + campaign gates (53 tests): variant overlays, prefetch gate at #120, milestone compare — `just test-phase6`
- [~] `validation_block_tests` — passes in normal runs; `processnewblock_signals_ordering` is nondeterministic under extreme parallel stress (timeout or debug `CheckBlockIndex` assert); same class as upstream; no `TestSubscriber` ordering failures observed in stress runs
- [x] `validation_chainstatemanager_tests` — LMDB reader-slot hygiene (`mdb_reader_check` before read txns); assumeutxo cases pass
- [x] First-start LevelDB → LMDB migration smoke on a real Core datadir (`~/.bitcoin-swords`); documented procedure with `-connect=0`; `*.leveldb.bak` created; second start skips migration
- [x] Functional subset (4-pack): `feature_assumeutxo.py`, `feature_dbcrash.py`, `feature_coinstatsindex.py`, `feature_index_prune.py` — `just test-functional`
- [x] Local ThreadSanitizer locking gates: `just test-tsan` (`cs_main_locking_tests`, `validation_chainstatemanager_tests`, `txdb_prefetch_tests` in `build-tsan/`)
- [x] Knots regtest `hash_serialized_3` equivalence at height 101 with compression disabled (see procedure below)
- [ ] `test/functional/` full suite (~281 scripts; non-blocking — triage template below)
- [~] ThreadSanitizer CI: upstream `ci_native_tsan` job in [`.github/workflows/ci.yml`](../../.github/workflows/ci.yml) (container `00_setup_env_native_tsan.sh`); local repro via `just configure-tsan && just test-tsan`
- [ ] Reproducible chainstate hash comparison against Knots on a fixed block range
- [x] Promotion gate tests: `just test-promotion-gates` (unit); live campaign check: `just promotion-gates` (exit 1 until sweep passes)
- [ ] DEBUG_ONLY parallelism promotion (`-benchstats`, `-coinprefetchpar`, `-blockdecompresspar`, `-utxoencodepar`) — **deferred** until `just promotion-gates` returns 0 and operator checklist complete (see below)
- [x] No change to block acceptance order or rejection reasons (local-only locking changes)

**Aggregate gate:** `just verify` — build + bootstrap + `test-phase-d` + `test-parse-log` + `test-parse-benchstats` + `test-compare-benchstats` + `test-phase3` + `test-baseline` + `test-phase0` + `test-phase5` + `test-phase6` + `test-promotion-gates` + functional 4-pack. This is the curated Swords operator bar, not the full unit suite (`just test`).

#### Reproduction

```bash
# Full unit suite
just test

# Dictionary bootstrap unit tests
build/bin/test_bitcoin --run_test=dict_bootstrap_tests

# Log parser unit tests
python3 contrib/swords/test_parse_reindex_log.py -v

# IBD read-path unit tests (justfile variable phase_d_tests)
just test-phase-d

# Functional subset — stop any local Swords bitcoind first
test/functional/test_runner.py \
  feature_assumeutxo.py feature_dbcrash.py \
  feature_coinstatsindex.py feature_index_prune.py

# Local ThreadSanitizer locking gates (separate build-tsan/ tree)
just configure-tsan   # first time only
just test-tsan        # cs_main_locking_tests + validation_chainstatemanager_tests

# Optional: validation_block_tests under TSan (nondeterministic under extreme stress)
build-tsan/bin/test_bitcoin --run_test=validation_block_tests

# Phase 0 check + parser gates (also in just verify)
just test-phase0-check
just test-parse-benchstats

# Promotion prerequisites (exit 1 until mainnet sweep campaign passes)
just promotion-gates

# Full verification bar
just verify

# Optional stress repro for [~] processnewblock_signals_ordering flake
for i in $(seq 1 20); do
  timeout 120 build/bin/test_bitcoin \
    --run_test=validation_block_tests/processnewblock_signals_ordering || break
done
```

#### Knots regtest equivalence procedure

Independent mining produces different blocks per node; **copy the chain** for a valid hash comparison.

```bash
# 1. Mine on Knots (reference)
knots-bitcoind -regtest -datadir=/tmp/knots-regtest -utxozstd=0 -blockzstd=0 -daemon
knots-cli -regtest -datadir=/tmp/knots-regtest createwallet test
ADDR=$(knots-cli -regtest -datadir=/tmp/knots-regtest getnewaddress)
knots-cli -regtest -datadir=/tmp/knots-regtest generatetoaddress 101 "$ADDR"
HASH_KNOTS=$(knots-cli -regtest -datadir=/tmp/knots-regtest gettxoutsetinfo | jq -r .hash_serialized_3)
knots-cli -regtest -datadir=/tmp/knots-regtest stop

# 2. Copy identical chain to Swords datadir
mkdir -p /tmp/swords-regtest/regtest
cp -a /tmp/knots-regtest/regtest/{blocks,chainstate} /tmp/swords-regtest/regtest/

# 3. Swords read-only load
swords-bitcoind -regtest -datadir=/tmp/swords-regtest -utxozstd=0 -blockzstd=0 -connect=0 -daemon
HASH_SWORDS=$(swords-cli -regtest -datadir=/tmp/swords-regtest gettxoutsetinfo | jq -r .hash_serialized_3)
# Gate: HASH_KNOTS == HASH_SWORDS at height 101
```

Reference value at height 101 (compression disabled): `ac2d71cc68ec0f9080c837dac71fb84211fb84b44f3560c2fc89f6e2c22b8abc`.

#### v1 release notes (`v1.0.0-swords`)

- One-time LevelDB → LMDB migration; `*.leveldb.bak` disk footprint documented in §2.
- Dedicated datadir recommended (`~/.bitcoin-swords`).
- `consensusrules=rdts` (Knots BIP-110).
- Placeholder zstd dictionaries (`share/swords/{blk,utxo}.dict`); production training deferred v1.1.
- Network coverage: regtest `hash_serialized_3` verified vs Knots 29.x; mainnet/testnet/signet spot-checks after `~/.bitcoin-swords` `-reindex-chainstate` completes.
- Known `[~]`: `processnewblock_signals_ordering` parallel-stress nondeterminism (upstream parity).
- Notable fixes: `-reindex` extended-header scan; LMDB `ResizeCache` shutdown SIGSEGV; `ReleaseLocksForBlockIo` lock order; Swords fastprune wrap heights (303/608–609/2153–2160).

---

## Dependencies

| Library | Purpose | Integration |
|---------|---------|-------------|
| [LMDB](https://www.symas.com/lmdb) 0.9.35 | Primary database backend | Embedded in `src/lmdb/` or system via `-DWITH_SYSTEM_LMDB=ON` |
| [zstd](https://github.com/facebook/zstd) | Block and UTXO compression | System library via `cmake/module/FindZstd.cmake` |
| LevelDB (migration only) | One-time LevelDB → LMDB read | `cmake/leveldb_migration.cmake`; not used at runtime |

---

## IBD profiling and parameter sweeps (Phases 0–6)

**Status: Implemented (operator tooling)**

Swords ships a phased operator workflow for mainnet `-reindex-chainstate` profiling: live health checks, log parsers, microbench gates, kernel traces, and one-knob parameter sweeps. Use a dedicated datadir (default `~/.bitcoin-swords`) and profile-log archive (`~/.bitcoin-swords-profile-logs`). Rebuild after tree changes (`just build-daemon`) so benchstats rollups include current counters (e.g. Phase 4 `wait` fields).

IBD parallelism flags (`-benchstats`, `-coinprefetchpar`, `-blockdecompresspar`, `-utxoencodepar`) remain **DEBUG_ONLY** until a controlled sweep campaign passes the prefetch exit gates below; see [§4](#4-cs_main-locking-improvements) for reader budgeting and promotion criteria.

### Phase overview

| Phase | Recipes | Purpose |
|-------|---------|---------|
| 0 | `just watch-cpu`, `just phase0-check` | Live CPU/RSS sampling + automated failure detection during IBD |
| 1–2 | `just parse-log`, `just baseline-compare` | Segment replay from `debug.log` + microbench regression gates |
| 3 | `just phase3` | Unified benchstats scorecard (milestones, prefetch health, implied blk/s) |
| 4 | *(in-process)* | Benchstats `wait` counters in rollups (rebuild required) |
| 5 | `sudo -E just profile-connectblock`, `sudo -E just profile-utxo-flush` | Kernel-level ConnectBlock / UTXO flush traces (diagnostic) |
| 6 | `just phase6`, `just sweep-*` | One-knob parameter sweeps with archived A/B matrix |

Additional helpers: `just parse-benchstats`, `just compare-benchstats`, `just export-benchstats`, `just phase0-snapshot`, `just profile-ibd`, `just phase2` (unit tests + baseline compare), `just test-tsan` (local TSan locking gates), `just promotion-gates` (DEBUG_ONLY promotion prerequisites), `just verify` (full tooling + functional gate).

**Datadir defaults:** `justfile` variables `datadir` (`~/.bitcoin-swords`) and `profile_logs` (`~/.bitcoin-swords-profile-logs`). Override with `SWORDS_DATADIR` / `SWORDS_LOG_ARCHIVE` where supported.

### Phase 0 — live health

```bash
# Background CPU/RSS + block height CSV (Ctrl+C or DURATION= to stop)
just watch-cpu

# Automated health check (non-zero exit for CI / cron)
just phase0-check
```

`just phase0-check` tails `debug.log` from the latest `Swords run started` marker:

| Result | Condition |
|--------|-----------|
| **FAIL** (exit 1) | `readers_full > 0` on the latest benchstats rollup, or any `MDB_READERS_FULL` line since run start |
| **WARN** (exit 0) | `txindex_per_blk_ms` above threshold (default 500 ms/blk), or `prefetch_hit=0` with `coin_prevouts ≥ 64` for the last N rollups (default N=3) |
| **OK** (exit 0) | None of the above |

Tune thresholds: `just phase0-check TXINDEX_WARN_MS=750 PREFETCH_ROLLUPS=5`.

### Phases 1–2 — log replay and microbench gates

```bash
just parse-log                    # segment replay metrics from live datadir
just parse-archived-log latest    # same parser on archived debug.log
just baseline-compare             # bench_bitcoin medians vs captured baseline
just phase2                       # test-phase-d then baseline-compare (fail-fast gate)
```

`just baseline-compare` exit codes are documented in [share/swords/bench-baselines/README.md](../../share/swords/bench-baselines/README.md#compare-exit-codes-just-baseline-compare--baseline_reportpy-compare):

| Code | Meaning |
|------|---------|
| 0 | All checks OK |
| 1 | WARN: segment or benchstats drift (\|delta\| > 10%), missing field, or bench \|delta\| > 10% |
| 2 | FAIL: any bench median slower than baseline by > 5% |

### Phase 3 — benchstats scorecard

```bash
just phase3        # human-readable milestone table
just phase3-json   # machine-readable report
```

Rollup milestones default to **60, 110, 120, 200, 250** blocks (~117k cliff captured at #120). Reports include `readers_full`, `prefetch_hit`, per-stage ms/blk, and `implied_blk_per_s`.

### Phase 5 — kernel traces (diagnostic)

Requires USDT tracepoints in the running `bitcoind` (`just check-usdt`). Spot-check at rollup milestones **200** and **250** during sweeps:

```bash
sudo -E just profile-connectblock   # ConnectBlock latency; explains par_jobs=0 in benchstats
sudo -E just profile-utxo-flush     # UTXO cache flush / LMDB write spikes
```

These are **not blocking gates**; they explain tail bottlenecks after benchstats identifies them.

### Phase 6 — parameter sweeps and campaign

Each sweep variant changes **one logical knob** vs `baseline`. Reset chain data between variants so comparisons are fair.

```bash
just sweep-campaign                              # gate status + next recommended variant
just sweep-apply <variant>                       # write merged bitcoin.conf
SWORDS_SWEEP_VARIANT=<variant> just reset-datadir && just sweep-start <variant> # stamp archive + fresh replay
just phase0-check                                # periodic during IBD
just phase3                                      # milestone scorecard
just sweep-finish <variant>                      # archive debug.log + sweep-record
just sweep-compare                               # cross-variant milestone matrix
```

**Campaign order** (`just sweep-campaign`):

1. `prefetch_serial` — control (`-coinprefetchpar=1`; safe until parallel prefetch validated)
2. `baseline` — Swords IBD defaults (auto prefetch after reader budgeting fix)
3. `txindex_500` — optional (`-txindexbatch=500`; skip unless txindex is a hotspot)
4. `flush_128m` / `flush_256m` — only when `flush_lmdb_per_blk_ms` hotspot detected across recorded variants
5. `script_par` — explicit `-par=N-1` (override: `SWORDS_SWEEP_PAR=<n>`)
6. `explicit_parallel` — blocked until **baseline prefetch gate** passes at rollup **#120** (`readers_full=0`, `prefetch_hit>0`)

`just sweep-finish baseline` requires rollup #120 in the archived log; the prefetch gate **fails closed** if that milestone is missing.

**Production exit gates** (before promoting DEBUG_ONLY parallelism defaults):

Run `just promotion-gates` after the sweep campaign; it exits **0** only when automated gates pass (otherwise **1**). Operator steps below are printed but not auto-checked.

| Gate | Check |
|------|-------|
| Flags still DEBUG_ONLY | `contrib/swords/promotion_gates.py` reads `src/init.cpp` — all four flags must retain `ArgsManager::DEBUG_ONLY` |
| Sweep control recorded | `prefetch_serial` and `baseline` variants in `phase6-sweep-state.json` |
| Baseline prefetch gate | Rollup **#120**: `readers_full=0`, `prefetch_hit>0` (`baseline_passes_prefetch_gate`) |
| A/B throughput | `baseline` `implied_blk_per_s` > `prefetch_serial` at milestones **120, 200, 250** |
| IBD depth | Baseline log rollup **#250** with `readers_full=0`, `prefetch_hit>0` (~250k blocks) |
| Operator: live health | `just phase0-check` returns 0 during healthy IBD |
| Operator: verify bar | `just verify` green |
| Operator: campaign tail | `explicit_parallel` variant recorded after baseline gate passes |
| Promotion edit | Remove `DEBUG_ONLY` from `-benchstats`, `-coinprefetchpar`, `-blockdecompresspar`, `-utxoencodepar` in `src/init.cpp` **only after** `just promotion-gates` returns 0 and operator checklist is complete |

**Functional full suite (non-blocking):** triage with `test/functional/test_runner.py --help`; representative run:

```bash
# Stop local Swords bitcoind first. Full suite is hours; start with extended subset or --exclude.
test/functional/test_runner.py --combinedlogslen=4000
# Status: unchecked — track failures in issue/PR when run completes
```

Startup confirms effective flags:

```bash
grep 'Swords LMDB parallelism' ~/.bitcoin-swords/debug.log
grep 'Swords run started' ~/.bitcoin-swords/debug.log
```

Sweep state JSON lives under the profile-log archive (outside the datadir); see `just sweep-status` and `just phase6`.

### Troubleshooting

| Symptom | Likely cause | Action |
|---------|--------------|--------|
| `MDB_READERS_FULL` in log or `readers_full > 0` in benchstats | LMDB reader table exhausted under parallel coin prefetch + txindex/RPC concurrency (~117k blocks on large `dbcache-ibd`) | Immediate: `just sweep-apply prefetch_serial` or `-coinprefetchpar=1` in `bitcoin.conf`. Long-term: reader budgeting in [§4 IBD read parallelism](#4-cs_main-locking-improvements) (`ComputeCoinPrefetchWorkers`). Raise `-dbcache` if `maxreaders` is low. |
| `Excessive logging detected` / suppressed `coindb` lines | Thousands of `MDB_READERS_FULL` `LogError` lines tripped Bitcoin's per-source log rate limiter | Fix prefetch reader storm (above); do not treat suppression as benign — prefetch is likely dead for the rest of the run. |
| `prefetch_hit=0` with `coin_prevouts ≥ 64` | Parallel prefetch disabled after reader exhaustion or cooldown | Confirm `grep -i readers_full` / `just phase0-check`. Compare `prefetch_serial` vs `baseline` in `just sweep-compare`. |
| `txindex_per_blk_ms` rising to seconds/blk | txindex LMDB map growth (see [disk budgeting](#ibd-disk-and-resource-budgeting) below) | Ensure ~1 TB disk headroom; try optional `txindex_500` variant; Phase 5 traces at milestones 200/250. |
| `ThreadRPCServer incorrect password attempt` on RPC | Node reachable from non-localhost | Bind RPC to localhost (`rpcbind=127.0.0.1`, `rpcallowip=127.0.0.1/32`) and firewall external ports; see [Security and production hygiene](#security-and-production-hygiene). |
| `par_jobs=0` in benchstats | Expected when script-check parallelism is idle or gated; not alone a failure | Use Phase 5 `profile-connectblock` at tail milestones to see ConnectBlock breakdown. |
| Phase 4 `wait` counters missing | Stale `bitcoind` binary | `just build-daemon` and restart IBD. |

**Log hygiene:** enable `debug=bench` / `debug=coindb` only in short profiling windows; comment out for long unattended runs (see [Security and production hygiene](#security-and-production-hygiene) and §2 [`-reindex-chainstate` operations](#reindex-chainstate-operations)).

---

## Security and production hygiene

Operator practices for mainnet IBD profiling on a dedicated datadir (`~/.bitcoin-swords`) and profile-log archive (`~/.bitcoin-swords-profile-logs`). These items do not change consensus; they reduce attack surface and accidental data exposure during long `-reindex-chainstate` campaigns.

### RPC exposure

For profiling and unattended IBD, bind JSON-RPC to loopback only. Set these in the **base** `bitcoin.conf` before `just sweep-apply` — variant overlays do not override RPC settings:

```ini
rpcbind=127.0.0.1
rpcallowip=127.0.0.1/32
# rpcport=8332   # optional; default mainnet RPC port
```

`rpcbind` selects the listen address; `rpcallowip` must also permit the client subnet ([`doc/JSON-RPC-interface.md`](../JSON-RPC-interface.md)). Set **both** together for intentional localhost-only binding — `-rpcbind` alone is ignored without `-rpcallowip`, and `-rpcallowip` without `-rpcbind` triggers an upstream warning and may not produce the expected listen surface.

Lines such as `ThreadRPCServer incorrect password attempt from <addr>` in `debug.log` (from [`src/httprpc.cpp`](../src/httprpc.cpp)) indicate a remote host reached RPC and guessed credentials. That is a common internet-wide probe pattern when RPC is reachable from non-localhost — not necessarily a local misconfiguration of `rpcpassword`. Remediation: localhost bindings above, host firewall on the RPC port, and never expose RPC to the open internet during profiling.

### Profile-log archive privacy

`just reset-datadir` archives `debug.log` under `profile_logs` (default `~/.bitcoin-swords-profile-logs`) with a `meta.txt` that records `source_datadir`, host-local paths, and optional `sweep_variant=`. Archived logs may also include absolute paths, peer addresses, and Phase 5 `perf.data` kernel traces.

- Create or use a profile-log root readable only by the operator (`chmod 700`). When the datadir already exists (the normal profiling path), `just reset-datadir` `mkdir -p` the archive root and sets mode `0700` on it (and on each stamp subdirectory when a log is archived); first-time datadir creation exits before archive setup. Chmod is best-effort (`2>/dev/null || true`) and may fail on some network filesystems.
- Override location with `SWORDS_LOG_ARCHIVE` if the default under `$HOME` is synced or shared.
- Treat archives like credentials: do not copy to public issue trackers without redacting paths and RPC-related lines.

### Sweep variant stamping

Phase 6 archives are easier to audit when `meta.txt` includes `sweep_variant=<id>`:

```bash
just sweep-apply baseline
SWORDS_SWEEP_VARIANT=baseline just reset-datadir && just sweep-start baseline
# … IBD to milestones …
just sweep-finish baseline                        # exports variant before archive + sweep-record
```

Each `just` recipe runs in a separate process. `sweep-finish` exports `SWORDS_SWEEP_VARIANT` before `reset-datadir` and `sweep-record` in one bash recipe, so archives are stamped automatically. For a manual mid-run reset (e.g. before `sweep-start`), set `SWORDS_SWEEP_VARIANT=<variant>` on the **same shell line** as `just reset-datadir`. `sweep-record` rejects a mismatch between `--variant` and the env.

### Log volume

High-volume `debug=bench` / `debug=coindb` categories are for short profiling windows only. Comment them out for long unattended IBD runs — see §2 [`-reindex-chainstate` operations](#reindex-chainstate-operations) and the troubleshooting table above (log hygiene row).

---

## IBD disk and resource budgeting

Mainnet IBD with `txindex=1` grows the txindex LMDB map substantially during sync. Observed on Swords mainnet runs: **16 GiB → ~512 GiB by ~292k blocks**. Plan **~1 TB** free disk headroom for a full mainnet IBD including blocks, chainstate, and txindex.

For high-RAM nodes, pair **`reservedram=4096`** with an explicit IBD cache override **`dbcache-ibd=49152`** (48 GiB) so auto formulas leave headroom for the OS and other services while maximizing validation throughput. See [bitcoin-conf.md](../bitcoin-conf.md#bitcoin-swords-options) for all options.

---

## Configuration summary

Example `bitcoin.conf` for a 96 GiB machine:

```ini
# Production IBD profiling: RPC localhost-only (see Security and production hygiene).
# Set in base bitcoin.conf before `just sweep-apply` — overlays do not change RPC keys.
rpcbind=127.0.0.1
rpcallowip=127.0.0.1/32

reservedram=4096
dbcache-ibd=49152
# Conservative override; auto synced ≈ 23 GiB with 96 GiB RAM and reservedram=4096
dbcache-synced=16384

blockzstd=1
blockzstdlevel=20

utxozstd=1
utxozstdlevel=20

migrateleveldb=1  # enabled by default
```

See [bitcoin-conf.md](../bitcoin-conf.md#bitcoin-swords-options) for all options with defaults.

---

## Related documentation

- [share/swords/bench-baselines/README.md](../../share/swords/bench-baselines/README.md) — microbench baseline capture and compare exit codes
- [cs_main_invariants.md](cs_main_invariants.md) — `cs_main` locking invariants (feature 4)
- [files.md](../files.md) — on-disk format changes
- [dependencies.md](../dependencies.md) — LMDB, zstd
- [bitcoin-conf.md](../bitcoin-conf.md) — new options
- [reduce-memory.md](../reduce-memory.md) — Swords allows much higher caches; also how to reduce memory

---

## Glossary

| Term | Meaning |
|------|---------|
| IBD | Initial Block Download |
| RDTS / BIP-110 | Reduced Data Temporary Softfork (Knots consensus rules) |
| Coins tip cache | In-memory `CCoinsViewCache` layer above the persistent UTXO store |
| Dictionary compression | zstd mode using a pre-trained dictionary for better ratios on small/repetitive records |