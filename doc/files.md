# Bitcoin Knots / Bitcoin Swords file system

This document describes the on-disk layout for Bitcoin Knots / Bitcoin Swords data directories. Bitcoin Swords extends several formats (LMDB databases, per-block zstd compression, UTXO zstd at rest); see [design/swords.md](design/swords.md) for rationale and options.

**Contents**

- [Data directory location](#data-directory-location)

- [Data directory layout](#data-directory-layout)

- [Multi-wallet environment](#multi-wallet-environment)

  - [Berkeley DB database based wallets](#berkeley-db-database-based-wallets)

  - [SQLite database based wallets](#sqlite-database-based-wallets)

- [GUI settings](#gui-settings)

- [Legacy subdirectories and files](#legacy-subdirectories-and-files)

- [Notes](#notes)

## Data directory location

The data directory is the default location where the Bitcoin Core files are stored.

1. The default data directory paths for supported platforms are:

Platform | Data directory path
---------|--------------------
Linux    | `$HOME/.bitcoin/`
macOS    | `$HOME/Library/Application Support/Bitcoin/`
Windows  | `%LOCALAPPDATA%\Bitcoin\` <sup>[\[1\]](#note1)</sup>

2. A custom data directory path can be specified with the `-datadir` option.

3. All content of the data directory, except for `bitcoin.conf` file, is chain-specific. This means the actual data directory paths for non-mainnet cases differ:

Chain option                     | Data directory path
---------------------------------|------------------------------
`-chain=main` (default)          | *path_to_datadir*`/`
`-chain=test` or `-testnet`      | *path_to_datadir*`/testnet3/`
`-chain=testnet4` or `-testnet4` | *path_to_datadir*`/testnet4/`
`-chain=signet` or `-signet`     | *path_to_datadir*`/signet/`
`-chain=regtest` or `-regtest`   | *path_to_datadir*`/regtest/`

## Data directory layout

Subdirectory       | File(s)               | Description
-------------------|-----------------------|------------
`blocks/`          |                       | Blocks directory; can be specified by `-blocksdir` option (except for `blocks/index/`)
`blocks/index/`    | LMDB database (`data.mdb`, `lock.mdb`) | Block index; `-blocksdir` option does not affect this path
`blocks/`          | `blkNNNNN.dat`<sup>[\[2\]](#note2)</sup> | Actual Bitcoin blocks. Legacy layout: 8-byte header (magic + size) + raw serialized block. Swords extended layout: 9-byte header (magic + flags + stored_size) + payload; bit 0 of flags indicates zstd-compressed payload. Compression is per-block (not per-file) for random access. Plaintext is compressed then XOR-obfuscated via `xor.dat`. Max 128 MiB per file. Set `-blockzstd=0` to write new blocks with the legacy 8-byte header.
`blocks/`          | `revNNNNN.dat`<sup>[\[2\]](#note2)</sup> | Block undo data (custom format)
`blocks/`          | `xor.dat`             | Rolling XOR pattern for block and undo data files
`chainstate/`      | LMDB database (`data.mdb`, `lock.mdb`) | Blockchain state (UTXO set and metadata). New UTXO writes use legacy raw serialized `Coin` bytes or `0x01 0x01` + zstd payload when compression wins; legacy and migrated entries remain readable. One `MDB_env` per database directory.
`*.leveldb.bak/`   | Legacy LevelDB backup directory | Created when a pre-Swords LevelDB database is migrated to LMDB (e.g. `chainstate.leveldb.bak/`, `blocks/index.leveldb.bak/`, `indexes/txindex.leveldb.bak/`). Remove manually after verifying the migration. To roll back, delete the LMDB directory and rename the backup back to the original path.
`*.lmdb-migrate-tmp/` | Temporary LMDB migration workspace | Present only during an in-progress migration; removed on success or failure
`indexes/txindex/` | LMDB database (`data.mdb`, `lock.mdb`) | Transaction index; *optional*, used if `-txindex=1`
`indexes/blockfilter/basic/db/` | LMDB database (`data.mdb`, `lock.mdb`) | Blockfilter index database for the basic filtertype; *optional*, used if `-blockfilterindex=basic`
`indexes/blockfilter/basic/`    | `fltrNNNNN.dat`<sup>[\[2\]](#note2)</sup> | Blockfilter index filters for the basic filtertype; *optional*, used if `-blockfilterindex=basic`
`indexes/coinstats/db/` | LMDB database (`data.mdb`, `lock.mdb`) | Coinstats index; *optional*, used if `-coinstatsindex=1`

### LMDB XOR obfuscation per database (Swords)

Application-layer XOR at the LMDB value layer (`CDBWrapper` `f_obfuscate`). Block/undo `*.dat` files use a separate rolling XOR via `blocks/xor.dat`.

| Database path | XOR enabled | Source |
|---------------|-------------|--------|
| `chainstate/` | yes | `src/validation.cpp` |
| `blocks/index/` | yes | `src/init.cpp` |
| `indexes/txindex/` | yes | `src/index/txindex.cpp` |
| `indexes/blockfilter/.../db/` | no (upstream default) | `src/index/blockfilterindex.cpp` |
| `indexes/coinstats/db/` | no (upstream default) | `src/index/coinstatsindex.cpp` |
`wallets/`         |                       | [Contains wallets](#multi-wallet-environment); can be specified by `-walletdir` option; if `wallets/` subdirectory does not exist, wallets reside in the [data directory](#data-directory-location)
`./`               | `anchors.dat`         | Anchor IP address database, created on shutdown and deleted at startup. Anchors are last known outgoing block-relay-only peers that are tried to re-connect to on startup
`./`               | `banlist.json`        | Stores the addresses/subnets of banned nodes.
`./`               | `bitcoin.conf`        | User-defined [configuration settings](bitcoin-conf.md) for `bitcoind` or `bitcoin-qt`. File is not written to by the software and must be created manually. Path can be specified by `-conf` option
`./`               | `bitcoin_rw.conf`     | Contains [configuration settings](bitcoin-conf.md) modified by `bitcoind` or `bitcoin-qt`; can be specified by `-confrw` option
`./`               | `bitcoind.pid`        | Stores the process ID (PID) of `bitcoind` or `bitcoin-qt` while running; created at start and deleted on shutdown; can be specified by `-pid` option
`./`               | `debug.log`           | Contains debug information and general logging generated by `bitcoind` or `bitcoin-qt`; can be specified by `-debuglogfile` option
`./`               | `fee_estimates.dat`   | Stores statistics used to estimate minimum transaction fees required for confirmation
`./`               | `guisettings.ini.bak` | Backup of former [GUI settings](#gui-settings) after `-resetguisettings` option is used
`./`               | `ip_asn.map`          | IP addresses to Autonomous System Numbers (ASNs) mapping used for bucketing of the peers; path can be specified with the `-asmap` option
`./`               | `mempool.dat`         | Dump of the mempool's transactions
`./`               | `onion_v3_private_key` | Cached Tor onion service private key for `-listenonion` option
`./`               | `i2p_private_key`     | Private key that corresponds to our I2P address. When `-i2psam=` is specified the contents of this file is used to identify ourselves for making outgoing connections to I2P peers and possibly accepting incoming ones. Automatically generated if it does not exist.
`./`               | `peers.dat`           | Peer IP address database (custom format)
`./`               | `settings.json`       | Read-write settings set through GUI or RPC interfaces, augmenting manual settings from [bitcoin.conf](bitcoin-conf.md). File is created automatically if read-write settings storage is not disabled with `-nosettings` option. Path can be specified with `-settings` option
`./`               | `.cookie`             | Session RPC authentication cookie; if used, created at start and deleted on shutdown; can be specified by `-rpccookiefile` option
`./`               | `.lock`               | Data directory lock file

## Multi-wallet environment

Wallets are Berkeley DB (BDB) or SQLite databases.

1. Each user-defined wallet named "wallet_name" resides in the `wallets/wallet_name/` subdirectory.

2. The default (unnamed) wallet resides in `wallets/` subdirectory; if the latter does not exist, the wallet resides in the data directory.

3. A wallet database path can be specified with the `-wallet` option.

4. `wallet.dat` files must not be shared across different node instances, as that can result in key-reuse and double-spends due the lack of synchronization between instances.

5. Any copy or backup of the wallet should be done through a `backupwallet` call in order to update and lock the wallet, preventing any file corruption caused by updates during the copy.


### Berkeley DB database based wallets

Subdirectory | File(s)           | Description
-------------|-------------------|-------------
`database/`  | BDB logging files | Part of BDB environment; created at start and deleted on shutdown; a user *must keep it as safe* as personal wallet `wallet.dat`
`./`         | `db.log`          | BDB error file
`./`         | `wallet.dat`      | Personal wallet (a BDB database) with keys and transactions
`./`         | `.walletlock`     | BDB wallet lock file

### SQLite database based wallets

Subdirectory | File                 | Description
-------------|----------------------|-------------
`./`         | `wallet.dat`         | Personal wallet (a SQLite database) with keys and transactions
`./`         | `wallet.dat-journal` | SQLite Rollback Journal file for `wallet.dat`. Usually created at start and deleted on shutdown. A user *must keep it as safe* as the `wallet.dat` file.


## GUI settings

`bitcoin-qt` uses [`QSettings`](https://doc.qt.io/qt-5/qsettings.html) class; this implies platform-specific [locations where application settings are stored](https://doc.qt.io/qt-5/qsettings.html#locations-where-application-settings-are-stored).
You can change this with the `-guisettingsdir=<path>` option. It will use the cross-platform ini format then. `<path>` is where the `.ini` file is stored.

## Legacy subdirectories and files

These subdirectories and files are no longer used by Bitcoin Core:

Path           | Description | Repository notes
---------------|-------------|-----------------
`banlist.dat`  | Stores the addresses/subnets of banned nodes; superseded by `banlist.json` in 22.0 and completely ignored in 23.0 | [PR #20966](https://github.com/bitcoin/bitcoin/pull/20966), [PR #22570](https://github.com/bitcoin/bitcoin/pull/22570)
`blktree/`     | Blockchain index; replaced by `blocks/index/` in [0.8.0](https://github.com/bitcoin/bitcoin/blob/master/doc/release-notes/release-notes-0.8.0.md#improvements) | [PR #2231](https://github.com/bitcoin/bitcoin/pull/2231), [`8fdc94cc`](https://github.com/bitcoin/bitcoin/commit/8fdc94cc8f0341e96b1edb3a5b56811c0b20bd15)
`coins/`       | Unspent transaction output database; replaced by `chainstate/` in 0.8.0 | [PR #2231](https://github.com/bitcoin/bitcoin/pull/2231), [`8fdc94cc`](https://github.com/bitcoin/bitcoin/commit/8fdc94cc8f0341e96b1edb3a5b56811c0b20bd15)
`blkindex.dat` | Blockchain index BDB database; replaced by {`chainstate/`, `blocks/index/`, `blocks/revNNNNN.dat`<sup>[\[2\]](#note2)</sup>} in 0.8.0 | [PR #1677](https://github.com/bitcoin/bitcoin/pull/1677)
`blk000?.dat`  | Block data (custom format, 2 GiB per file); replaced by `blocks/blkNNNNN.dat`<sup>[\[2\]](#note2)</sup> in 0.8.0 | [PR #1677](https://github.com/bitcoin/bitcoin/pull/1677)
`addr.dat`     | Peer IP address BDB database; replaced by `peers.dat` in [0.7.0](https://github.com/bitcoin/bitcoin/blob/master/doc/release-notes/release-notes-0.7.0.md) | [PR #1198](https://github.com/bitcoin/bitcoin/pull/1198), [`928d3a01`](https://github.com/bitcoin/bitcoin/commit/928d3a011cc66c7f907c4d053f674ea77dc611cc)
`onion_private_key` | Cached Tor onion service private key for `-listenonion` option. Was used for Tor v2 services; replaced by `onion_v3_private_key` in [0.21.0](https://github.com/bitcoin/bitcoin/blob/master/doc/release-notes/release-notes-0.21.0.md) | [PR #19954](https://github.com/bitcoin/bitcoin/pull/19954)

## Notes

<a name="note1">1</a>. The `/` (slash, U+002F) is used as the platform-independent path component separator in this document.

<a name="note2">2</a>. `NNNNN` matches `[0-9]{5}` regex.

### Bitcoin Swords on-disk formats

**Block file per-block header** (`src/node/blockfile_format.h`):

| Format | Header size | Layout |
|--------|-------------|--------|
| Legacy | 8 bytes | `magic (4)` + `payload_size (4)` + `payload` |
| Extended | 9 bytes | `magic (4)` + `flags (1)` + `stored_size (4)` + `payload` |

When `flags & 0x01`, the payload is zstd-compressed using the bundled block dictionary (default path under the install prefix; override with `-blockzstddict`). `FlatFilePos.nPos` always points to the first payload byte. When `-blockzstd=0`, new blocks use the legacy 8-byte header instead of the extended format.

**UTXO value encoding** (`src/txdb.cpp`):

| Form | Prefix | Read | Write |
|------|--------|------|-------|
| Legacy (pre-Swords / migrated) | none | ✓ | ✓ (when compression disabled or not smaller) |
| Versioned uncompressed | `0x01 0x00` | ✓ | — (not written by current code) |
| Compressed LMDB | `0x01 0x01` | ✓ | ✓ (when `-utxozstd=1` and compressed size wins) |

**Bundled zstd dictionaries** (not in the datadir): shipped under the binary install prefix at `share/swords/blk.dict` and `share/swords/utxo.dict` (or the source tree during development). Override with `-blockzstddict` / `-utxozstddict`. See [design/swords.md](design/swords.md).

**LMDB layout:** Each database directory (`chainstate/`, `blocks/index/`, `indexes/...`) contains `data.mdb` and `lock.mdb`. Map size is derived from `-dbcache` or set explicitly with `-dbmapsize`. Legacy LevelDB directories are migrated automatically on startup (`-migrateleveldb=1`, default) with backup to `*.leveldb.bak/`.

**Migration smoke (brief):** Use a test datadir, not production. Start with `-connect=0` so no IBD runs during verification; call `getblockchaininfo` (and optionally `gettxoutsetinfo`), then `stop` immediately. Second start should open LMDB directly with no new migration lines. See [design/swords.md](design/swords.md) for the full procedure and shutdown/IBD caveats.
