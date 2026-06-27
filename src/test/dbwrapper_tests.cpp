// Copyright (c) 2012-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <dbwrapper.h>
#include <dbwrapper_leveldb_migrate.h>
#include <serialize.h>
#include <streams.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/string.h>

#include <atomic>
#include <memory>
#include <ranges>
#include <chrono>
#include <thread>
#include <vector>

#include <boost/test/unit_test.hpp>

using util::ToString;

BOOST_FIXTURE_TEST_SUITE(dbwrapper_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(dbwrapper)
{
    // Perform tests both obfuscated and non-obfuscated.
    for (const bool obfuscate : {false, true}) {
        constexpr size_t CACHE_SIZE{1_MiB};
        const fs::path path{m_args.GetDataDirBase() / "dbwrapper"};

        Obfuscation obfuscation;
        std::vector<std::pair<uint8_t, uint256>> key_values{};

        // Write values
        {
            CDBWrapper dbw{{.path = path, .cache_bytes = CACHE_SIZE, .wipe_data = true, .obfuscate = obfuscate}};
            BOOST_CHECK_EQUAL(obfuscate, !dbw.IsEmpty());

            // Ensure that we're doing real obfuscation when obfuscate=true
            obfuscation = dbwrapper_private::GetObfuscateKey(dbw);
            BOOST_CHECK_EQUAL(obfuscate, dbwrapper_private::GetObfuscateKey(dbw));

            for (uint8_t k{0}; k < 10; ++k) {
                uint8_t key{k};
                uint256 value{m_rng.rand256()};
                BOOST_CHECK(dbw.Write(key, value));
                key_values.emplace_back(key, value);
            }
        }

        // Verify that the obfuscation key is never obfuscated
        {
            CDBWrapper dbw{{.path = path, .cache_bytes = CACHE_SIZE, .obfuscate = false}};
            BOOST_CHECK_EQUAL(obfuscation, dbwrapper_private::GetObfuscateKey(dbw));
        }

        // Read back the values
        {
            CDBWrapper dbw{{.path = path, .cache_bytes = CACHE_SIZE, .obfuscate = obfuscate}};

            // Ensure obfuscation is read back correctly
            BOOST_CHECK_EQUAL(obfuscation, dbwrapper_private::GetObfuscateKey(dbw));
            BOOST_CHECK_EQUAL(obfuscate, dbwrapper_private::GetObfuscateKey(dbw));

            // Verify all written values
            for (const auto& [key, expected_value] : key_values) {
                uint256 read_value{};
                BOOST_CHECK(dbw.Read(key, read_value));
                BOOST_CHECK_EQUAL(read_value, expected_value);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(dbwrapper_basic_data)
{
    // Perform tests both obfuscated and non-obfuscated.
    for (bool obfuscate : {false, true}) {
        fs::path ph = m_args.GetDataDirBase() / (obfuscate ? "dbwrapper_1_obfuscate_true" : "dbwrapper_1_obfuscate_false");
        CDBWrapper dbw({.path = ph, .cache_bytes = 1 << 20, .memory_only = false, .wipe_data = true, .obfuscate = obfuscate});

        uint256 res;
        uint32_t res_uint_32;
        bool res_bool;

        // Ensure that we're doing real obfuscation when obfuscate=true
        BOOST_CHECK_EQUAL(obfuscate, dbwrapper_private::GetObfuscateKey(dbw));

        //Simulate block raw data - "b + block hash"
        std::string key_block = "b" + m_rng.rand256().ToString();

        uint256 in_block = m_rng.rand256();
        BOOST_CHECK(dbw.Write(key_block, in_block));
        BOOST_CHECK(dbw.Read(key_block, res));
        BOOST_CHECK_EQUAL(res.ToString(), in_block.ToString());

        //Simulate file raw data - "f + file_number"
        std::string key_file = strprintf("f%04x", m_rng.rand32());

        uint256 in_file_info = m_rng.rand256();
        BOOST_CHECK(dbw.Write(key_file, in_file_info));
        BOOST_CHECK(dbw.Read(key_file, res));
        BOOST_CHECK_EQUAL(res.ToString(), in_file_info.ToString());

        //Simulate transaction raw data - "t + transaction hash"
        std::string key_transaction = "t" + m_rng.rand256().ToString();

        uint256 in_transaction = m_rng.rand256();
        BOOST_CHECK(dbw.Write(key_transaction, in_transaction));
        BOOST_CHECK(dbw.Read(key_transaction, res));
        BOOST_CHECK_EQUAL(res.ToString(), in_transaction.ToString());

        //Simulate UTXO raw data - "c + transaction hash"
        std::string key_utxo = "c" + m_rng.rand256().ToString();

        uint256 in_utxo = m_rng.rand256();
        BOOST_CHECK(dbw.Write(key_utxo, in_utxo));
        BOOST_CHECK(dbw.Read(key_utxo, res));
        BOOST_CHECK_EQUAL(res.ToString(), in_utxo.ToString());

        //Simulate last block file number - "l"
        uint8_t key_last_blockfile_number{'l'};
        uint32_t lastblockfilenumber = m_rng.rand32();
        BOOST_CHECK(dbw.Write(key_last_blockfile_number, lastblockfilenumber));
        BOOST_CHECK(dbw.Read(key_last_blockfile_number, res_uint_32));
        BOOST_CHECK_EQUAL(lastblockfilenumber, res_uint_32);

        //Simulate Is Reindexing - "R"
        uint8_t key_IsReindexing{'R'};
        bool isInReindexing = m_rng.randbool();
        BOOST_CHECK(dbw.Write(key_IsReindexing, isInReindexing));
        BOOST_CHECK(dbw.Read(key_IsReindexing, res_bool));
        BOOST_CHECK_EQUAL(isInReindexing, res_bool);

        //Simulate last block hash up to which UXTO covers - 'B'
        uint8_t key_lastblockhash_uxto{'B'};
        uint256 lastblock_hash = m_rng.rand256();
        BOOST_CHECK(dbw.Write(key_lastblockhash_uxto, lastblock_hash));
        BOOST_CHECK(dbw.Read(key_lastblockhash_uxto, res));
        BOOST_CHECK_EQUAL(lastblock_hash, res);

        //Simulate file raw data - "F + filename_number + filename"
        std::string file_option_tag = "F";
        uint8_t filename_length = m_rng.randbits(8);
        std::string filename = "randomfilename";
        std::string key_file_option = strprintf("%s%01x%s", file_option_tag,filename_length,filename);

        bool in_file_bool = m_rng.randbool();
        BOOST_CHECK(dbw.Write(key_file_option, in_file_bool));
        BOOST_CHECK(dbw.Read(key_file_option, res_bool));
        BOOST_CHECK_EQUAL(res_bool, in_file_bool);
   }
}

// Test batch operations
BOOST_AUTO_TEST_CASE(dbwrapper_batch)
{
    // Perform tests both obfuscated and non-obfuscated.
    for (const bool obfuscate : {false, true}) {
        fs::path ph = m_args.GetDataDirBase() / (obfuscate ? "dbwrapper_batch_obfuscate_true" : "dbwrapper_batch_obfuscate_false");
        CDBWrapper dbw({.path = ph, .cache_bytes = 1 << 20, .memory_only = true, .wipe_data = false, .obfuscate = obfuscate});

        uint8_t key{'i'};
        uint256 in = m_rng.rand256();
        uint8_t key2{'j'};
        uint256 in2 = m_rng.rand256();
        uint8_t key3{'k'};
        uint256 in3 = m_rng.rand256();

        uint256 res;
        CDBBatch batch(dbw);

        batch.Write(key, in);
        batch.Write(key2, in2);
        batch.Write(key3, in3);

        // Remove key3 before it's even been written
        batch.Erase(key3);

        BOOST_CHECK(dbw.WriteBatch(batch));

        BOOST_CHECK(dbw.Read(key, res));
        BOOST_CHECK_EQUAL(res.ToString(), in.ToString());
        BOOST_CHECK(dbw.Read(key2, res));
        BOOST_CHECK_EQUAL(res.ToString(), in2.ToString());

        // key3 should've never been written
        BOOST_CHECK(dbw.Read(key3, res) == false);
    }
}

BOOST_AUTO_TEST_CASE(dbwrapper_iterator)
{
    // Perform tests both obfuscated and non-obfuscated.
    for (const bool obfuscate : {false, true}) {
        fs::path ph = m_args.GetDataDirBase() / (obfuscate ? "dbwrapper_iterator_obfuscate_true" : "dbwrapper_iterator_obfuscate_false");
        CDBWrapper dbw({.path = ph, .cache_bytes = 1 << 20, .memory_only = true, .wipe_data = false, .obfuscate = obfuscate});

        // The two keys are intentionally chosen for ordering
        uint8_t key{'j'};
        uint256 in = m_rng.rand256();
        BOOST_CHECK(dbw.Write(key, in));
        uint8_t key2{'k'};
        uint256 in2 = m_rng.rand256();
        BOOST_CHECK(dbw.Write(key2, in2));

        std::unique_ptr<CDBIterator> it(const_cast<CDBWrapper&>(dbw).NewIterator());

        // Be sure to seek past the obfuscation key (if it exists)
        it->Seek(key);

        uint8_t key_res;
        uint256 val_res;

        BOOST_REQUIRE(it->GetKey(key_res));
        BOOST_REQUIRE(it->GetValue(val_res));
        BOOST_CHECK_EQUAL(key_res, key);
        BOOST_CHECK_EQUAL(val_res.ToString(), in.ToString());

        it->Next();

        BOOST_REQUIRE(it->GetKey(key_res));
        BOOST_REQUIRE(it->GetValue(val_res));
        BOOST_CHECK_EQUAL(key_res, key2);
        BOOST_CHECK_EQUAL(val_res.ToString(), in2.ToString());

        it->Next();
        BOOST_CHECK_EQUAL(it->Valid(), false);
    }
}

// Test that we do not obfuscation if there is existing data.
BOOST_AUTO_TEST_CASE(existing_data_no_obfuscate)
{
    // We're going to share this fs::path between two wrappers
    fs::path ph = m_args.GetDataDirBase() / "existing_data_no_obfuscate";
    fs::create_directories(ph);

    // Set up a non-obfuscated wrapper to write some initial data.
    std::unique_ptr<CDBWrapper> dbw = std::make_unique<CDBWrapper>(DBParams{.path = ph, .cache_bytes = 1 << 10, .memory_only = false, .wipe_data = false, .obfuscate = false});
    uint8_t key{'k'};
    uint256 in = m_rng.rand256();
    uint256 res;

    BOOST_CHECK(dbw->Write(key, in));
    BOOST_CHECK(dbw->Read(key, res));
    BOOST_CHECK_EQUAL(res.ToString(), in.ToString());

    // Call the destructor to release the LMDB environment lock
    dbw.reset();

    // Now, set up another wrapper that wants to obfuscate the same directory
    CDBWrapper odbw({.path = ph, .cache_bytes = 1 << 10, .memory_only = false, .wipe_data = false, .obfuscate = true});

    // Check that the key/val we wrote with unobfuscated wrapper exists and
    // is readable.
    uint256 res2;
    BOOST_CHECK(odbw.Read(key, res2));
    BOOST_CHECK_EQUAL(res2.ToString(), in.ToString());

    BOOST_CHECK(!odbw.IsEmpty()); // There should be existing data
    BOOST_CHECK(!dbwrapper_private::GetObfuscateKey(odbw)); // The key should be an empty string

    uint256 in2 = m_rng.rand256();
    uint256 res3;

    // Check that we can write successfully
    BOOST_CHECK(odbw.Write(key, in2));
    BOOST_CHECK(odbw.Read(key, res3));
    BOOST_CHECK_EQUAL(res3.ToString(), in2.ToString());
}

// Ensure that we start obfuscating during a reindex.
BOOST_AUTO_TEST_CASE(existing_data_reindex)
{
    // We're going to share this fs::path between two wrappers
    fs::path ph = m_args.GetDataDirBase() / "existing_data_reindex";
    fs::create_directories(ph);

    // Set up a non-obfuscated wrapper to write some initial data.
    std::unique_ptr<CDBWrapper> dbw = std::make_unique<CDBWrapper>(DBParams{.path = ph, .cache_bytes = 1 << 10, .memory_only = false, .wipe_data = false, .obfuscate = false});
    uint8_t key{'k'};
    uint256 in = m_rng.rand256();
    uint256 res;

    BOOST_CHECK(dbw->Write(key, in));
    BOOST_CHECK(dbw->Read(key, res));
    BOOST_CHECK_EQUAL(res.ToString(), in.ToString());

    // Call the destructor to release the LMDB environment lock
    dbw.reset();

    // Simulate a -reindex by wiping the existing data store
    CDBWrapper odbw({.path = ph, .cache_bytes = 1 << 10, .memory_only = false, .wipe_data = true, .obfuscate = true});

    // Check that the key/val we wrote with unobfuscated wrapper doesn't exist
    uint256 res2;
    BOOST_CHECK(!odbw.Read(key, res2));
    BOOST_CHECK(dbwrapper_private::GetObfuscateKey(odbw));

    uint256 in2 = m_rng.rand256();
    uint256 res3;

    // Check that we can write successfully
    BOOST_CHECK(odbw.Write(key, in2));
    BOOST_CHECK(odbw.Read(key, res3));
    BOOST_CHECK_EQUAL(res3.ToString(), in2.ToString());
}

BOOST_AUTO_TEST_CASE(iterator_ordering)
{
    fs::path ph = m_args.GetDataDirBase() / "iterator_ordering";
    CDBWrapper dbw({.path = ph, .cache_bytes = 1 << 20, .memory_only = true, .wipe_data = false, .obfuscate = false});
    for (int x=0x00; x<256; ++x) {
        uint8_t key = x;
        uint32_t value = x*x;
        if (!(x & 1)) BOOST_CHECK(dbw.Write(key, value));
    }

    // Check that creating an iterator creates a snapshot (IteratorImpl uses a dedicated read txn).
    std::unique_ptr<CDBIterator> it(const_cast<CDBWrapper&>(dbw).NewIterator());

    for (unsigned int x=0x00; x<256; ++x) {
        uint8_t key = x;
        uint32_t value = x*x;
        if (x & 1) BOOST_CHECK(dbw.Write(key, value));
    }

    for (const int seek_start : {0x00, 0x80}) {
        it->Seek((uint8_t)seek_start);
        for (unsigned int x=seek_start; x<255; ++x) {
            uint8_t key;
            uint32_t value;
            BOOST_CHECK(it->Valid());
            if (!it->Valid()) // Avoid spurious errors about invalid iterator's key and value in case of failure
                break;
            BOOST_CHECK(it->GetKey(key));
            if (x & 1) {
                BOOST_CHECK_EQUAL(key, x + 1);
                continue;
            }
            BOOST_CHECK(it->GetValue(value));
            BOOST_CHECK_EQUAL(key, x);
            BOOST_CHECK_EQUAL(value, x*x);
            it->Next();
        }
        BOOST_CHECK(!it->Valid());
    }
}

struct StringContentsSerializer {
    // Used to make two serialized objects the same while letting them have different lengths
    // This is a terrible idea
    std::string str;
    StringContentsSerializer() = default;
    explicit StringContentsSerializer(const std::string& inp) : str(inp) {}

    template<typename Stream>
    void Serialize(Stream& s) const
    {
        for (size_t i = 0; i < str.size(); i++) {
            s << uint8_t(str[i]);
        }
    }

    template<typename Stream>
    void Unserialize(Stream& s)
    {
        str.clear();
        uint8_t c{0};
        while (!s.eof()) {
            s >> c;
            str.push_back(c);
        }
    }
};

BOOST_AUTO_TEST_CASE(iterator_string_ordering)
{
    fs::path ph = m_args.GetDataDirBase() / "iterator_string_ordering";
    CDBWrapper dbw({.path = ph, .cache_bytes = 1 << 20, .memory_only = true, .wipe_data = false, .obfuscate = false});
    for (int x = 0; x < 10; ++x) {
        for (int y = 0; y < 10; ++y) {
            std::string key{ToString(x)};
            for (int z = 0; z < y; ++z)
                key += key;
            uint32_t value = x*x;
            BOOST_CHECK(dbw.Write(StringContentsSerializer{key}, value));
        }
    }

    std::unique_ptr<CDBIterator> it(const_cast<CDBWrapper&>(dbw).NewIterator());
    for (const int seek_start : {0, 5}) {
        it->Seek(StringContentsSerializer{ToString(seek_start)});
        for (unsigned int x = seek_start; x < 10; ++x) {
            for (int y = 0; y < 10; ++y) {
                std::string exp_key{ToString(x)};
                for (int z = 0; z < y; ++z)
                    exp_key += exp_key;
                StringContentsSerializer key;
                uint32_t value;
                BOOST_CHECK(it->Valid());
                if (!it->Valid()) // Avoid spurious errors about invalid iterator's key and value in case of failure
                    break;
                BOOST_CHECK(it->GetKey(key));
                BOOST_CHECK(it->GetValue(value));
                BOOST_CHECK_EQUAL(key.str, exp_key);
                BOOST_CHECK_EQUAL(value, x*x);
                it->Next();
            }
        }
        BOOST_CHECK(!it->Valid());
    }
}

BOOST_AUTO_TEST_CASE(unicodepath)
{
    // Attempt to create a database with a UTF8 character in the path.
    // On Windows this test will fail if the directory is created using
    // the ANSI CreateDirectoryA call and the code page isn't UTF8.
    // It will succeed if created with CreateDirectoryW.
    fs::path ph = m_args.GetDataDirBase() / "test_runner_₿_🏃_20191128_104644";
    CDBWrapper dbw({.path = ph, .cache_bytes = 1 << 20});

    fs::path lockPath = ph / "lock.mdb";
    BOOST_CHECK(fs::exists(lockPath));
}

BOOST_AUTO_TEST_CASE(dbwrapper_concurrent_reads)
{
    fs::path ph = m_args.GetDataDirBase() / "dbwrapper_concurrent_reads";
    constexpr size_t CACHE_SIZE{1_MiB};
    constexpr int NUM_KEYS{64};
    constexpr int NUM_THREADS{8};

    CDBWrapper dbw{{.path = ph, .cache_bytes = CACHE_SIZE, .wipe_data = true, .obfuscate = false}};
    std::vector<std::pair<uint8_t, uint256>> key_values;
    key_values.reserve(NUM_KEYS);
    for (uint8_t k = 0; k < NUM_KEYS; ++k) {
        uint256 value{m_rng.rand256()};
        BOOST_CHECK(dbw.Write(k, value));
        key_values.emplace_back(k, value);
    }

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&dbw, &key_values, &failures]() {
            for (const auto& [key, expected] : key_values) {
                uint256 actual;
                if (!dbw.Read(key, actual) || actual != expected) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    BOOST_CHECK_EQUAL(failures.load(), 0);
}

BOOST_AUTO_TEST_CASE(dbwrapper_concurrent_read_write)
{
    fs::path ph = m_args.GetDataDirBase() / "dbwrapper_concurrent_read_write";
    constexpr size_t CACHE_SIZE{1_MiB};
    CDBWrapper dbw{{.path = ph, .cache_bytes = CACHE_SIZE, .wipe_data = true, .obfuscate = false}};
    BOOST_REQUIRE(dbw.Write(uint8_t{0}, m_rng.rand256()));

    std::atomic<bool> stop{false};
    std::atomic<int> read_failures{0};
    std::atomic<int> iterator_failures{0};

    std::thread writer([&]() {
        for (int round = 0; round < 50; ++round) {
            for (uint8_t k = 0; k < 16; ++k) {
                if (!dbw.Write(k, m_rng.rand256())) {
                    read_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        stop.store(true, std::memory_order_release);
    });

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                uint256 value;
                if (!dbw.Read(uint8_t{0}, value)) {
                    read_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::thread iterator_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        std::unique_ptr<CDBIterator> it{dbw.NewIterator()};
        it->Seek(uint8_t{0});
        uint256 snapshot_value;
        if (!it->Valid() || !it->GetValue(snapshot_value)) {
            iterator_failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        while (!stop.load(std::memory_order_acquire)) {
            uint256 current;
            if (!it->GetValue(current) || current != snapshot_value) {
                iterator_failures.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    });

    writer.join();
    for (auto& reader : readers) reader.join();
    iterator_thread.join();

    BOOST_CHECK_EQUAL(read_failures.load(), 0);
    BOOST_CHECK_EQUAL(iterator_failures.load(), 0);
}

BOOST_AUTO_TEST_CASE(dbwrapper_map_growth)
{
    fs::path ph = m_args.GetDataDirBase() / "dbwrapper_map_growth";
    constexpr size_t TINY_MAP{256 << 10};
    CDBWrapper dbw{{.path = ph, .cache_bytes = 1 << 20, .wipe_data = true, .map_size_bytes = TINY_MAP}};

    CDBBatch batch(dbw);
    const std::string payload(32 << 10, 'x');
    for (uint8_t k = 0; k < 32; ++k) {
        batch.Write(k, payload);
    }
    BOOST_CHECK(dbw.WriteBatch(batch));
    std::string read_back;
    BOOST_CHECK(dbw.Read(uint8_t{31}, read_back));
    BOOST_CHECK_EQUAL(read_back, payload);
}

static std::vector<std::pair<std::string, std::string>> SerializeLevelDBEntries(
    const std::vector<std::pair<uint8_t, uint256>>& entries)
{
    std::vector<std::pair<std::string, std::string>> serialized;
    serialized.reserve(entries.size());
    for (const auto& [key, value] : entries) {
        DataStream ssKey, ssValue;
        ssKey << key;
        ssValue << value;
        serialized.emplace_back(ssKey.str(), ssValue.str());
    }
    return serialized;
}

BOOST_AUTO_TEST_CASE(leveldb_migration)
{
    fs::path ph = m_args.GetDataDirBase() / "leveldb_migration";
    fs::remove_all(ph);
    fs::create_directories(ph);

    std::vector<std::pair<uint8_t, uint256>> entries;
    for (uint8_t k = 10; k < 15; ++k) {
        entries.emplace_back(k, m_rng.rand256());
    }
    BOOST_REQUIRE(dbwrapper_leveldb_migrate::WriteLevelDBTestEntries(ph, SerializeLevelDBEntries(entries)));

    size_t final_map_size{0};
    const std::string error = dbwrapper_leveldb_migrate::MigrateLevelDBToLMDB(ph, 0, &final_map_size);
    BOOST_CHECK(error.empty());
    BOOST_CHECK(final_map_size > 0);
    BOOST_CHECK(fs::exists(ph / "data.mdb"));
    BOOST_CHECK(fs::exists(fs::u8path(fs::PathToString(ph) + ".leveldb.bak")));

    CDBWrapper dbw{{.path = ph, .cache_bytes = 1 << 20, .obfuscate = false}};
    for (const auto& [key, expected] : entries) {
        uint256 actual;
        BOOST_CHECK(dbw.Read(key, actual));
        BOOST_CHECK_EQUAL(actual, expected);
    }
}

BOOST_AUTO_TEST_CASE(leveldb_migration_disabled)
{
    const bool old_setting = dbwrapper_settings::g_auto_migrate_leveldb;
    dbwrapper_settings::g_auto_migrate_leveldb = false;

    fs::path ph = m_args.GetDataDirBase() / "leveldb_migration_disabled";
    fs::remove_all(ph);
    fs::create_directories(ph);
    BOOST_REQUIRE(dbwrapper_leveldb_migrate::WriteLevelDBTestEntries(ph, SerializeLevelDBEntries({{11, m_rng.rand256()}})));

    bool threw{false};
    try {
        CDBWrapper{{.path = ph, .cache_bytes = 1 << 20}};
    } catch (const dbwrapper_error&) {
        threw = true;
    }
    BOOST_CHECK(threw);

    dbwrapper_settings::g_auto_migrate_leveldb = old_setting;
}

BOOST_AUTO_TEST_CASE(leveldb_migration_stale_backup)
{
    fs::path ph = m_args.GetDataDirBase() / "leveldb_migration_stale_backup";
    fs::remove_all(ph);
    fs::remove_all(fs::u8path(fs::PathToString(ph) + ".leveldb.bak"));
    fs::create_directories(ph);
    BOOST_REQUIRE(dbwrapper_leveldb_migrate::WriteLevelDBTestEntries(ph, SerializeLevelDBEntries({{12, m_rng.rand256()}})));
    fs::create_directories(fs::u8path(fs::PathToString(ph) + ".leveldb.bak"));

    const std::string error = dbwrapper_leveldb_migrate::MigrateLevelDBToLMDB(ph, 0);
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(error.find("backup already exists") != std::string::npos);
}


BOOST_AUTO_TEST_SUITE_END()
