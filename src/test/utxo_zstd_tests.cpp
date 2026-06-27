// Copyright (c) 2025 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <compress/zstd.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <txdb.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <util/transaction_identifier.h>

#include <boost/test/unit_test.hpp>

#include <vector>

BOOST_FIXTURE_TEST_SUITE(utxo_zstd_tests, BasicTestingSetup)

static std::vector<uint8_t> SerializeCoinBytes(const Coin& coin)
{
    std::vector<uint8_t> bytes;
    VectorWriter{bytes, 0, coin};
    return bytes;
}

static Coin MakeSampleCoin(int height, bool coinbase, CAmount value)
{
    CScript script;
    script << OP_DUP << OP_HASH160 << ParseHex("0102030405060708090a0b0c0d0e0f10111213") << OP_EQUALVERIFY << OP_CHECKSIG;
    return Coin{CTxOut{value, script}, height, coinbase};
}

BOOST_AUTO_TEST_CASE(utxo_round_trip_with_dictionary)
{
    const fs::path dict_path{compress::DefaultUtxoDictionaryPath()};
    BOOST_REQUIRE_MESSAGE(!dict_path.empty(), "UTXO dictionary path must be configured");
    auto dictionary{compress::LoadDictionaryFile(dict_path)};
    BOOST_REQUIRE(dictionary.has_value());
    compress::UtxoZstd zstd{std::move(*dictionary)};
    BOOST_REQUIRE(zstd);

    const Coin coin{MakeSampleCoin(850000, false, 2500000000)};
    const std::vector<uint8_t> serialized{SerializeCoinBytes(coin)};

    std::vector<uint8_t> compressed;
    BOOST_REQUIRE(zstd.Compress(serialized, compressed, DEFAULT_UTXO_ZSTD_LEVEL));
    BOOST_CHECK(!compressed.empty());

    std::vector<uint8_t> decompressed;
    BOOST_REQUIRE(zstd.Decompress(compressed, decompressed, 1 << 20));
    BOOST_CHECK_EQUAL_COLLECTIONS(decompressed.begin(), decompressed.end(), serialized.begin(), serialized.end());

    CoinsViewOptions options;
    options.utxo_zstd = true;
    options.utxo_zstd_level = DEFAULT_UTXO_ZSTD_LEVEL;
    options.utxo_zstd_dict_path = dict_path;

    CCoinsViewDB db{{.path = "utxo-zstd-test", .cache_bytes = 1 << 20, .memory_only = true, .obfuscate = true}, options};
    const Txid txid{Txid::FromUint256(uint256::ONE)};
    const COutPoint outpoint{txid, 1};
    CCoinsViewCache cache{&db, /*deterministic=*/true};
    cache.SetBestBlock(uint256::ONE);
    cache.AddCoin(outpoint, Coin{coin}, /*possible_overwrite=*/false);

    BOOST_REQUIRE(cache.Flush());

    const auto loaded{db.GetCoin(outpoint)};
    BOOST_REQUIRE(loaded.has_value());
    BOOST_CHECK_EQUAL(loaded->out.nValue, coin.out.nValue);
    BOOST_CHECK_EQUAL(loaded->nHeight, coin.nHeight);
    BOOST_CHECK_EQUAL(loaded->fCoinBase, coin.fCoinBase);
    BOOST_CHECK(loaded->out.scriptPubKey == coin.out.scriptPubKey);
}

