// Copyright (c) 2026 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <common/args.h>
#include <compress/zstd.h>
#include <dbwrapper.h>
#include <node/coins_view_args.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <streams.h>
#include <random.h>
#include <test/util/coins.h>
#include <test/util/setup_common.h>
#include <util/fs.h>
#include <txdb.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <util/transaction_identifier.h>

#include <boost/test/unit_test.hpp>

#include <map>
#include <vector>

namespace {
static constexpr uint8_t DB_COIN{'C'};

struct RawCoinEntry {
    COutPoint* outpoint;
    uint8_t key;
    explicit RawCoinEntry(const COutPoint* ptr) : outpoint(const_cast<COutPoint*>(ptr)), key(DB_COIN) {}

    SERIALIZE_METHODS(RawCoinEntry, obj) { READWRITE(obj.key, obj.outpoint->hash, VARINT(obj.outpoint->n)); }
};

struct RawCoinDBValue {
    std::vector<uint8_t> bytes;

    template<typename Stream>
    void Unserialize(Stream& s)
    {
        bytes.resize(s.size());
        if (!bytes.empty()) s.read(MakeWritableByteSpan(bytes));
    }
};

std::map<COutPoint, std::vector<uint8_t>> ReadRawCoinValues(const fs::path& db_path)
{
    CDBWrapper db{{.path = db_path, .cache_bytes = 1 << 20, .obfuscate = true}};
    std::unique_ptr<CDBIterator> cursor{db.NewIterator()};
    cursor->Seek(DB_COIN);

    std::map<COutPoint, std::vector<uint8_t>> values;
    while (cursor->Valid()) {
        RawCoinEntry entry{nullptr};
        COutPoint outpoint;
        entry.outpoint = &outpoint;
        if (!cursor->GetKey(entry) || entry.key != DB_COIN) break;

        RawCoinDBValue stored;
        if (!cursor->GetValue(stored)) break;
        values.emplace(outpoint, stored.bytes);
        cursor->Next();
    }
    return values;
}

CoinsViewOptions MakeZstdOptions(const fs::path& dict_path, int encode_workers)
{
    CoinsViewOptions options;
    options.utxo_zstd = true;
    options.utxo_zstd_level = DEFAULT_UTXO_ZSTD_LEVEL;
    options.utxo_zstd_dict_path = dict_path;
    options.utxo_encode_workers = encode_workers;
    options.batch_write_bytes = 64 << 20;
    return options;
}

void PopulateDirtyCoins(FastRandomContext& rng, CCoinsViewCache& cache, const uint256& hash_block, int count)
{
    cache.SetBestBlock(hash_block);
    for (int i = 0; i < count; ++i) {
        AddTestCoin(rng, cache);
    }
}

class CCoinsViewCacheProbe : public CCoinsViewCache
{
public:
    explicit CCoinsViewCacheProbe(CCoinsView* base) : CCoinsViewCache(base) {}

    bool HasFlaggedEntries() const { return m_sentinel.second.Next() != &m_sentinel; }
};

class FailBatchWriteView final : public CCoinsViewBacked
{
public:
    explicit FailBatchWriteView(CCoinsView* base_in) : CCoinsViewBacked(base_in) {}

