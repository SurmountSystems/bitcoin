Bitcoin Swords
==============

**Bitcoin Swords** is a fork of [Bitcoin Knots](https://bitcoinknots.org) (`29.x-knots` branch) focused on performance and storage for high-RAM full nodes. It retains full Knots consensus, including [BIP-110](https://github.com/bitcoin/bips/blob/master/bip-0110.mediawiki) (Reduced Data Temporary Softfork / RDTS). **This is not a consensus fork** — all changes are local node implementation details (storage format, database backend, caching). Blocks relayed on the network and chainstate hashes remain identical to Knots.

See the [design document](doc/design/swords.md) for architecture, reasoning, and implementation status.

### Features

| Feature | Status | Summary |
|---------|--------|---------|
| **blk*.dat zstd compression** | Implemented (code) | Per-block zstd dictionary compression; legacy 8-byte headers still readable; bundled dict is a placeholder |
| **LevelDB → LMDB** | Implemented (code) | `CDBWrapper` uses LMDB with automatic migration from LevelDB; **not yet exercised on a real datadir** |
| **Expanded caches + UTXO zstd** | Implemented (code) | Up to 48 GiB auto dbcache; IBD/synced profiles; UTXO zstd at LMDB boundary |
| **cs_main locking** | Implemented (Phase B, partial verify) | Shorter critical sections; lock-order fix applied; parallel validation test still flaky |

**Not production-ready.** Unit tests cover most Swords paths, but `validation_block_tests` is intermittently flaky, `validation_chainstatemanager_tests` (assumeutxo) currently fails against LMDB, functional tests and mainnet migration have not been run. See [doc/design/swords.md](doc/design/swords.md) verification gates.

**Dedicated datadir:** `~/.bitcoin-swords` (hardlink clone from Core v30; `bitcoin.conf` present; first `bitcoind` start not yet attempted).

### Example high-RAM configuration

For a machine with 96 GiB RAM (adjust values to your hardware):

```ini
# Reserve RAM for OS, wallet, mempool, and other processes
reservedram=4096

# Aggressive cache during initial block download; shrinks automatically on IBD exit
# when -dbcache is not set explicitly (auto defaults: ~62.5% / ~25% of usable RAM)
dbcache-ibd=49152
# Conservative override; auto synced ≈ 23 GiB with 96 GiB RAM and reservedram=4096
dbcache-synced=16384

# LevelDB → LMDB migration (enabled by default; shown for clarity)
migrateleveldb=1

# Block and UTXO zstd compression (enabled by default; shown for clarity)
blockzstd=1
blockzstdlevel=20
utxozstd=1
utxozstdlevel=20
```

All Swords-specific options are documented in [doc/bitcoin-conf.md](doc/bitcoin-conf.md#bitcoin-swords-options).

---

Bitcoin Knots
=============

https://bitcoinknots.org

For an immediately usable, binary version of the Bitcoin Knots software, see
the website.

What is Bitcoin Knots?
----------------------

Bitcoin Knots connects to the Bitcoin peer-to-peer network to download and fully
validate blocks and transactions. It also includes a wallet and graphical user
interface, which can be optionally built.

Further information about Bitcoin Knots is available in the [doc folder](/doc).

License
-------

Bitcoin Knots is released under the terms of the MIT license. See [COPYING](COPYING) for more
information or see https://opensource.org/licenses/MIT.

Development Process
-------------------

Development generally takes place as part of [Bitcoin Core](https://github.com/bitcoin/bitcoin), and is merged into
Knots for each release.

Even if your pull request to Core is closed, or if your feature is not
suitable for Core (eg, because it builds on a feature not supported in Core;
relies on centralised services; etc), it may still be eligible for inclusion
in Bitcoin Knots. In this case, a pull request may be opened on the
[Knots GitHub](https://github.com/bitcoinknots/bitcoin) for review and consideration.
When accepted, you are expected to maintain the submitted branch in your own
repository, and it will be automatically merged into new releases of Knots.

Developer IRC can be found on Freenode at #bitcoin-dev.

Testing
-------

Testing and code review is the bottleneck for development; we get more pull
requests than we can review and test on short notice. Please be patient and help out by testing
other people's pull requests, and remember this is a security-critical project where any mistake might cost people
lots of money.

### Automated Testing

Developers are strongly encouraged to write [unit tests](src/test/README.md) for new code, and to
submit new unit tests for old code. Unit tests can be compiled and run
(assuming they weren't disabled during the generation of the build system) with: `ctest`. Further details on running
and extending unit tests can be found in [/src/test/README.md](/src/test/README.md).

There are also [regression and integration tests](/test), written
in Python.
These tests can be run (if the [test dependencies](/test) are installed) with: `build/test/functional/test_runner.py`
(assuming `build` is your build directory).

The CI (Continuous Integration) systems make sure that every pull request is built for Windows, Linux, and macOS,
and that unit/sanity tests are run automatically.

### Manual Quality Assurance (QA) Testing

Changes should be tested by somebody other than the developer who wrote the
code. This is especially important for large or high-risk changes. It is useful
to add a test plan to the pull request description if testing the changes is
not straightforward.

Translations
------------

Changes to translations as well as new translations can be submitted to
[Bitcoin Core's Transifex page](https://explore.transifex.com/bitcoin/bitcoin/).

Translations are periodically pulled from Transifex and merged into the git repository. See the
[translation process](doc/translation_process.md) for details on how this works.

**Important**: We do not accept translation changes as GitHub pull requests because the next
pull from Transifex would automatically overwrite them again.
