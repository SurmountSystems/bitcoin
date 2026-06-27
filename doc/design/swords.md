# Bitcoin Swords — Design Document

This document tracks the planned fork of [Bitcoin Knots](https://bitcoinknots.org) called **Bitcoin Swords**. It is the authoritative reference for scope, architecture, and implementation planning. Implementation will proceed in later phases; this document is documentation-only for now.

## Project identity

| Item | Value |
|------|-------|
| Name | Bitcoin Swords |
| Base | Bitcoin Knots (`29.x-knots` branch) |
| Consensus | Retain full Knots consensus, including [BIP-110](https://github.com/bitcoin/bips/blob/master/bip-0110.mediawiki) (Reduced Data Temporary Softfork / RDTS) |
| Goal | Performance and storage improvements for high-RAM, high-throughput full nodes without compromising validation correctness |

Bitcoin Swords is not a consensus fork. All changes are local node implementation details (storage format, database backend, caching, locking). Blocks relayed on the network and chainstate hashes must remain identical to Knots.

## Feature overview

Four major workstreams:

1. **zstd dictionary compression for `blk*.dat` block files**
2. **Replace LevelDB with LMDB** (maximally concurrent C++ implementation)
3. **Expanded, configurable caches with zstd-compressed UTXO storage**
4. **`cs_main` locking improvements** for better parallelism, especially during IBD

---

## 1. zstd dictionary compression for block files

### Current state

Block data is stored as raw network-format bytes via `FlatFileSeq` in `src/node/blockstorage.cpp`:

- Files: `blocks/blkNNNNN.dat` (prefix `blk`, 16 MiB preallocation chunks)
- Max file size: 128 MiB (`MAX_BLOCKFILE_SIZE` in `src/node/blockstorage.h`)
- Per-block on-disk layout (see `WriteBlock` / `ReadRawBlock`):
  - 4-byte message-start magic
  - 4-byte uncompressed payload size
  - Serialized `CBlock` (with witness)

Blocks are read at arbitrary `FlatFilePos` offsets; pruning, serving, and reindex all depend on efficient random access within a file.

There is no zstd dependency in the tree today.

### Rationale

Individual blocks are highly repetitive in structure (headers, witness serialization patterns, transaction layout). A zstd **trained dictionary** captures those patterns well. At ~100–128 MiB per `blk*.dat` file, dictionary compression is a natural fit: enough sample data to train a good dictionary, while still operating at a granularity that preserves random access.

Expected benefits:

- Reduced disk usage for the blocks directory (often hundreds of GB)
- Less I/O bandwidth during block reads (decompression is typically faster than reading extra bytes from disk)
- Configurable CPU/disk trade-off via compression level

### Proposed design

#### Compression unit

Compress **per-block payloads** (not whole files). Whole-file compression would break the existing `FlatFilePos` random-access model unless we added a secondary offset index.

#### On-disk format (new blocks)

Extend the per-block header to support compressed payloads:

```
[magic: 4 bytes]
[flags: 1 byte]         // bit 0: payload is zstd-compressed
[stored_size: 4 bytes]  // compressed size if flag set, else uncompressed size
[payload: stored_size bytes]
```

Legacy blocks (no flag, existing layout) must continue to read correctly. Detection: if `flags` byte is absent or zero and magic+size match the legacy layout, use the existing path.

#### Dictionary

- Train one or more dictionaries offline from a representative sample of mainnet block data.
- Ship a default dictionary with the release (e.g. `share/swords/blk.dict`).
- Allow override via `-blockzstddict=<path>`.
- Optionally support per-`blk` file dictionaries stored in `blocks/index/` for marginal ratio gains (deferred; start with a single global dictionary).

Dictionary training can be a standalone tool (`contrib/swords/train-blk-dict.cpp` or script) run once against existing `blk*.dat` files.

#### Configuration options (planned)

| Option | Default | Description |
|--------|---------|-------------|
| `-blockzstd` | `1` | Enable zstd compression for new block writes |
| `-blockzstdlevel=<n>` | `20` | zstd compression level (1–22) |
| `-blockzstddict=<path>` | bundled default | Path to dictionary file |
| `-blockzstddecompress` | `1` | Allow reading zstd-compressed blocks (disable only for debugging) |

#### Code touchpoints

| File | Role |
|------|------|
| `src/node/blockstorage.cpp` | `WriteBlock`, `ReadRawBlock`, reindex paths |
| `src/node/blockstorage.h` | Format constants, new options on `BlockManager::Options` |
| `src/flatfile.h` / `src/flatfile.cpp` | Possibly unchanged; compression is above `FlatFileSeq` |
| `src/kernel/blockmanager_opts.h` | Thread new options through construction |
| `cmake/` | Add `FindZstd.cmake` or use `pkg-config libzstd` |
| `doc/files.md` | Document new dictionary file and format change |

#### Migration / compatibility

- **Reading**: Must read both legacy uncompressed and new compressed blocks indefinitely.
- **Writing**: New blocks written after upgrade use compression when `-blockzstd=1`.
- **Reindex**: `-reindex` reads legacy format and may rewrite in compressed format.
- **No network impact**: Peers still exchange raw blocks; compression is local storage only.

#### Risks and open questions

- [ ] Benchmark decompression overhead during parallel validation (especially with many `ReadBlock` calls).
- [ ] Confirm compression ratio with a real dictionary trained on post-SegWit blocks.
- [ ] Decide whether `rev*.dat` undo files should also be compressed (out of scope for v1 unless trivial).
- [ ] Interaction with XOR obfuscation (`blocks/xor.dat`) — compress before or after XOR? (Likely: compress plaintext, then XOR the compressed bytes, matching current security model.)

---

## 2. LevelDB → LMDB migration

### Current state

All chain index and UTXO persistence goes through `CDBWrapper` (`src/dbwrapper.h`, `src/dbwrapper.cpp`), which wraps the vendored LevelDB in `src/leveldb/`.

**Databases using `CDBWrapper`:**

| Database | Class | Path | Source |
|----------|-------|------|--------|
| Block tree index | `kernel::BlockTreeDB` | `blocks/index/` | `src/node/blockstorage.h` |
| UTXO set (chainstate) | `CCoinsViewDB` | `chainstate/` | `src/txdb.h`, `src/txdb.cpp` |
| UTXO snapshot chainstate | `CCoinsViewDB` | `chainstate_snapshot/` | same |
| Transaction index | `TxIndex::DB` | `indexes/txindex/` | `src/index/txindex.h` |
| Block filter index | `BlockFilterIndex::DB` | `indexes/blockfilter/.../db/` | `src/index/blockfilterindex.cpp` |
| Coinstats index | `CoinStatsIndex::DB` | `indexes/coinstats/db/` | `src/index/coinstatsindex.cpp` |
| Base index | `BaseIndex::DB` | per-index | `src/index/base.h` |

**Build system:**

- `cmake/leveldb.cmake` — embeds LevelDB
- `cmake/module/FindLevelDB.cmake`
- `src/CMakeLists.txt` — links `leveldb` into `bitcoin_node` and related targets

**Tests:**

- `src/test/dbwrapper_tests.cpp`

### Rationale

LevelDB allows only one concurrent writer and serializes writers with readers in practice. LMDB provides **MVCC**: many readers proceed without blocking a single writer (and readers do not block each other). For a node that is simultaneously validating, serving RPC, and flushing UTXO batches, LMDB is a better fit for maximizing read concurrency in C++.

### Proposed design

#### Abstraction strategy

Retain the `CDBWrapper` public API (`Read`, `Write`, `Erase`, `WriteBatch`, `NewIterator`, `EstimateSize`, obfuscation support) and **swap the implementation** from LevelDB to LMDB. This minimizes churn at call sites (`BlockTreeDB`, `CCoinsViewDB`, index DBs).

Rename internally if helpful (`CDBWrapper` → implementation detail), but avoid a flag-day rename across the tree in the first PR.

#### LMDB environment layout

| Current (LevelDB) | Proposed (LMDB) |
|-------------------|-----------------|
| Directory of SSTables and logs | Single `data.mdb` + `lock.mdb` per database directory |
| `cache_bytes` for table cache | LMDB uses mmap; `cache_bytes` maps to OS page cache behavior. Expose `-dbmapsize` for explicit map sizing. |

Each existing database directory (`chainstate/`, `blocks/index/`, etc.) becomes one LMDB environment.

#### Concurrency model

- **Readers**: `MDB_RDONLY` transactions; many concurrent reader threads (RPC, `GetCoin`, index lookups, block index walks).
- **Writer**: One `MDB_WRITEMAP` or standard write transaction at a time; batch UTXO flushes and block index updates serialize writes (same logical constraint as today, but readers are not stalled).
- **Thread-local reader txn**: Pool of read-only transactions for hot paths (`GetCoin`, `ReadBlockIndex`) to avoid repeated `mdb_txn_begin` overhead.
- **No long-lived write transactions**: Keep write txn scope as short as today's `WriteBatch` commits.

#### Obfuscation

`CDBWrapper` currently XOR-obfuscates values with a random key stored in the DB. Preserve this at the LMDB value layer so on-disk bytes are not plaintext UTXO data.

#### Migration path

1. **Detection**: On startup, detect LevelDB vs LMDB directory layout (LevelDB has `CURRENT`, `MANIFEST-*`; LMDB has `data.mdb`).
2. **Automatic migration tool**: `-migrateleveldb` (or automatic on first run) reads all keys via existing iterator API from LevelDB, writes to LMDB, renames old dir to `chainstate.leveldb.bak`.
3. **Fresh installs**: Write LMDB directly.
4. **Rollback**: Keep backup directory until user confirms; document manual rollback in release notes.

#### Code touchpoints

| Area | Files |
|------|-------|
| Core wrapper | `src/dbwrapper.h`, `src/dbwrapper.cpp` (rewrite) |
| Remove vendored LevelDB | `src/leveldb/` (delete after migration complete) |
| CMake | `cmake/leveldb.cmake` → `cmake/lmdb.cmake`, `cmake/module/FindLMDB.cmake`, `CMakeLists.txt`, `depends/` |
| Sanity check | `dbwrapper_SanityCheck()` — LMDB version check |
| Tests | `src/test/dbwrapper_tests.cpp`, new concurrency stress tests |
| Docs | `doc/files.md`, `doc/dependencies.md` |

#### Risks and open questions

- [ ] LMDB map size must be set large enough for peak UTXO set growth; document sizing guidance and auto-grow strategy (`mdb_env_set_mapsize`).
- [ ] Windows/macOS LMDB build and CI coverage.
- [ ] `CCoinsViewDB::ResizeCache()` currently recreates the LevelDB options with a new block cache; LMDB does not have an equivalent — redefine this as map-size / reader-pool tuning.
- [ ] Evaluate whether `memenv` (in-memory chainstate for tests) needs an LMDB in-memory mode (`MDB_NOSUBDIR` + tmpfs) or a separate test double.

---

## 3. Cache limits and UTXO zstd compression

### Current state

Cache sizing is controlled primarily by `-dbcache` (MiB):

| Constant | Value | File |
|----------|-------|------|
| `MIN_DBCACHE_BYTES` | 4 MiB | `src/node/dbcache.h` |
| `MAX_DEFAULT_DBCACHE` (auto) | 2 GiB | `src/node/dbcache.h` |
| `RESERVED_RAM` | 2 GiB | `src/node/dbcache.h` |
| Auto formula | `(total_ram - 2 GiB) / 4`, capped at 2 GiB | `src/node/dbcache.cpp` |

`-dbcache` is split in `kernel::CacheSizes` (`src/kernel/caches.h`):

| Pool | Cap | Notes |
|------|-----|-------|
| Block tree DB cache | 2 MiB max | Tiny; block index is mostly on disk |
| Coins DB cache | 8 MiB max | LevelDB block cache for chainstate |
| Coins tip cache | Remainder | In-memory UTXO cache (`CCoinsViewCache`) |

Index caches are carved out separately in `src/node/caches.cpp` (txindex up to 1 GiB, filter indexes up to 1 GiB combined).

**IBD cache rebalancing** happens in `ChainstateManager::MaybeRebalanceCaches()` (`src/validation.cpp`): when both IBD and snapshot chainstates exist, caches are split 95/5; otherwise the active chainstate gets 100%.

There is no dynamic IBD → synced transition that lowers memory automatically today; the user must restart with a lower `-dbcache`.

### Rationale

On a 96 GiB DDR5 machine, the current 2 GiB auto cap leaves most RAM unused. Keeping more of the UTXO set in the coins tip cache reduces LMDB reads during block validation. Compressing UTXO values before LMDB storage reduces disk footprint and can improve effective cache hit rate (more entries fit in the same RAM).

### Proposed design

#### Expanded cache configuration

Add granular, optional overrides (all in MiB; `0` = use computed default):

| Option | Purpose |
|--------|---------|
| `-dbcache` | Total cache budget (existing; raise auto cap) |
| `-dbcache-ibd` | Total cache budget while IBD is active (default: aggressive, e.g. 50–75% of `(ram - reserved)`) |
| `-dbcache-synced` | Total cache budget after IBD completes (default: moderate, e.g. 25% of `(ram - reserved)`) |
| `-coinscache` | Override coins tip cache size directly |
| `-coinsdbcache` | Override coins DB cache / LMDB reader pool budget |
| `-blocktreecache` | Override block index DB cache |
| `-maxmempool` | Existing; clarify relationship to cache budget |
| `-reservedram` | Override `RESERVED_RAM` (default 2 GiB; allow 4–8 GiB on large systems) |

**Auto formula changes (planned):**

- Raise `MAX_DEFAULT_DBCACHE` significantly on 64-bit (e.g. allow up to 32–48 GiB when RAM permits).
- On 96 GiB RAM with `-reservedram=4096`: IBD default might target ~48 GiB dbcache; synced default ~16 GiB.

#### IBD → synced transition

Hook into `ChainstateManager::IsInitialBlockDownload()` transition (`src/validation.cpp`):

1. When IBD flips from `true` → `false`, call new `ChainstateManager::ApplySyncedCacheProfile()`.
2. Shrink coins tip cache and LMDB map read-ahead toward `-dbcache-synced` values.
3. Log the transition clearly: `IBD complete; reducing cache from X MiB to Y MiB`.
4. Optionally trigger a gentle `CoinsViewCache` flush before shrinking to avoid abrupt eviction spikes.

`MaybeRebalanceCaches()` remains for snapshot/IBD dual-chainstate scenarios.

#### UTXO zstd dictionary compression

Apply at the `CCoinsViewDB` / LMDB value boundary in `src/txdb.cpp`:

- On `BatchWrite`: serialize `Coin` → compress with zstd + dictionary → store in LMDB.
- On `GetCoin` / iterator: decompress → deserialize `Coin`.
- Stored value prefix: `[version: 1 byte][compression_flag: 1 byte][payload...]` to distinguish legacy LevelDB-migrated entries, uncompressed LMDB entries, and compressed LMDB entries.

**Dictionary:**

- Train on serialized UTXO entries sampled from `chainstate/` (tool similar to block dictionary trainer).
- Ship default `share/swords/utxo.dict`.
- `-utxozstddict=<path>`, `-utxozstdlevel=<n>` (default 20), `-utxozstd=1`.

**Interaction with in-memory cache:**

`CCoinsViewCache` holds deserialized `Coin` objects. Compression affects only the persistent layer; the tip cache is unaffected. This means compression cost is paid on cache miss (disk read) and flush (disk write), which is the desired trade-off.

#### Code touchpoints

| File | Role |
|------|------|
| `src/node/dbcache.h`, `src/node/dbcache.cpp` | New limits, IBD/synced profiles |
| `src/kernel/caches.h` | Raise/remove tiny caps on block tree and coins DB pools |
| `src/node/caches.cpp` | Wire new options |
| `src/init.cpp` | Register `ArgsManager` options |
| `src/validation.cpp` | `MaybeRebalanceCaches`, IBD transition hook |
| `src/txdb.cpp` | Compress/decompress at rest |
| `src/coins.h` | No change to `Coin` serialization in memory |

#### Risks and open questions

- [ ] Compressed UTXO values must remain byte-for-byte deterministic for a given `Coin` — use fixed zstd parameters and pinned dictionary version.
- [ ] Migration: existing LevelDB/LMDB entries may be uncompressed; mixed-format support required during transition.
- [ ] Measure CPU overhead on flush (`FlushStateToDisk`) with multi-GB UTXO writes.
- [ ] Wallet rescans and `assumeutxo` snapshot load paths must handle compressed chainstate.

---

## 4. `cs_main` locking improvements

### Current state

`cs_main` is a global `RecursiveMutex` (`src/kernel/cs_main.h`). It guards chainstate, block index, and much of validation. It is one of the primary bottlenecks for concurrent RPC, P2P block processing, and background tasks.

**Heavy `cs_main` users (non-exhaustive):**

| Area | Files | Notes |
|------|-------|-------|
| Validation / chainstate | `src/validation.cpp`, `src/validation.h` | ConnectBlock, ActivateBestChain, FlushStateToDisk |
| Block storage | `src/node/blockstorage.cpp`, `src/node/blockstorage.h` | `LoadBlockIndexGuts`, prune, block pos updates |
| P2P | `src/net_processing.cpp` | Block acceptance, header processing |
| RPC | `src/rpc/blockchain.cpp`, `src/rpc/mining.cpp`, others | Many calls hold `cs_main` for entire RPC |
| Indexes | `src/index/base.cpp` | BlockConnected callbacks |

Clang thread-safety annotations (`EXCLUSIVE_LOCKS_REQUIRED`, `LOCK(cs_main)`) are used extensively; any change must preserve these invariants or update annotations.

### Rationale

During IBD, the node downloads and validates blocks aggressively. Holding `cs_main` across disk I/O, decompression, or long RPC walks starves other work. The goal is **more parallelism without changing validation outcomes**.

### Proposed design (incremental)

This is a multi-phase effort. Each phase must pass all existing functional tests plus new concurrency tests.

#### Phase A — Document invariants (no code change)

Write an invariants checklist before moving locks:

1. Block index (`m_block_index`) consistency: no dangling `CBlockIndex` pointers.
2. Active chain tip visibility: `ChainActive().Tip()` is stable for the duration of a validation step that depends on it.
3. UTXO view consistency: `CCoinsViewCache` parent chain matches the block being connected.
4. `nChainWork` / `nStatus` updates are visible atomically to P2P relay decisions.
5. Prune height and block file deletion cannot race with block reads.

#### Phase B — Shorten critical sections

| Opportunity | Approach |
|-------------|----------|
| `ReadBlock` / `ReadRawBlock` | Read and decompress block data **outside** `cs_main`; only lock to copy `FlatFilePos` from `CBlockIndex` |
| `FlushStateToDisk` | Release `cs_main` during LMDB write batches where the flushed view is immutable |
| RPC `getblock` | Load block without holding `cs_main` for entire serialization; use `LOCK(cs_main)` only for hash lookup |
| P2P `ProcessNewBlock` | Separate block download (already mostly lock-free) from validation entry |

#### Phase C — Finer-grained locks (higher risk)

Introduce sub-locks only where ordering is provably safe:

| Lock | Protects |
|------|----------|
| `cs_block_index` | `m_block_index`, `CBlockIndex` mutations |
| `cs_chainstate` | Active tip, `Chainstate` connect/disconnect |
| `cs_main` (reduced role) | Coordinates cross-cutting operations; eventual minimization |

**Warning:** Bitcoin Core has historically avoided fine-grained locking in validation due to subtle bugs. Any Phase C work requires formal lock ordering (`cs_block_index` → `cs_chainstate`, never reverse) and TSan runs.

#### Phase D — IBD-specific parallelism

- Parallel block fetch is already pipelined; focus on **parallel script check** (existing `CCheckQueue`) and **UTXO cache prefetch** during connect.
- With LMDB: use concurrent read transactions for `GetCoin` during validation when the view is read-only relative to the connecting block height.
- Consider a worker pool for zstd decompression of blocks read from disk during IBD.

#### Verification gates

Every locking change must satisfy:

- [ ] `test/functional/` full suite
- [ ] `validation_chainstate_tests`, `blockmanager_tests`, `coins_tests`
- [ ] ThreadSanitizer CI job (if available)
- [ ] Reproducible chainstate hash comparison against Knots on a fixed block range
- [ ] No change to block acceptance order or rejection reasons

#### Code touchpoints

| File | Role |
|------|------|
| `src/kernel/cs_main.h` | Possibly introduce additional mutex declarations |
| `src/validation.cpp` | Primary refactor target |
| `src/node/blockstorage.cpp` | Block read/write lock scope |
| `src/net_processing.cpp` | P2P block pipeline |
| `src/rpc/blockchain.cpp` | RPC lock scope reduction |

---

## Implementation roadmap (planned)

Suggested order minimizes rework:

| Phase | Workstream | Depends on |
|-------|------------|------------|
| **0** | This document + benchmarks baseline | — |
| **1** | LMDB backend behind `CDBWrapper` + migration tool | — |
| **2** | Cache limit expansion + IBD/synced profiles | Phase 1 (LMDB cache model) |
| **3** | UTXO zstd compression at rest | Phase 1 |
| **4** | `blk*.dat` zstd compression | — (independent; can parallel with 1–3) |
| **5** | `cs_main` Phase B shortenings | Phases 1, 4 (I/O paths stable) |
| **6** | `cs_main` Phase C/D (only if Phase B insufficient) | Phase 5 |

Each phase should be a reviewable PR with its own release notes entry.

---

## New dependencies (planned)

| Library | Purpose | Integration |
|---------|---------|-------------|
| [LMDB](https://www.symas.com/lmdb) | Replace LevelDB | `depends` + `cmake/module/FindLMDB.cmake` |
| [zstd](https://github.com/facebook/zstd) | Block and UTXO compression | `depends` + `cmake/module/FindZstd.cmake` |

---

## Configuration summary (planned defaults)

Example `bitcoin.conf` for a 96 GiB machine:

```ini
# Bitcoin Swords — high-RAM profile (illustrative; not yet implemented)
dbcache=49152
dbcache-ibd=49152
dbcache-synced=16384
reservedram=4096

blockzstd=1
blockzstdlevel=20

utxozstd=1
utxozstdlevel=20
```

---

## Related documentation to update during implementation

- `doc/files.md` — on-disk format changes
- `doc/dependencies.md` — LMDB, zstd
- `doc/bitcoin-conf.md` — new options
- `doc/reduce-memory.md` — updated guidance (Swords allows much higher caches)
- `README.md` — Bitcoin Swords identity section (when fork is public)

---

## Glossary

| Term | Meaning |
|------|---------|
| IBD | Initial Block Download |
| RDTS / BIP-110 | Reduced Data Temporary Softfork (Knots consensus rules) |
| Coins tip cache | In-memory `CCoinsViewCache` layer above the persistent UTXO store |
| Dictionary compression | zstd mode using a pre-trained dictionary for better ratios on small/repetitive records |