    bool BatchWrite(CoinsViewCacheCursor& cursor, const uint256& hashBlock) override
    {
        (void)cursor;
        (void)hashBlock;
        return false;
    }
};

bool CompareSerialParallelFlush(const fs::path& dict_path,
                                int coin_count,
                                const uint256& hash_block,
                                const uint256& coin_seed)
{
    const fs::path serial_path{fs::temp_directory_path() / fs::u8path(strprintf("parallel-serial-%d", coin_count))};
    const fs::path parallel_path{fs::temp_directory_path() / fs::u8path(strprintf("parallel-par-%d", coin_count))};
    fs::remove_all(serial_path);
    fs::remove_all(parallel_path);

    CoinsViewOptions serial_options{MakeZstdOptions(dict_path, /*encode_workers=*/0)};
    CCoinsViewDB serial_db{{.path = serial_path, .cache_bytes = 1 << 20, .wipe_data = true, .obfuscate = true}, serial_options};
    {
        FastRandomContext coin_rng;
        coin_rng.Reseed(coin_seed);
        CCoinsViewCache cache{&serial_db, /*deterministic=*/true};
        PopulateDirtyCoins(coin_rng, cache, hash_block, coin_count);
        if (!cache.Flush()) return false;
    }

    CoinsViewOptions parallel_options{MakeZstdOptions(dict_path, /*encode_workers=*/4)};
    CCoinsViewDB parallel_db{{.path = parallel_path, .cache_bytes = 1 << 20, .wipe_data = true, .obfuscate = true}, parallel_options};
    {
        FastRandomContext coin_rng;
        coin_rng.Reseed(coin_seed);
        CCoinsViewCache cache{&parallel_db, /*deterministic=*/true};
        PopulateDirtyCoins(coin_rng, cache, hash_block, coin_count);
        if (!cache.Flush()) return false;
    }

    const auto serial_values{ReadRawCoinValues(serial_path)};
    const auto parallel_values{ReadRawCoinValues(parallel_path)};
    if (serial_values.size() != static_cast<size_t>(coin_count)) return false;
    if (parallel_values.size() != static_cast<size_t>(coin_count)) return false;
    for (const auto& [outpoint, serial_bytes] : serial_values) {
        const auto it{parallel_values.find(outpoint)};
        if (it == parallel_values.end()) return false;
        if (serial_bytes != it->second) return false;
    }
    return true;
}

bool CompareSnapshotAndLegacyFlush(const fs::path& dict_path,
                                   int coin_count,
                                   const uint256& hash_block,
                                   const uint256& coin_seed,
                                   bool will_erase)
{
    const fs::path legacy_path{fs::temp_directory_path() / fs::u8path(strprintf("flush-legacy-%d-%d", coin_count, will_erase))};
    const fs::path snapshot_path{fs::temp_directory_path() / fs::u8path(strprintf("flush-snapshot-%d-%d", coin_count, will_erase))};
    fs::remove_all(legacy_path);
    fs::remove_all(snapshot_path);

    CoinsViewOptions options{MakeZstdOptions(dict_path, /*encode_workers=*/4)};

    CCoinsViewDB legacy_db{{.path = legacy_path, .cache_bytes = 1 << 20, .wipe_data = true, .obfuscate = true}, options};
    {
        FastRandomContext coin_rng;
        coin_rng.Reseed(coin_seed);
        CCoinsViewCache cache{&legacy_db, /*deterministic=*/true};
        PopulateDirtyCoins(coin_rng, cache, hash_block, coin_count);
        if (will_erase ? !cache.Flush() : !cache.Sync()) return false;
    }

    CCoinsViewDB snapshot_db{{.path = snapshot_path, .cache_bytes = 1 << 20, .wipe_data = true, .obfuscate = true}, options};
    {
        FastRandomContext coin_rng;
        coin_rng.Reseed(coin_seed);
        CCoinsViewCache cache{&snapshot_db, /*deterministic=*/true};
        PopulateDirtyCoins(coin_rng, cache, hash_block, coin_count);
        const CoinsFlushSnapshot snapshot{cache.CaptureFlushSnapshot(will_erase)};
        if (!snapshot_db.BatchWriteFromSnapshot(snapshot)) return false;
        if (!cache.ValidateFlushSnapshot(snapshot)) return false;
        cache.CommitFlushSnapshot(snapshot);
    }

    const auto legacy_values{ReadRawCoinValues(legacy_path)};
    const auto snapshot_values{ReadRawCoinValues(snapshot_path)};
    return legacy_values == snapshot_values
        && legacy_values.size() == static_cast<size_t>(coin_count);
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(txdb_parallel_encode_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(parallel_encode_matches_serial_lmdb_bytes)
{
    const fs::path dict_path{compress::DefaultUtxoDictionaryPath()};
    BOOST_REQUIRE_MESSAGE(!dict_path.empty(), "UTXO dictionary path must be configured");
    BOOST_REQUIRE(fs::exists(dict_path));

    const fs::path serial_path{m_args.GetDataDirBase() / "parallel-encode-serial"};
    const fs::path parallel_path{m_args.GetDataDirBase() / "parallel-encode-parallel"};
    fs::remove_all(serial_path);
    fs::remove_all(parallel_path);

    const uint256 hash_block{m_rng.rand256()};
    const uint256 coin_seed{m_rng.rand256()};
    constexpr int COIN_COUNT{300};

    CoinsViewOptions serial_options{MakeZstdOptions(dict_path, /*encode_workers=*/0)};
    CCoinsViewDB serial_db{{.path = serial_path, .cache_bytes = 1 << 20, .wipe_data = true, .obfuscate = true}, serial_options};
    {
        FastRandomContext coin_rng;
        coin_rng.Reseed(coin_seed);
        CCoinsViewCache cache{&serial_db, /*deterministic=*/true};
        PopulateDirtyCoins(coin_rng, cache, hash_block, COIN_COUNT);
        BOOST_REQUIRE(cache.Flush());
    }

    CoinsViewOptions parallel_options{MakeZstdOptions(dict_path, /*encode_workers=*/4)};
    CCoinsViewDB parallel_db{{.path = parallel_path, .cache_bytes = 1 << 20, .wipe_data = true, .obfuscate = true}, parallel_options};
    {
        FastRandomContext coin_rng;
        coin_rng.Reseed(coin_seed);
        CCoinsViewCache cache{&parallel_db, /*deterministic=*/true};
        PopulateDirtyCoins(coin_rng, cache, hash_block, COIN_COUNT);
        BOOST_REQUIRE(cache.Flush());
    }

    const auto serial_values{ReadRawCoinValues(serial_path)};
    const auto parallel_values{ReadRawCoinValues(parallel_path)};
    BOOST_REQUIRE_EQUAL(serial_values.size(), COIN_COUNT);
    BOOST_REQUIRE_EQUAL(parallel_values.size(), COIN_COUNT);

    for (const auto& [outpoint, serial_bytes] : serial_values) {
        const auto it{parallel_values.find(outpoint)};
        BOOST_REQUIRE(it != parallel_values.end());
        BOOST_CHECK_EQUAL_COLLECTIONS(serial_bytes.begin(), serial_bytes.end(), it->second.begin(), it->second.end());
    }
}

BOOST_AUTO_TEST_CASE(parallel_encode_disabled_below_threshold)
{
    const fs::path dict_path{compress::DefaultUtxoDictionaryPath()};
    BOOST_REQUIRE_MESSAGE(!dict_path.empty(), "UTXO dictionary path must be configured");

    const fs::path serial_path{m_args.GetDataDirBase() / "parallel-encode-small-serial"};
    const fs::path parallel_path{m_args.GetDataDirBase() / "parallel-encode-small-parallel"};
    fs::remove_all(serial_path);
    fs::remove_all(parallel_path);

    const uint256 hash_block{m_rng.rand256()};
    const uint256 coin_seed{m_rng.rand256()};
    constexpr int COIN_COUNT{32};

    CoinsViewOptions serial_options{MakeZstdOptions(dict_path, /*encode_workers=*/0)};
    CCoinsViewDB serial_db{{.path = serial_path, .cache_bytes = 1 << 20, .wipe_data = true, .obfuscate = true}, serial_options};
    {
        FastRandomContext coin_rng;
        coin_rng.Reseed(coin_seed);
        CCoinsViewCache cache{&serial_db, /*deterministic=*/true};
        PopulateDirtyCoins(coin_rng, cache, hash_block, COIN_COUNT);
        BOOST_REQUIRE(cache.Flush());
    }

    CoinsViewOptions parallel_options{MakeZstdOptions(dict_path, /*encode_workers=*/4)};
    CCoinsViewDB parallel_db{{.path = parallel_path, .cache_bytes = 1 << 20, .wipe_data = true, .obfuscate = true}, parallel_options};
    {
        FastRandomContext coin_rng;
        coin_rng.Reseed(coin_seed);
        CCoinsViewCache cache{&parallel_db, /*deterministic=*/true};
        PopulateDirtyCoins(coin_rng, cache, hash_block, COIN_COUNT);
        BOOST_REQUIRE(cache.Flush());
    }

    const auto serial_values{ReadRawCoinValues(serial_path)};
    const auto parallel_values{ReadRawCoinValues(parallel_path)};
    BOOST_REQUIRE_EQUAL(serial_values.size(), COIN_COUNT);
    BOOST_REQUIRE_EQUAL(parallel_values.size(), COIN_COUNT);
    for (const auto& [outpoint, serial_bytes] : serial_values) {
        const auto it{parallel_values.find(outpoint)};
        BOOST_REQUIRE(it != parallel_values.end());
        BOOST_CHECK_EQUAL_COLLECTIONS(serial_bytes.begin(), serial_bytes.end(), it->second.begin(), it->second.end());
    }
}

BOOST_AUTO_TEST_CASE(parallel_encode_disabled_without_zstd)
{
    const fs::path dict_path{compress::DefaultUtxoDictionaryPath()};
    BOOST_REQUIRE_MESSAGE(!dict_path.empty(), "UTXO dictionary path must be configured");

    const fs::path serial_path{m_args.GetDataDirBase() / "parallel-encode-nozstd-serial"};
    const fs::path parallel_path{m_args.GetDataDirBase() / "parallel-encode-nozstd-parallel"};
    fs::remove_all(serial_path);
    fs::remove_all(parallel_path);

    const uint256 hash_block{m_rng.rand256()};
    const uint256 coin_seed{m_rng.rand256()};
    constexpr int COIN_COUNT{300};

    CoinsViewOptions serial_options;
    serial_options.utxo_zstd = false;
    serial_options.utxo_encode_workers = 0;

    CoinsViewOptions parallel_options{serial_options};
    parallel_options.utxo_encode_workers = 4;

    CCoinsViewDB serial_db{{.path = serial_path, .cache_bytes = 1 << 20, .wipe_data = true, .obfuscate = true}, serial_options};
    {
        FastRandomContext coin_rng;
        coin_rng.Reseed(coin_seed);
        CCoinsViewCache cache{&serial_db, /*deterministic=*/true};
        PopulateDirtyCoins(coin_rng, cache, hash_block, COIN_COUNT);
        BOOST_REQUIRE(cache.Flush());
    }

    CCoinsViewDB parallel_db{{.path = parallel_path, .cache_bytes = 1 << 20, .wipe_data = true, .obfuscate = true}, parallel_options};
    {
        FastRandomContext coin_rng;
        coin_rng.Reseed(coin_seed);
        CCoinsViewCache cache{&parallel_db, /*deterministic=*/true};
        PopulateDirtyCoins(coin_rng, cache, hash_block, COIN_COUNT);
        BOOST_REQUIRE(cache.Flush());
    }

    const auto serial_values{ReadRawCoinValues(serial_path)};
    const auto parallel_values{ReadRawCoinValues(parallel_path)};
    BOOST_REQUIRE_EQUAL(serial_values.size(), COIN_COUNT);
    BOOST_REQUIRE_EQUAL(parallel_values.size(), COIN_COUNT);
    for (const auto& [outpoint, serial_bytes] : serial_values) {
        const auto it{parallel_values.find(outpoint)};
        BOOST_REQUIRE(it != parallel_values.end());
        BOOST_CHECK_EQUAL_COLLECTIONS(serial_bytes.begin(), serial_bytes.end(), it->second.begin(), it->second.end());
    }
}

BOOST_AUTO_TEST_CASE(parallel_encode_threshold_boundary)
{
    const fs::path dict_path{compress::DefaultUtxoDictionaryPath()};
    BOOST_REQUIRE_MESSAGE(!dict_path.empty(), "UTXO dictionary path must be configured");

    const uint256 hash_block{m_rng.rand256()};
    const uint256 coin_seed{m_rng.rand256()};

    BOOST_CHECK(CompareSerialParallelFlush(dict_path, 255, hash_block, coin_seed));
    BOOST_CHECK(CompareSerialParallelFlush(dict_path, 256, hash_block, coin_seed));
}

BOOST_AUTO_TEST_CASE(parallel_encode_spent_and_unspent_mix)
{
    const fs::path dict_path{compress::DefaultUtxoDictionaryPath()};
    BOOST_REQUIRE_MESSAGE(!dict_path.empty(), "UTXO dictionary path must be configured");

    const fs::path db_path{m_args.GetDataDirBase() / "parallel-encode-spent-mix"};
    fs::remove_all(db_path);

    CoinsViewOptions options{MakeZstdOptions(dict_path, /*encode_workers=*/4)};
    CCoinsViewDB db{{.path = db_path, .cache_bytes = 1 << 20, .wipe_data = true, .obfuscate = true}, options};
    CCoinsViewCache cache{&db, /*deterministic=*/true};

    const uint256 hash_block{m_rng.rand256()};
    cache.SetBestBlock(hash_block);

    std::vector<COutPoint> outpoints;
    outpoints.reserve(300);
    for (int i = 0; i < 300; ++i) {
        outpoints.push_back(AddTestCoin(m_rng, cache));
    }
    for (int i = 0; i < 100; ++i) {
        cache.SpendCoin(outpoints[static_cast<size_t>(i)]);
    }

    BOOST_REQUIRE(cache.Flush());

    for (int i = 0; i < 100; ++i) {
        BOOST_CHECK(!db.HaveCoin(outpoints[static_cast<size_t>(i)]));
    }
    for (int i = 100; i < 300; ++i) {
        BOOST_CHECK(db.HaveCoin(outpoints[static_cast<size_t>(i)]));
    }
    BOOST_CHECK_EQUAL(ReadRawCoinValues(db_path).size(), 200U);
}

BOOST_AUTO_TEST_CASE(sync_preserves_dirty_when_batchwrite_fails)
{
    const fs::path dict_path{compress::DefaultUtxoDictionaryPath()};
    BOOST_REQUIRE_MESSAGE(!dict_path.empty(), "UTXO dictionary path must be configured");

    CoinsViewOptions options{MakeZstdOptions(dict_path, /*encode_workers=*/4)};
    CCoinsViewDB db{{.path = "sync-dirty-fail", .cache_bytes = 1 << 20, .memory_only = true, .obfuscate = true}, options};
    FailBatchWriteView failing{&db};
    CCoinsViewCacheProbe cache{&failing};

    const uint256 hash_block{m_rng.rand256()};
    PopulateDirtyCoins(m_rng, cache, hash_block, 300);
    BOOST_REQUIRE(cache.HasFlaggedEntries());

    BOOST_CHECK(!cache.Sync());
    BOOST_CHECK(cache.HasFlaggedEntries());
}

BOOST_AUTO_TEST_CASE(sync_clears_dirty_after_successful_parallel_batchwrite)
{
    const fs::path dict_path{compress::DefaultUtxoDictionaryPath()};
    BOOST_REQUIRE_MESSAGE(!dict_path.empty(), "UTXO dictionary path must be configured");

    CoinsViewOptions options{MakeZstdOptions(dict_path, /*encode_workers=*/4)};
    CCoinsViewDB db{{.path = "sync-dirty-success", .cache_bytes = 1 << 20, .memory_only = true, .obfuscate = true}, options};
    CCoinsViewCacheProbe cache{&db};

    const uint256 hash_block{m_rng.rand256()};
    PopulateDirtyCoins(m_rng, cache, hash_block, 300);
    BOOST_REQUIRE(cache.HasFlaggedEntries());

    BOOST_REQUIRE(cache.Sync());
    BOOST_CHECK(!cache.HasFlaggedEntries());
}

BOOST_AUTO_TEST_CASE(flush_snapshot_matches_legacy_lmdb_bytes)
{
    const fs::path dict_path{compress::DefaultUtxoDictionaryPath()};
    BOOST_REQUIRE_MESSAGE(!dict_path.empty(), "UTXO dictionary path must be configured");
    BOOST_REQUIRE(fs::exists(dict_path));

    const uint256 hash_block{m_rng.rand256()};
    const uint256 coin_seed{m_rng.rand256()};
    constexpr int COIN_COUNT{300};

    BOOST_CHECK(CompareSnapshotAndLegacyFlush(dict_path, COIN_COUNT, hash_block, coin_seed, /*will_erase=*/true));
    BOOST_CHECK(CompareSnapshotAndLegacyFlush(dict_path, COIN_COUNT, hash_block, coin_seed, /*will_erase=*/false));
}

BOOST_AUTO_TEST_CASE(flush_snapshot_stale_detection)
{
    const fs::path dict_path{compress::DefaultUtxoDictionaryPath()};
    BOOST_REQUIRE_MESSAGE(!dict_path.empty(), "UTXO dictionary path must be configured");

    CoinsViewOptions options{MakeZstdOptions(dict_path, /*encode_workers=*/4)};
    const uint256 hash_block{m_rng.rand256()};

    {
        CCoinsViewDB db{{.path = "flush-snapshot-stale-add", .cache_bytes = 1 << 20, .memory_only = true, .obfuscate = true}, options};
        CCoinsViewCache cache{&db, /*deterministic=*/true};
        PopulateDirtyCoins(m_rng, cache, hash_block, 300);
        const CoinsFlushSnapshot snapshot{cache.CaptureFlushSnapshot(/*will_erase=*/false)};
        AddTestCoin(m_rng, cache);
        BOOST_CHECK(!cache.ValidateFlushSnapshot(snapshot));
    }

    {
        CCoinsViewDB db{{.path = "flush-snapshot-stale-spend", .cache_bytes = 1 << 20, .memory_only = true, .obfuscate = true}, options};
        CCoinsViewCache cache{&db, /*deterministic=*/true};
        PopulateDirtyCoins(m_rng, cache, hash_block, 300);
        const CoinsFlushSnapshot snapshot{cache.CaptureFlushSnapshot(/*will_erase=*/false)};
        BOOST_REQUIRE(!snapshot.entries.empty());
        cache.SpendCoin(snapshot.entries.front().outpoint);
        BOOST_CHECK(!cache.ValidateFlushSnapshot(snapshot));
    }
}

BOOST_AUTO_TEST_CASE(negative_utxoencodepar_rejected)
{
    ArgsManager args;
    args.ForceSetArg("-utxoencodepar", "-1");
    CoinsViewOptions options;
    const auto result{node::ReadCoinsViewArgs(args, options)};
    BOOST_CHECK(!result);
}

BOOST_AUTO_TEST_SUITE_END()