# `cs_main` invariants checklist

This document records the invariants that **must** be preserved when shortening `cs_main`
critical sections or introducing finer-grained locking (Bitcoin Swords feature 4).

## Required invariants

1. **Block index consistency** — `m_block_index` entries remain valid; no dangling `CBlockIndex*`
   pointers. Block index metadata (`nStatus`, `nFile`, `nDataPos`, `nUndoPos`) read for disk I/O
   must be snapshotted under `cs_main` before releasing the lock.

2. **Active chain tip stability** — `ChainActive().Tip()` and validation steps that depend on the
   current tip must not observe inconsistent chain state. Block reads for connect/disconnect use a
   `BlockReadLoc` snapshot taken while the caller holds `cs_main`.

3. **UTXO view consistency** — `CCoinsViewCache` mutations during `ConnectBlock` / `DisconnectBlock`
   remain serialized under `cs_main`. Coins flushes still run under `cs_main` because the cache
   cursor references live in-memory state.

4. **`nChainWork` / `nStatus` atomicity** — P2P relay decisions that consult block index status
   continue to do so under `cs_main`. Only immutable snapshots (position, hash, have-data flags)
   are copied for out-of-lock disk reads.

5. **Prune vs. block read** — Prune file sets are computed under `cs_main`; `UnlinkPrunedFiles`
   operates on that fixed set outside the lock. RPC/P2P paths check `BLOCK_HAVE_DATA` under
   `cs_main` before reading.

6. **Block-index write lock ordering** — `m_cs_block_index_write` serializes LMDB block-index
   writes across chainstates. **Never wait on `cs_main` while holding `m_cs_block_index_write`.**
   Both `FlushStateToDisk` and `WriteBlockIndexDB` follow the same pattern: snapshot dirty
   block-index state under `cs_main`, release `cs_main`, take `m_cs_block_index_write` for I/O,
   release `m_cs_block_index_write`, then re-acquire `cs_main` only for `CommitBlockIndexWriteBatch`.
   `FlushStateToDiskLocked` (caller holds `cs_main` exactly once) must keep that hold alive across
   `LEAVE_CRITICAL_SECTION` / `ENTER_CRITICAL_SECTION` (do not end the RAII scope before `LEAVE`),
   and must not hold `cs_LastBlockFile` (or any other lock) across that `LEAVE`. Callers already
   holding `cs_main` must use `FlushStateToDiskLocked`; `FlushStateToDisk` acquires `cs_main` itself.
   `ActivateBestChain` must release `m_chainstate_mutex` before calling `FlushStateToDisk` (flush takes
   `cs_main`; parallel `ProcessNewBlock` may hold `cs_main` while waiting on `m_chainstate_mutex`).

7. **No validation outcome changes** — Block acceptance order, rejection reasons, and chainstate
   hashes must remain identical to unmodified Knots.

## Phase B patterns (implemented)

| Pattern | Usage |
|---------|-------|
| `BlockReadLoc` / `UndoReadLoc` | Copy `FlatFilePos` + hash under brief `cs_main` lock; read/decompress outside |
| `LEAVE_CRITICAL_SECTION` / `ENTER_CRITICAL_SECTION` | Release `cs_main` during disk read inside validation functions that otherwise hold the lock |
| `BlockIndexWriteBatch` | `PrepareBlockIndexWriteBatch` copies dirty state under `cs_main` without clearing; LMDB write outside `cs_main` |
| `CommitBlockIndexWriteBatch` | Clears dirty flags only after successful LMDB write and only if in-memory state still matches the prepared snapshot |
| `m_cs_block_index_write` | Mutex serializes LMDB writes; **never** wait on `cs_main` while holding `m_cs_block_index_write` — `FlushStateToDisk` and `WriteBlockIndexDB` snapshot under `cs_main`, `LEAVE`/`release` `cs_main`, write under `m_cs_block_index_write`, then commit under `cs_main` only after releasing the write mutex |
| Deferred P2P cmpctblock read | Capture `BlockReadLoc` under `cs_main`; read block after the send loop releases the lock |

## Not yet implemented (Phase C+)

- Separate `cs_block_index` / `cs_chainstate` sub-locks
- UTXO flush without holding `cs_main` during LMDB write
- IBD decompression worker pool

See [swords.md](swords.md) section 4 for the full roadmap.