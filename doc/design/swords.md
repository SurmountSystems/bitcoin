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
| 4 | `cs_main` locking improvements | **Implemented (Phase B)** |

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

#### Remaining work

- [ ] Train production dictionary from representative mainnet `blk*.dat` sample (bundled dict is a placeholder).
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
| Sync mode | `MDB_NOSYNC` for on-disk envs | Matches prior LevelDB durability trade-off |

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
bitcoind -datadir=$HOME/.bitcoin-swords -connect=0 -daemon=0

# After "Done loading", verify RPC then stop before any catch-up:
bitcoin-cli -datadir=$HOME/.bitcoin-swords getblockchaininfo
bitcoin-cli -datadir=$HOME/.bitcoin-swords gettxoutsetinfo   # optional; slow on large UTXO sets
bitcoin-cli -datadir=$HOME/.bitcoin-swords stop
```

Confirm in `debug.log`: three `Finished LevelDB -> LMDB migration` lines, `Opened LMDB successfully` for each DB, no `MDB_MAP_FULL`. On second start, expect direct `Opening LMDB` / `Opened LMDB successfully` with **no** new `Migrating LevelDB` lines.

**Do not** leave the node on the open network during migration smoke. The P0-3 run synced ~600 blocks between migration completion and `stop`; shutdown then raced with `UpdateTip`, leaving chainstate tip ahead of flushed UTXOs (recoverable with `-reindex-chainstate`, not a migration defect).

#### Known operational caveats

| Caveat | Notes |
|--------|-------|
| **Shutdown during IBD** | `MDB_NOSYNC` matches prior LevelDB durability: shutdown flush is the durability boundary. If `stop` is requested while msghand is still connecting blocks, `UpdateTip` may run after `Shutdown: In progress`, leaving tip metadata ahead of flushed coins. Avoid by using `-connect=0` during smoke, or wait for IBD to quiesce before `stop`. Recovery: `-reindex-chainstate`. |
| **Large txindex migration logs** | Progress logs every 1M entries (`dbwrapper_leveldb_migrate.cpp`). Older builds logged every 100k entries and could trigger `Excessive logging detected` suppression for ~1B-entry txindex (~3 min of suppressed disk logs); cosmetic only. |
| **Shutdown flush observability** | Shutdown logs `Flushing chainstate to disk on shutdown...` before the final `FlushStateMode::ALWAYS` passes; large flushes also emit the existing `Flushing large (N GiB) UTXO set` warning. |

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

Bundled `share/swords/utxo.dict` (install prefix, not datadir) is a bootstrap placeholder; a mainnet-trained dictionary is recommended for production.

#### Code touchpoints

| File | Role |
|------|------|
| `src/node/dbcache.h`, `src/node/dbcache.cpp` | Limits, IBD/synced profiles |
| `src/node/caches.cpp` | Option wiring, shrink logic |
| `src/validation.cpp` | `ApplySyncedCacheProfile`, IBD transition hook |
| `src/txdb.cpp` | Compress/decompress at rest |
| `share/swords/utxo.dict` | Bundled bootstrap dictionary |

#### Remaining work

- [ ] Train production UTXO dictionary from mainnet chainstate sample.
- [ ] Measure CPU overhead on large `FlushStateToDisk` with compression enabled.

---

## 4. `cs_main` locking improvements

**Status: Implemented (Phase B)**

### Rationale

`cs_main` is a global `RecursiveMutex` guarding chainstate, block index, and much of validation. It is a primary bottleneck for concurrent RPC, P2P block processing, and background tasks during IBD. The goal is **more parallelism without changing validation outcomes**.

### Design (incremental)

#### Phase A — Document invariants ✅

See [cs_main_invariants.md](cs_main_invariants.md) for the invariants checklist.

#### Phase B — Shorten critical sections ✅

| Opportunity | Implementation |
|-------------|----------------|
| `ReadBlock` / `ReadRawBlock` | `BlockReadLoc` / `UndoReadLoc` snapshot `FlatFilePos` + hash under brief `cs_main`; I/O and zstd decompress outside lock |
| `FlushStateToDisk` | `BlockIndexWriteBatch` collected under `cs_main`; block file flush, LMDB block-index write, and prune unlink run outside lock; coins flush remains under `cs_main` (cache cursor is live state) |
| RPC `getblock` | Lookup under lock; `GetRawBlockChecked` / `GetBlockChecked` use `BlockReadLoc` and read outside lock |
| P2P block serving | `getdata` / REST already read outside lock; cmpctblock announcements defer disk read until after the send loop releases `cs_main` |
| Validation connect/disconnect | `ConnectTip`, `DisconnectTip`, `RollforwardBlock`, `VerifyDB` release `cs_main` during `ReadBlock` via `LEAVE_CRITICAL_SECTION` |

**Code touchpoints:** `src/node/blockstorage.{h,cpp}`, `src/validation.cpp`, `src/net_processing.cpp`, `src/rpc/blockchain.cpp`, `src/init.cpp` (ZMQ).

#### Phase C — Finer-grained locks (higher risk, not implemented)

Introduce sub-locks (`cs_block_index`, `cs_chainstate`) only where ordering is provably safe. Requires formal lock ordering and TSan runs.

#### Phase D — IBD-specific parallelism (not implemented)

- Worker pool for zstd decompression during IBD block reads.
- Concurrent LMDB read transactions for `GetCoin` during validation when view is read-only.
- Release `cs_main` during UTXO LMDB write batches (requires immutable flush snapshot).

#### Verification gates

- [x] Unit tests: `blockmanager_tests`, `blockchain_tests`, `cs_main_locking_tests`, `caches_tests`, `utxo_zstd_tests`, `zstd_tests`, `dbwrapper_tests`
- [~] `validation_block_tests` — passes in isolation; `processnewblock_signals_ordering` **~90–95% pass @ 120s** post-P0-1 (was ~20%); P0-1 `cs_main`/`m_chainstate_mutex` deadlock fixed; remaining ~5–10% flake tracked separately (validation-interface ordering / test harness, not P0-1)
- [~] `validation_chainstatemanager_tests` — assumeutxo snapshot tests require LMDB reader-slot hygiene (`mdb_reader_check` before read txns)
- [x] First-start LevelDB → LMDB migration on a real Core datadir (`~/.bitcoin-swords`, 2026-06-27): blocks/index 921108 entries/341ms, chainstate 167830082 entries/58s, txindex 1246227219 entries/488s; `Loaded best chain` height=915951; `*.leveldb.bak` created; second start skips migration; first-start RPC `gettxoutsetinfo` `hash_serialized_3=6966e63cfaba6fef05aab3f5d260d4152a306451d8f9b4e4e27dba84f950faa4`; smoke procedure documented (`-connect=0`); post-smoke IBD/shutdown inconsistency recovered via `-reindex-chainstate`
- [ ] `test/functional/` full suite
- [ ] ThreadSanitizer CI job
- [ ] Reproducible chainstate hash comparison against Knots on a fixed block range
- [x] No change to block acceptance order or rejection reasons (local-only locking changes)

---

## Dependencies

| Library | Purpose | Integration |
|---------|---------|-------------|
| [LMDB](https://www.symas.com/lmdb) 0.9.35 | Primary database backend | Embedded in `src/lmdb/` or system via `-DWITH_SYSTEM_LMDB=ON` |
| [zstd](https://github.com/facebook/zstd) | Block and UTXO compression | System library via `cmake/module/FindZstd.cmake` |
| LevelDB (migration only) | One-time LevelDB → LMDB read | `cmake/leveldb_migration.cmake`; not used at runtime |

---

## Configuration summary

Example `bitcoin.conf` for a 96 GiB machine:

```ini
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