BOOST_AUTO_TEST_CASE(utxo_mixed_legacy_and_compressed_reads)
{
    const fs::path dict_path{compress::DefaultUtxoDictionaryPath()};
    BOOST_REQUIRE_MESSAGE(!dict_path.empty(), "UTXO dictionary path must be configured");
    auto dictionary{compress::LoadDictionaryFile(dict_path)};
    BOOST_REQUIRE(dictionary.has_value());

    CoinsViewOptions options;
    options.utxo_zstd = false;
    CCoinsViewDB db{{.path = "utxo-mixed-test", .cache_bytes = 1 << 20, .memory_only = true, .obfuscate = true}, options};

    const Coin legacy_coin{MakeSampleCoin(42, true, 500000000)};
    const Coin compressed_coin{MakeSampleCoin(900000, false, 100000)};

    const Txid txid{Txid::FromUint256(uint256::ONE)};
    CCoinsViewCache cache{&db, /*deterministic=*/true};
    cache.SetBestBlock(uint256::ONE);
    cache.AddCoin(COutPoint{txid, 0}, Coin{legacy_coin}, /*possible_overwrite=*/false);
    BOOST_REQUIRE(cache.Flush());

    options.utxo_zstd = true;
    options.utxo_zstd_level = DEFAULT_UTXO_ZSTD_LEVEL;
    options.utxo_zstd_dict_path = dict_path;
    CCoinsViewDB compressed_db{{.path = "utxo-mixed-test", .cache_bytes = 1 << 20, .memory_only = true, .obfuscate = true}, options};
    CCoinsViewCache compressed_cache{&compressed_db, /*deterministic=*/true};
    compressed_cache.SetBestBlock(uint256::ONE);
    compressed_cache.AddCoin(COutPoint{txid, 2}, Coin{compressed_coin}, /*possible_overwrite=*/false);
    BOOST_REQUIRE(compressed_cache.Flush());

    const auto legacy_loaded{db.GetCoin(COutPoint{txid, 0})};
    const auto compressed_loaded{compressed_db.GetCoin(COutPoint{txid, 2})};
    BOOST_REQUIRE(legacy_loaded.has_value());
    BOOST_REQUIRE(compressed_loaded.has_value());
    BOOST_CHECK_EQUAL(legacy_loaded->out.nValue, legacy_coin.out.nValue);
    BOOST_CHECK_EQUAL(compressed_loaded->out.nValue, compressed_coin.out.nValue);
}

BOOST_AUTO_TEST_CASE(utxo_legacy_prefix_collision_with_dictionary)
{
    const fs::path dict_path{compress::DefaultUtxoDictionaryPath()};
    BOOST_REQUIRE_MESSAGE(!dict_path.empty(), "UTXO dictionary path must be configured");

    CScript script;
    script << OP_1;
    const Coin legacy_coin{CTxOut{1, script}, 0, true};
    const std::vector<uint8_t> legacy_bytes{SerializeCoinBytes(legacy_coin)};
    BOOST_REQUIRE_GE(legacy_bytes.size(), 2);
    BOOST_CHECK_EQUAL(legacy_bytes[0], 0x01);
    BOOST_CHECK_EQUAL(legacy_bytes[1], 0x01);

    CoinsViewOptions options;
    options.utxo_zstd = false;
    options.utxo_zstd_dict_path = dict_path;
    CCoinsViewDB db{{.path = "utxo-collision-test", .cache_bytes = 1 << 20, .memory_only = true, .obfuscate = true}, options};

    const Txid txid{Txid::FromUint256(uint256::ONE)};
    CCoinsViewCache cache{&db, /*deterministic=*/true};
    cache.SetBestBlock(uint256::ONE);
    cache.AddCoin(COutPoint{txid, 0}, Coin{legacy_coin}, /*possible_overwrite=*/false);
    BOOST_REQUIRE(cache.Flush());

    const auto loaded{db.GetCoin(COutPoint{txid, 0})};
    BOOST_REQUIRE(loaded.has_value());
    BOOST_CHECK_EQUAL(loaded->out.nValue, 1);
    BOOST_CHECK_EQUAL(loaded->nHeight, 0u);
    BOOST_CHECK(loaded->IsCoinBase());
}

BOOST_AUTO_TEST_CASE(utxo_cursor_reads_compressed_entries)
{
    const fs::path dict_path{compress::DefaultUtxoDictionaryPath()};
    BOOST_REQUIRE_MESSAGE(!dict_path.empty(), "UTXO dictionary path must be configured");

    CoinsViewOptions options;
    options.utxo_zstd = true;
    options.utxo_zstd_level = DEFAULT_UTXO_ZSTD_LEVEL;
    options.utxo_zstd_dict_path = dict_path;
    CCoinsViewDB db{{.path = "utxo-cursor-test", .cache_bytes = 1 << 20, .memory_only = true, .obfuscate = true}, options};

    const Txid txid{Txid::FromUint256(uint256::ONE)};
    const Coin coin{MakeSampleCoin(12345, false, 99999)};
    CCoinsViewCache cache{&db, /*deterministic=*/true};
    cache.SetBestBlock(uint256::ONE);
    cache.AddCoin(COutPoint{txid, 0}, Coin{coin}, /*possible_overwrite=*/false);
    BOOST_REQUIRE(cache.Flush());

    const auto cursor{db.Cursor()};
    BOOST_REQUIRE(cursor);
    BOOST_REQUIRE(cursor->Valid());
    COutPoint key;
    Coin value;
    BOOST_REQUIRE(cursor->GetKey(key));
    BOOST_REQUIRE(cursor->GetValue(value));
    BOOST_CHECK_EQUAL(key.hash, txid);
    BOOST_CHECK_EQUAL(key.n, 0u);
    BOOST_CHECK_EQUAL(value.out.nValue, coin.out.nValue);
}

BOOST_AUTO_TEST_CASE(utxo_compression_is_deterministic)
{
    const fs::path dict_path{compress::DefaultUtxoDictionaryPath()};
    BOOST_REQUIRE_MESSAGE(!dict_path.empty(), "UTXO dictionary path must be configured");
    auto dictionary{compress::LoadDictionaryFile(dict_path)};
    BOOST_REQUIRE(dictionary.has_value());
    compress::UtxoZstd zstd{std::move(*dictionary)};

    const std::vector<uint8_t> serialized{SerializeCoinBytes(MakeSampleCoin(500000, false, 1000000))};
    std::vector<uint8_t> first;
    std::vector<uint8_t> second;
    BOOST_REQUIRE(zstd.Compress(serialized, first, DEFAULT_UTXO_ZSTD_LEVEL));
    BOOST_REQUIRE(zstd.Compress(serialized, second, DEFAULT_UTXO_ZSTD_LEVEL));
    BOOST_CHECK_EQUAL_COLLECTIONS(first.begin(), first.end(), second.begin(), second.end());
}

BOOST_AUTO_TEST_CASE(utxo_skips_compression_when_not_smaller)
{
    const fs::path dict_path{compress::DefaultUtxoDictionaryPath()};
    BOOST_REQUIRE_MESSAGE(!dict_path.empty(), "UTXO dictionary path must be configured");
    auto dictionary{compress::LoadDictionaryFile(dict_path)};
    BOOST_REQUIRE(dictionary.has_value());
    compress::UtxoZstd zstd{std::move(*dictionary)};

    const Coin coin{MakeSampleCoin(1, false, 123456789)};
    const std::vector<uint8_t> serialized{SerializeCoinBytes(coin)};
    std::vector<uint8_t> compressed;
    BOOST_REQUIRE(zstd.Compress(serialized, compressed, DEFAULT_UTXO_ZSTD_LEVEL));
    if (2 + compressed.size() < serialized.size()) {
        BOOST_TEST_MESSAGE("Skipping expand-only check: dictionary shrinks this sample");
        return;
    }

    CoinsViewOptions options;
    options.utxo_zstd = true;
    options.utxo_zstd_level = DEFAULT_UTXO_ZSTD_LEVEL;
    options.utxo_zstd_dict_path = dict_path;
    CCoinsViewDB db{{.path = "utxo-incompressible-test", .cache_bytes = 1 << 20, .memory_only = true, .obfuscate = true}, options};

    const Txid txid{Txid::FromUint256(uint256::ONE)};
    CCoinsViewCache cache{&db, /*deterministic=*/true};
    cache.SetBestBlock(uint256::ONE);
    cache.AddCoin(COutPoint{txid, 0}, Coin{coin}, /*possible_overwrite=*/false);
    BOOST_REQUIRE(cache.Flush());

    const auto loaded{db.GetCoin(COutPoint{txid, 0})};
    BOOST_REQUIRE(loaded.has_value());
    BOOST_CHECK_EQUAL(loaded->out.nValue, coin.out.nValue);
    const std::vector<uint8_t> loaded_bytes{SerializeCoinBytes(*loaded)};
    BOOST_CHECK_EQUAL_COLLECTIONS(loaded_bytes.begin(), loaded_bytes.end(), serialized.begin(), serialized.end());
}

BOOST_AUTO_TEST_SUITE_END()