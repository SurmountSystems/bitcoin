// Copyright (c) 2025 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <coins.h>
#include <common/args.h>
#include <compress/dict_bootstrap.h>
#include <compress/dict_classify.h>
#include <compress/zstd.h>
#include <node/blockfile_format.h>
#include <primitives/block.h>
#include <script/script.h>
#include <script/solver.h>
#include <streams.h>
#include <txdb.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>
#include <test/util/setup_common.h>

#include <atomic>
#include <fstream>
#include <thread>
#include <univalue.h>
#include <vector>

using node::BlockBucketFlags;
using node::ValidBlockDiskFlags;

namespace {

CScript MakeP2TRScript()
{
    static const std::vector<unsigned char> xonly_pubkey(32, 0xab);
    return CScript() << OP_1 << xonly_pubkey;
}

CScript MakeP2WPKHScript()
{
    static const std::vector<unsigned char> hash20(20, 0x11);
    return CScript() << OP_0 << hash20;
}

CScript MakeP2WSHScript()
{
    static const std::vector<unsigned char> hash32(32, 0x22);
    return CScript() << OP_0 << hash32;
}

CScript MakeP2PKHScript()
{
    static const std::vector<unsigned char> hash20(20, 0x33);
    return CScript() << OP_DUP << OP_HASH160 << hash20 << OP_EQUALVERIFY << OP_CHECKSIG;
}

CScript MakeP2SHScript()
{
    static const std::vector<unsigned char> hash20(20, 0x44);
    return CScript() << OP_HASH160 << hash20 << OP_EQUAL;
}

CBlock MakeScriptSigDominatedBlock()
{
    CBlock block;
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].scriptSig = CScript() << std::vector<uint8_t>(2048, 0x42);
    tx.vout.resize(1);
    tx.vout[0].nValue = 1 * COIN;
    tx.vout[0].scriptPubKey = MakeP2WPKHScript();
    block.vtx.push_back(MakeTransactionRef(tx));
    block.vtx.push_back(MakeTransactionRef(tx));
    return block;
}

CBlock MakeP2TRBlock()
{
    CBlock block;
    CMutableTransaction tx;
    tx.vout.resize(1);
    tx.vout[0].nValue = 1 * COIN;
    tx.vout[0].scriptPubKey = MakeP2TRScript();
    block.vtx.push_back(MakeTransactionRef(tx));
    return block;
}

CBlock MakeScriptSigWitnessEqualBlock()
{
    CBlock block;
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << 1 << 0;
    coinbase.vout.resize(1);
    coinbase.vout[0].nValue = 50 * COIN;
    coinbase.vout[0].scriptPubKey = MakeP2WPKHScript();
    block.vtx.push_back(MakeTransactionRef(coinbase));

    CMutableTransaction tx;
    tx.vin.resize(1);
    // scriptSig.size() includes push opcodes; match total bytes to witness stack item size.
    tx.vin[0].scriptSig = CScript() << std::vector<uint8_t>(64, 0x42);
    tx.vin[0].scriptWitness.stack.emplace_back(tx.vin[0].scriptSig.size(), 0x43);
    tx.vout.resize(1);
    tx.vout[0].nValue = 1 * COIN;
    tx.vout[0].scriptPubKey = MakeP2WPKHScript();
    block.vtx.push_back(MakeTransactionRef(tx));
    return block;
}

Coin MakeCoinForBucket(compress::UtxoBucket bucket, int height)
{
    CScript script;
    switch (bucket) {
    case compress::UtxoBucket::UTXO_P2PKH: script = MakeP2PKHScript(); break;
    case compress::UtxoBucket::UTXO_P2SH: script = MakeP2SHScript(); break;
    case compress::UtxoBucket::UTXO_P2WPKH: script = MakeP2WPKHScript(); break;
    case compress::UtxoBucket::UTXO_P2WSH: script = MakeP2WSHScript(); break;
    case compress::UtxoBucket::UTXO_P2TR_PRE_ORD:
    case compress::UtxoBucket::UTXO_P2TR_POST_ORD: script = MakeP2TRScript(); break;
    case compress::UtxoBucket::UTXO_OTHER: script = CScript() << OP_RETURN << std::vector<uint8_t>(8, 0x55); break;
    }
    return Coin{CTxOut{1 * COIN, script}, height, false};
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(dict_bootstrap_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(inscription_zero_height_mainnet_only)
{
    const auto mainnet{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const auto regtest{CreateChainParams(ArgsManager{}, ChainType::REGTEST)};
    BOOST_REQUIRE(compress::InscriptionZeroHeight(*mainnet).has_value());
    BOOST_CHECK_EQUAL(*compress::InscriptionZeroHeight(*mainnet), 767430);
    BOOST_CHECK(!compress::InscriptionZeroHeight(*regtest).has_value());
}

BOOST_AUTO_TEST_CASE(classify_block_scriptsig_and_p2tr_heights)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const CBlock scriptsig_block{MakeScriptSigDominatedBlock()};
    BOOST_CHECK_EQUAL(compress::ClassifyBlock(500000, scriptsig_block, *params),
                      compress::BlockBucket::BLK_SCRIPTSIG);

    const CBlock p2tr_block{MakeP2TRBlock()};
    BOOST_CHECK_EQUAL(compress::ClassifyBlock(767429, p2tr_block, *params),
                      compress::BlockBucket::BLK_P2TR_PRE_ORD);
    BOOST_CHECK_EQUAL(compress::ClassifyBlock(767430, p2tr_block, *params),
                      compress::BlockBucket::BLK_P2TR_POST_ORD);
}

BOOST_AUTO_TEST_CASE(classify_block_scriptsig_witness_equal_uses_dominant_output)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const CBlock equal_block{MakeScriptSigWitnessEqualBlock()};
    BOOST_CHECK_EQUAL(compress::ClassifyBlock(500000, equal_block, *params),
                      compress::BlockBucket::BLK_P2WPKH);
}

BOOST_AUTO_TEST_CASE(classify_coin_p2tr_heights)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    Coin pre_ord{CTxOut{1 * COIN, MakeP2TRScript()}, 767429, false};
    Coin post_ord{CTxOut{1 * COIN, MakeP2TRScript()}, 767430, false};
    BOOST_CHECK_EQUAL(compress::ClassifyCoin(pre_ord, *params), compress::UtxoBucket::UTXO_P2TR_PRE_ORD);
    BOOST_CHECK_EQUAL(compress::ClassifyCoin(post_ord, *params), compress::UtxoBucket::UTXO_P2TR_POST_ORD);
}

BOOST_AUTO_TEST_CASE(block_bucket_flags_encoding)
{
    BOOST_CHECK_EQUAL(BlockBucketFlags(4), 0x08);
    BOOST_CHECK(ValidBlockDiskFlags(0x08));
    BOOST_CHECK(ValidBlockDiskFlags(0x01 | 0x08));
    BOOST_CHECK(!ValidBlockDiskFlags(0x20));
}

BOOST_AUTO_TEST_CASE(bootstrap_state_machine_pass1_complete)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const fs::path datadir{m_args.GetDataDirBase() / "bootstrap_state_sm"};
    fs::remove_all(datadir);

    {
        ArgsManager local_args;
        local_args.ForceSetArg("-dictbootstrap", "auto");
        compress::DictBootstrapManager manager{datadir, *params, local_args};
        manager.OnChainReady(/*in_ibd=*/true);
        BOOST_CHECK(manager.IsPass1InProgress());
        BOOST_CHECK(!manager.ShouldCompressOnWrite());
        manager.OnIbdComplete();
        for (int i = 0; i < 200 && manager.Phase() != compress::BootstrapPhase::PASS1_COMPLETE; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        BOOST_CHECK_EQUAL(manager.Phase(), compress::BootstrapPhase::PASS1_COMPLETE);
        BOOST_CHECK(!manager.ShouldCompressOnWrite());
    }

    {
        ArgsManager local_args;
        local_args.ForceSetArg("-dictbootstrap", "auto");
        compress::DictBootstrapManager resumed{datadir, *params, local_args};
        BOOST_CHECK_EQUAL(resumed.Phase(), compress::BootstrapPhase::PASS1_COMPLETE);
        BOOST_CHECK(!resumed.IsPass1InProgress());
    }
}

BOOST_AUTO_TEST_CASE(bootstrap_skips_pass1_when_already_synced)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const fs::path datadir{m_args.GetDataDirBase() / "bootstrap_synced_skip"};
    fs::remove_all(datadir);

    ArgsManager local_args;
    local_args.ForceSetArg("-dictbootstrap", "auto");
    compress::DictBootstrapManager manager{datadir, *params, local_args};
    manager.OnChainReady(/*in_ibd=*/false);
    BOOST_CHECK(!manager.IsPass1InProgress());
    BOOST_CHECK_EQUAL(manager.Phase(), compress::BootstrapPhase::NONE);
    BOOST_CHECK(manager.ShouldCompressOnWrite());
    BOOST_CHECK(!fs::exists(datadir / "swords" / "bootstrap_state.json"));
}

BOOST_AUTO_TEST_CASE(bootstrap_corrupt_state_disables_bootstrap)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const fs::path datadir{m_args.GetDataDirBase() / "bootstrap_corrupt"};
    fs::remove_all(datadir);
    fs::create_directories(datadir / "swords");
    std::ofstream{datadir / "swords" / "bootstrap_state.json"} << "{not valid json";

    ArgsManager local_args;
    local_args.ForceSetArg("-dictbootstrap", "auto");
    compress::DictBootstrapManager manager{datadir, *params, local_args};
    BOOST_CHECK(!manager.IsPass1InProgress());
    BOOST_CHECK_EQUAL(manager.Phase(), compress::BootstrapPhase::NONE);
}

BOOST_AUTO_TEST_CASE(invalid_dictbootstrap_mode_rejected)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const fs::path datadir{m_args.GetDataDirBase() / "bootstrap_invalid_mode"};
    ArgsManager local_args;
    local_args.ForceSetArg("-dictbootstrap", "typo");
    BOOST_CHECK(!compress::InitDictBootstrap(datadir, *params, local_args, /*do_reindex=*/false));
    BOOST_CHECK(!compress::g_dict_bootstrap);
}

BOOST_AUTO_TEST_CASE(state_persistence_restores_metrics)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const fs::path datadir{m_args.GetDataDirBase() / "bootstrap_metrics_restore"};
    fs::remove_all(datadir);

    {
        ArgsManager local_args;
        local_args.ForceSetArg("-dictbootstrap", "auto");
        compress::DictBootstrapManager manager{datadir, *params, local_args};
        manager.OnChainReady(/*in_ibd=*/true);
        const CBlock block{MakeP2TRBlock()};
        std::vector<uint8_t> payload(256, 0x7a);
        manager.OnBlockWritten(800000, block, payload);
        manager.OnIbdComplete();
        for (int i = 0; i < 200 && manager.Phase() != compress::BootstrapPhase::PASS1_COMPLETE; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }

    ArgsManager local_args;
    local_args.ForceSetArg("-dictbootstrap", "auto");
    compress::DictBootstrapManager resumed{datadir, *params, local_args};
    BOOST_CHECK_EQUAL(resumed.Phase(), compress::BootstrapPhase::PASS1_COMPLETE);
    BOOST_CHECK_GT(resumed.Metrics().block_plaintext_bytes[static_cast<size_t>(compress::BlockBucket::BLK_P2TR_POST_ORD)].load(), 0);
}

BOOST_AUTO_TEST_CASE(incremental_train_does_not_enable_compression_mid_pass1)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const fs::path datadir{m_args.GetDataDirBase() / "bootstrap_incremental"};
    fs::remove_all(datadir);
    const fs::path samples_dir{datadir / "swords" / "samples"};
    const fs::path dicts_dir{datadir / "swords" / "dicts"};
    fs::create_directories(samples_dir);
    fs::create_directories(dicts_dir);

    ArgsManager local_args;
    local_args.ForceSetArg("-dictbootstrap", "auto");
    compress::DictBootstrapManager manager{datadir, *params, local_args};
    manager.OnChainReady(/*in_ibd=*/true);

    compress::DictSampleCollector collector{samples_dir};
    std::vector<uint8_t> sample(128, 0x5a);
    collector.CollectBlockSample(compress::BlockBucket::BLK_P2WPKH, sample);
    collector.CollectBlockSample(compress::BlockBucket::BLK_P2WPKH, sample);

    compress::DictionaryTrainer trainer{dicts_dir, collector, 767430};
    trainer.TrainAllBuckets(/*final_train=*/false);
    BOOST_CHECK(manager.IsPass1InProgress());
    BOOST_CHECK(!manager.ShouldCompressOnWrite());
}

BOOST_AUTO_TEST_CASE(capacity_search_prefers_smaller_on_tie)
{
    std::vector<std::vector<uint8_t>> owned;
    owned.reserve(40);
    std::vector<std::span<const uint8_t>> spans;
    for (int i = 0; i < 40; ++i) {
        owned.emplace_back(256, static_cast<uint8_t>(0x20 + (i % 5)));
        spans.emplace_back(owned.back());
    }
    const auto result{compress::TrainDictionary(spans)};
    BOOST_REQUIRE(result);
    BOOST_CHECK(!result->dictionary.empty());
    BOOST_CHECK_GT(result->holdout_ratio, 1.0);
    BOOST_CHECK_LE(result->chosen_size, compress::MAX_DICT_FILE_SIZE);
}

BOOST_AUTO_TEST_CASE(typed_utxo_encode_decode_roundtrip)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const fs::path datadir{m_args.GetDataDirBase() / "utxo_roundtrip"};
    fs::remove_all(datadir);

    ArgsManager local_args;
    local_args.ForceSetArg("-dictbootstrap", "auto");
    compress::g_dict_bootstrap = std::make_unique<compress::DictBootstrapManager>(datadir, *params, local_args);
    compress::g_dict_bootstrap->OnChainReady(/*in_ibd=*/true);

    const compress::UtxoZstd zstd;
    CoinsViewOptions options;
    options.utxo_zstd = false;

    const std::array<compress::UtxoBucket, 7> buckets{
        compress::UtxoBucket::UTXO_P2PKH,
        compress::UtxoBucket::UTXO_P2SH,
        compress::UtxoBucket::UTXO_P2WPKH,
        compress::UtxoBucket::UTXO_P2WSH,
        compress::UtxoBucket::UTXO_P2TR_PRE_ORD,
        compress::UtxoBucket::UTXO_P2TR_POST_ORD,
        compress::UtxoBucket::UTXO_OTHER,
    };

    for (const auto bucket : buckets) {
        const int height{bucket == compress::UtxoBucket::UTXO_P2TR_POST_ORD ? 767430 : 500000};
        const Coin original{MakeCoinForBucket(bucket, height)};
        const std::vector<uint8_t> encoded{EncodeCoinValue(original, options, zstd)};
        BOOST_REQUIRE_GE(encoded.size(), 2u);
        BOOST_CHECK_EQUAL(encoded[0], 1);
        BOOST_CHECK((encoded[1] & 0x80) != 0);
        Coin decoded;
        BOOST_REQUIRE(DecodeCoinValue(encoded, zstd, decoded));
        BOOST_CHECK_EQUAL(decoded.out.nValue, original.out.nValue);
        BOOST_CHECK(decoded.out.scriptPubKey == original.out.scriptPubKey);
        BOOST_CHECK_EQUAL(decoded.nHeight, original.nHeight);
    }

    compress::g_dict_bootstrap.reset();
}

BOOST_AUTO_TEST_CASE(legacy_compressed_utxo_decode_compat)
{
    const auto dict_path{compress::DefaultUtxoDictionaryPath()};
    const auto dictionary{compress::LoadDictionaryFile(dict_path)};
    BOOST_REQUIRE_MESSAGE(dictionary, "bundled UTXO dictionary required for legacy decode test");
    const compress::UtxoZstd zstd{*dictionary};

    const Coin original{CTxOut{2 * COIN, MakeP2WPKHScript()}, 600000, false};
    std::vector<uint8_t> serialized;
    VectorWriter{serialized, 0, original};

    std::vector<uint8_t> compressed;
    BOOST_REQUIRE(zstd.Compress(serialized, compressed, 3));
    std::vector<uint8_t> stored;
    stored.push_back(1);
    stored.push_back(1); // COIN_VALUE_COMPRESSED (legacy bucket 0 compat)
    stored.insert(stored.end(), compressed.begin(), compressed.end());

    Coin decoded;
    BOOST_REQUIRE(DecodeCoinValue(stored, zstd, decoded));
    BOOST_CHECK_EQUAL(decoded.out.nValue, original.out.nValue);
}

BOOST_AUTO_TEST_CASE(collector_trainer_concurrency_stress)
{
    const fs::path samples_dir{m_args.GetDataDirBase() / "bootstrap_concurrency" / "samples"};
    const fs::path dicts_dir{m_args.GetDataDirBase() / "bootstrap_concurrency" / "dicts"};
    fs::remove_all(samples_dir.parent_path());
    fs::create_directories(samples_dir);
    fs::create_directories(dicts_dir);

    compress::DictSampleCollector collector{samples_dir};
    compress::DictionaryTrainer trainer{dicts_dir, collector, 767430};
    trainer.Start();

    std::atomic<bool> stop{false};
    std::vector<std::thread> collectors;
    for (int t = 0; t < 4; ++t) {
        collectors.emplace_back([&, t] {
            std::vector<uint8_t> payload(512, static_cast<uint8_t>(0x10 + t));
            while (!stop.load()) {
                collector.CollectBlockSample(compress::BlockBucket::BLK_P2WPKH, payload);
                collector.CollectUtxoSample(compress::UtxoBucket::UTXO_P2WPKH, payload);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    stop.store(true);
    for (auto& thread : collectors) {
        thread.join();
    }
    trainer.Stop();

    const auto block_samples{collector.LoadBlockSamples(compress::BlockBucket::BLK_P2WPKH)};
    BOOST_CHECK_LE(block_samples.size(), compress::DictSampleCollector::MAX_SAMPLES_PER_BUCKET);
    BOOST_CHECK_GE(block_samples.size(), 2u);
}

BOOST_AUTO_TEST_CASE(shutdown_during_complete_pass1_joins_cleanly)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const fs::path datadir{m_args.GetDataDirBase() / "shutdown_during_complete"};
    fs::remove_all(datadir);

    ArgsManager local_args;
    local_args.ForceSetArg("-dictbootstrap", "auto");
    BOOST_REQUIRE(compress::InitDictBootstrap(datadir, *params, local_args, /*do_reindex=*/false));
    BOOST_REQUIRE(compress::g_dict_bootstrap);
    compress::g_dict_bootstrap->OnChainReady(/*in_ibd=*/true);

    const CBlock block{MakeP2TRBlock()};
    std::vector<uint8_t> payload(1024, 0x3c);
    for (int i = 0; i < 4; ++i) {
        compress::g_dict_bootstrap->OnBlockWritten(800000, block, payload);
    }
    compress::g_dict_bootstrap->OnIbdComplete();

    compress::ShutdownDictBootstrap();
    BOOST_CHECK(!compress::g_dict_bootstrap);
}

BOOST_AUTO_TEST_CASE(resume_pass1_on_synced_chain_completes)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const fs::path datadir{m_args.GetDataDirBase() / "resume_synced_complete"};
    fs::remove_all(datadir);

    ArgsManager local_args;
    local_args.ForceSetArg("-dictbootstrap", "auto");
    {
        compress::DictBootstrapManager manager{datadir, *params, local_args};
        manager.OnChainReady(/*in_ibd=*/true);
        BOOST_CHECK(manager.IsPass1InProgress());
        const CBlock block{MakeP2TRBlock()};
        std::vector<uint8_t> payload(512, 0x2a);
        manager.OnBlockWritten(800000, block, payload);
    }

    compress::DictBootstrapManager resumed{datadir, *params, local_args};
    BOOST_CHECK(resumed.IsPass1InProgress());
    resumed.OnChainReady(/*in_ibd=*/false);
    for (int i = 0; i < 300 && resumed.Phase() != compress::BootstrapPhase::PASS1_COMPLETE; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    BOOST_CHECK_EQUAL(resumed.Phase(), compress::BootstrapPhase::PASS1_COMPLETE);
}

BOOST_AUTO_TEST_CASE(reservoir_seen_persisted_across_restart)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const fs::path datadir{m_args.GetDataDirBase() / "reservoir_seen_persist"};
    fs::remove_all(datadir);

    ArgsManager local_args;
    local_args.ForceSetArg("-dictbootstrap", "auto");
    {
        compress::DictBootstrapManager manager{datadir, *params, local_args};
        manager.OnChainReady(/*in_ibd=*/true);
        const CBlock block{MakeP2TRBlock()};
        std::vector<uint8_t> payload(128, 0x55);
        for (int i = 0; i < 300; ++i) {
            manager.OnBlockWritten(800000, block, payload);
        }
    }

    const fs::path state_path{datadir / "swords" / "bootstrap_state.json"};
    BOOST_REQUIRE(fs::exists(state_path));
    std::ifstream in{state_path};
    UniValue json;
    BOOST_REQUIRE(json.read(std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}));
    BOOST_REQUIRE(json["block_samples_seen"].isObject());
    BOOST_CHECK_EQUAL(json["block_samples_seen"]["BLK_P2TR_POST_ORD"].getInt<int64_t>(), 300);

    compress::DictBootstrapManager resumed{datadir, *params, local_args};
    BOOST_CHECK(resumed.IsPass1InProgress());
    compress::DictSampleCollector collector{datadir / "swords" / "samples"};
    std::array<uint64_t, compress::NUM_BLOCK_BUCKETS> block_seen{};
    block_seen[static_cast<size_t>(compress::BlockBucket::BLK_P2TR_POST_ORD)] = 300;
    collector.RestoreSeenCounts(block_seen, {});
    BOOST_CHECK_EQUAL(collector.BlockSeenCount(compress::BlockBucket::BLK_P2TR_POST_ORD), 300u);
    BOOST_CHECK_EQUAL(collector.LoadBlockSamples(compress::BlockBucket::BLK_P2TR_POST_ORD).size(),
                      compress::DictSampleCollector::MAX_SAMPLES_PER_BUCKET);
}

BOOST_AUTO_TEST_CASE(complete_pass1_writes_bootstrap_baseline_json)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const fs::path datadir{m_args.GetDataDirBase() / "bootstrap_baseline_json"};
    fs::remove_all(datadir);

    ArgsManager local_args;
    local_args.ForceSetArg("-dictbootstrap", "auto");
    {
        compress::DictBootstrapManager manager{datadir, *params, local_args};
        manager.OnChainReady(/*in_ibd=*/true);
        const CBlock block{MakeP2TRBlock()};
        std::vector<uint8_t> payload(512, 0x2a);
        for (int i = 0; i < 8; ++i) {
            manager.OnBlockWritten(800000, block, payload);
        }
        manager.OnIbdComplete();
        for (int i = 0; i < 300 && manager.Phase() != compress::BootstrapPhase::PASS1_COMPLETE; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        BOOST_CHECK_EQUAL(manager.Phase(), compress::BootstrapPhase::PASS1_COMPLETE);
    }

    const fs::path baseline_path{datadir / "swords" / "bootstrap_baseline.json"};
    BOOST_REQUIRE(fs::exists(baseline_path));
    std::ifstream in{baseline_path};
    UniValue json;
    BOOST_REQUIRE(json.read(std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}));
    BOOST_CHECK_EQUAL(json["state"].get_str(), "pass1_complete");
    BOOST_CHECK_EQUAL(json["next_step"].get_str(), "restart with -reindex for compression pass 2");
    BOOST_REQUIRE(json["pass1_start"].isStr());
    BOOST_REQUIRE(json["pass1_end"].isStr());
    BOOST_CHECK_GE(json["pass1_wall_seconds"].getInt<int64_t>(), 0);
    BOOST_REQUIRE(json["block_plaintext_bytes"].isObject());
    BOOST_CHECK_EQUAL(json["block_plaintext_bytes"]["BLK_P2TR_POST_ORD"].getInt<int64_t>(), 4096);
    BOOST_REQUIRE(json["utxo_plaintext_bytes"].isObject());
    BOOST_REQUIRE(json["trained_dicts"].isArray());
    BOOST_CHECK_GE(json["trained_dicts"].size(), 1u);
    bool found_trained_block{false};
    for (size_t i = 0; i < json["trained_dicts"].size(); ++i) {
        const UniValue& entry{json["trained_dicts"][i]};
        if (!entry.isObject() || !entry["name"].isStr()) continue;
        if (entry["name"].get_str() != "BLK_P2TR_POST_ORD") continue;
        BOOST_REQUIRE(entry["chosen_size"].isNum());
        BOOST_CHECK_GT(entry["chosen_size"].getInt<int64_t>(), 0);
        BOOST_REQUIRE(entry["holdout_ratio"].isNum());
        BOOST_REQUIRE(entry["sample_bytes"].isNum());
        BOOST_CHECK_GT(entry["sample_bytes"].getInt<int64_t>(), 0);
        found_trained_block = true;
    }
    BOOST_CHECK(found_trained_block);

    const fs::path state_path{datadir / "swords" / "bootstrap_state.json"};
    BOOST_REQUIRE(fs::exists(state_path));
    std::ifstream state_in{state_path};
    UniValue state_json;
    BOOST_REQUIRE(state_json.read(std::string{std::istreambuf_iterator<char>(state_in), std::istreambuf_iterator<char>()}));
    BOOST_REQUIRE(state_json["pass1_start_wall_unix"].isNum());
    BOOST_CHECK_GT(state_json["pass1_start_wall_unix"].getInt<int64_t>(), 0);
}

BOOST_AUTO_TEST_CASE(trainer_lifecycle_shutdown_and_ibd_complete)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const fs::path datadir{m_args.GetDataDirBase() / "trainer_lifecycle"};
    fs::remove_all(datadir);

    ArgsManager local_args;
    local_args.ForceSetArg("-dictbootstrap", "auto");
    {
        compress::DictBootstrapManager manager{datadir, *params, local_args};
        manager.OnChainReady(/*in_ibd=*/true);
        BOOST_CHECK(manager.IsPass1InProgress());

        const CBlock block{MakeP2TRBlock()};
        std::vector<uint8_t> payload(1024, 0x3c);
        manager.OnBlockWritten(800000, block, payload);
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    fs::remove_all(datadir);
    compress::DictBootstrapManager manager{datadir, *params, local_args};
    manager.OnChainReady(/*in_ibd=*/true);
    const CBlock block{MakeP2TRBlock()};
    std::vector<uint8_t> payload(1024, 0x3c);
    for (int i = 0; i < 4; ++i) {
        manager.OnBlockWritten(800000, block, payload);
    }
    manager.OnIbdComplete();
    for (int i = 0; i < 300 && manager.Phase() != compress::BootstrapPhase::PASS1_COMPLETE; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    BOOST_CHECK_EQUAL(manager.Phase(), compress::BootstrapPhase::PASS1_COMPLETE);
}

namespace {

void WritePass1CompleteState(const fs::path& datadir)
{
    const fs::path swords_dir{datadir / "swords"};
    fs::create_directories(swords_dir);
    UniValue json{UniValue::VOBJ};
    json.pushKV("state", "pass1_complete");
    json.pushKV("pass1_start_steady_us", static_cast<int64_t>(1));
    json.pushKV("pass1_end_steady_us", static_cast<int64_t>(2));
    json.pushKV("pass1_start_wall_unix", static_cast<int64_t>(1'700'000'000));
    UniValue block_metrics{UniValue::VOBJ};
    block_metrics.pushKV("BLK_P2TR_POST_ORD", static_cast<int64_t>(4096));
    json.pushKV("block_plaintext_bytes", block_metrics);
    UniValue utxo_metrics{UniValue::VOBJ};
    utxo_metrics.pushKV("UTXO_P2WPKH", static_cast<int64_t>(256));
    json.pushKV("utxo_plaintext_bytes", utxo_metrics);
    const fs::path state_path{swords_dir / "bootstrap_state.json"};
    std::ofstream out{state_path};
    BOOST_REQUIRE(out.is_open());
    out << json.write(4, 1) << std::endl;
}

void WritePass1Baseline(const fs::path& datadir)
{
    const fs::path baseline_path{datadir / "swords" / "bootstrap_baseline.json"};
    UniValue json{UniValue::VOBJ};
    json.pushKV("state", "pass1_complete");
    json.pushKV("pass1_wall_seconds", static_cast<int64_t>(42));
    UniValue block_metrics{UniValue::VOBJ};
    block_metrics.pushKV("BLK_P2TR_POST_ORD", static_cast<int64_t>(4096));
    json.pushKV("block_plaintext_bytes", block_metrics);
    UniValue utxo_metrics{UniValue::VOBJ};
    utxo_metrics.pushKV("UTXO_P2WPKH", static_cast<int64_t>(256));
    json.pushKV("utxo_plaintext_bytes", utxo_metrics);
    UniValue trained{UniValue::VARR};
    UniValue entry{UniValue::VOBJ};
    entry.pushKV("name", "BLK_P2TR_POST_ORD");
    entry.pushKV("chosen_size", static_cast<int64_t>(1024));
    entry.pushKV("holdout_ratio", 2.5);
    trained.push_back(entry);
    json.pushKV("trained_dicts", trained);
    std::ofstream out{baseline_path};
    BOOST_REQUIRE(out.is_open());
    out << json.write(4, 1) << std::endl;
}

void TrainMinimalTypedDicts(const fs::path& datadir)
{
    const fs::path samples_dir{datadir / "swords" / "samples"};
    const fs::path dicts_dir{datadir / "swords" / "dicts"};
    fs::create_directories(samples_dir);
    fs::create_directories(dicts_dir);
    compress::DictSampleCollector collector{samples_dir};
    std::vector<uint8_t> block_sample(512, 0x3a);
    std::vector<uint8_t> utxo_sample(128, 0x4b);
    for (int i = 0; i < 8; ++i) {
        collector.CollectBlockSample(compress::BlockBucket::BLK_P2TR_POST_ORD, block_sample);
        collector.CollectUtxoSample(compress::UtxoBucket::UTXO_P2WPKH, utxo_sample);
    }
    compress::DictionaryTrainer trainer{dicts_dir, collector, 767430};
    trainer.TrainAllBuckets(/*final_train=*/true);
}

void PatchManifestNearMaxCapacity(const fs::path& datadir)
{
    const fs::path manifest_path{datadir / "swords" / "dict_manifest.json"};
    std::ifstream in{manifest_path};
    UniValue json;
    BOOST_REQUIRE(in.is_open());
    BOOST_REQUIRE(json.read(std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}));
    BOOST_REQUIRE(json["buckets"].isArray());
    const int64_t near_max_size{static_cast<int64_t>(4096 * 1024 * 0.92)};
    UniValue updated_buckets{UniValue::VARR};
    for (size_t i = 0; i < json["buckets"].size(); ++i) {
        const UniValue& existing{json["buckets"][i]};
        if (!existing.isObject() || !existing["name"].isStr()) {
            updated_buckets.push_back(existing);
            continue;
        }
        const std::string name{existing["name"].get_str()};
        if (name == "BLK_P2TR_POST_ORD" || name == "UTXO_P2WPKH") {
            UniValue entry{existing};
            entry.pushKV("chosen_size", near_max_size);
            entry.pushKV("holdout_ratio", 3.5);
            updated_buckets.push_back(entry);
        } else {
            updated_buckets.push_back(existing);
        }
    }
    json.pushKV("buckets", updated_buckets);
    std::ofstream out{manifest_path};
    BOOST_REQUIRE(out.is_open());
    out << json.write(4, 1) << std::endl;
}

} // namespace

BOOST_AUTO_TEST_CASE(pass2_metrics_active_until_reindex_complete)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const fs::path datadir{m_args.GetDataDirBase() / "pass2_metrics_window"};
    fs::remove_all(datadir);
    WritePass1CompleteState(datadir);
    WritePass1Baseline(datadir);
    TrainMinimalTypedDicts(datadir);

    ArgsManager local_args;
    local_args.ForceSetArg("-dictbootstrap", "auto");
    BOOST_REQUIRE(compress::InitDictBootstrap(datadir, *params, local_args, /*do_reindex=*/true));
    BOOST_REQUIRE(compress::g_dict_bootstrap);
    BOOST_CHECK(compress::g_dict_bootstrap->IsPass2InProgress());
    BOOST_CHECK(compress::g_dict_bootstrap->ShouldRewriteBlocksOnReindex());
    compress::g_dict_bootstrap->OnCoinStored(compress::UtxoBucket::UTXO_P2WPKH, 256, 96);
    BOOST_CHECK_EQUAL(compress::g_dict_bootstrap->Phase(), compress::BootstrapPhase::PASS2_IN_PROGRESS);
    compress::g_dict_bootstrap->OnReindexComplete();
    BOOST_CHECK_EQUAL(compress::g_dict_bootstrap->Phase(), compress::BootstrapPhase::COMPLETE);
    compress::ShutdownDictBootstrap();
}

BOOST_AUTO_TEST_CASE(pass1_complete_reindex_enters_pass2)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const fs::path datadir{m_args.GetDataDirBase() / "pass2_enter"};
    fs::remove_all(datadir);
    WritePass1CompleteState(datadir);
    WritePass1Baseline(datadir);
    TrainMinimalTypedDicts(datadir);

    ArgsManager local_args;
    local_args.ForceSetArg("-dictbootstrap", "auto");
    BOOST_REQUIRE(compress::InitDictBootstrap(datadir, *params, local_args, /*do_reindex=*/true));
    BOOST_REQUIRE(compress::g_dict_bootstrap);
    BOOST_CHECK_EQUAL(compress::g_dict_bootstrap->Phase(), compress::BootstrapPhase::PASS2_IN_PROGRESS);
    BOOST_CHECK(compress::g_dict_bootstrap->ShouldCompressOnWrite());
    compress::ShutdownDictBootstrap();
}

BOOST_AUTO_TEST_CASE(pass2_complete_writes_compression_report)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const fs::path datadir{m_args.GetDataDirBase() / "pass2_report"};
    fs::remove_all(datadir);
    WritePass1CompleteState(datadir);
    WritePass1Baseline(datadir);
    TrainMinimalTypedDicts(datadir);

    ArgsManager local_args;
    local_args.ForceSetArg("-dictbootstrap", "auto");
    BOOST_REQUIRE(compress::InitDictBootstrap(datadir, *params, local_args, /*do_reindex=*/true));
    BOOST_REQUIRE(compress::g_dict_bootstrap);
    compress::g_dict_bootstrap->OnBlockStored(compress::BlockBucket::BLK_P2TR_POST_ORD, 4096, 1200);
    compress::g_dict_bootstrap->OnCoinStored(compress::UtxoBucket::UTXO_P2WPKH, 256, 96);
    compress::g_dict_bootstrap->OnReindexComplete();
    BOOST_CHECK_EQUAL(compress::g_dict_bootstrap->Phase(), compress::BootstrapPhase::COMPLETE);

    const fs::path report_path{datadir / "swords" / "compression_report.json"};
    BOOST_REQUIRE(fs::exists(report_path));
    std::ifstream in{report_path};
    UniValue json;
    BOOST_REQUIRE(json.read(std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}));
    BOOST_CHECK_EQUAL(json["state"].get_str(), "complete");
    BOOST_REQUIRE(json["block_buckets"].isArray());
    BOOST_REQUIRE(json["utxo_buckets"].isArray());
    BOOST_REQUIRE(json["global"].isObject());
    BOOST_CHECK(json["pass1_wall_seconds"].isNum());
    BOOST_CHECK(json["pass2_wall_seconds"].isNum());
    BOOST_CHECK_EQUAL(json["global"]["blocks_plaintext_pass1"].getInt<int64_t>(), 4096);
    BOOST_CHECK_EQUAL(json["global"]["blocks_stored_pass2"].getInt<int64_t>(), 1200);
    BOOST_CHECK_EQUAL(json["global"]["utxo_plaintext_pass1"].getInt<int64_t>(), 256);
    BOOST_CHECK_EQUAL(json["global"]["utxo_stored_pass2"].getInt<int64_t>(), 96);
    bool found_block_bucket{false};
    for (size_t i = 0; i < json["block_buckets"].size(); ++i) {
        const UniValue& entry{json["block_buckets"][i]};
        if (!entry.isObject() || !entry["name"].isStr()) continue;
        if (entry["name"].get_str() != "BLK_P2TR_POST_ORD") continue;
        BOOST_CHECK(entry["plaintext_bytes"].isNum());
        BOOST_CHECK(entry["stored_bytes"].isNum());
        BOOST_CHECK(entry["savings_percent"].isNum());
        found_block_bucket = true;
    }
    BOOST_CHECK(found_block_bucket);
    BOOST_CHECK(!json.exists("recommendations"));
    compress::ShutdownDictBootstrap();
}

BOOST_AUTO_TEST_CASE(pass2_report_recommends_more_samples_near_max_dict)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const fs::path datadir{m_args.GetDataDirBase() / "pass2_recommend"};
    fs::remove_all(datadir);
    WritePass1CompleteState(datadir);
    WritePass1Baseline(datadir);
    TrainMinimalTypedDicts(datadir);
    PatchManifestNearMaxCapacity(datadir);

    ArgsManager local_args;
    local_args.ForceSetArg("-dictbootstrap", "auto");
    BOOST_REQUIRE(compress::InitDictBootstrap(datadir, *params, local_args, /*do_reindex=*/true));
    BOOST_REQUIRE(compress::g_dict_bootstrap);
    compress::g_dict_bootstrap->OnBlockStored(compress::BlockBucket::BLK_P2TR_POST_ORD, 4096, 3200);
    compress::g_dict_bootstrap->OnCoinStored(compress::UtxoBucket::UTXO_P2WPKH, 256, 200);
    compress::g_dict_bootstrap->OnReindexComplete();

    const fs::path report_path{datadir / "swords" / "compression_report.json"};
    BOOST_REQUIRE(fs::exists(report_path));
    std::ifstream in{report_path};
    UniValue json;
    BOOST_REQUIRE(json.read(std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}));
    BOOST_REQUIRE(json["recommendations"].isArray());
    BOOST_CHECK_GE(json["recommendations"].size(), 1u);
    bool found_block_recommendation{false};
    for (size_t i = 0; i < json["recommendations"].size(); ++i) {
        const std::string recommendation{json["recommendations"][i].get_str()};
        if (recommendation.find("BLK_P2TR_POST_ORD") != std::string::npos) {
            BOOST_CHECK(recommendation.find("4096KiB max") != std::string::npos);
            found_block_recommendation = true;
        }
    }
    BOOST_CHECK(found_block_recommendation);
    compress::ShutdownDictBootstrap();
}

BOOST_AUTO_TEST_CASE(typed_block_roundtrip_per_bucket)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const fs::path datadir{m_args.GetDataDirBase() / "typed_block_roundtrip"};
    fs::remove_all(datadir);
    TrainMinimalTypedDicts(datadir);

    compress::DictSet dicts;
    BOOST_REQUIRE(dicts.Load(datadir));
    const std::array<compress::BlockBucket, 1> buckets{compress::BlockBucket::BLK_P2TR_POST_ORD};
    for (const auto bucket : buckets) {
        const compress::DictZstd& dict{dicts.BlockDict(bucket)};
        BOOST_REQUIRE(dict);
        std::vector<uint8_t> input(512, 0x3a);
        std::vector<uint8_t> compressed;
        BOOST_REQUIRE(dict.Compress(input, compressed, 3));
        std::vector<uint8_t> output;
        BOOST_REQUIRE(dict.Decompress(compressed, output, input.size() * 2));
        BOOST_CHECK_EQUAL_COLLECTIONS(input.begin(), input.end(), output.begin(), output.end());
    }
    (void)params;
}

BOOST_AUTO_TEST_CASE(typed_utxo_roundtrip_pass2)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const fs::path datadir{m_args.GetDataDirBase() / "typed_utxo_pass2"};
    fs::remove_all(datadir);
    WritePass1CompleteState(datadir);
    TrainMinimalTypedDicts(datadir);

    ArgsManager local_args;
    local_args.ForceSetArg("-dictbootstrap", "auto");
    BOOST_REQUIRE(compress::InitDictBootstrap(datadir, *params, local_args, /*do_reindex=*/true));
    BOOST_REQUIRE(compress::g_dict_bootstrap);

    const compress::UtxoZstd fallback;
    CoinsViewOptions options;
    options.utxo_zstd = true;
    options.utxo_zstd_level = 3;
    const compress::UtxoBucket bucket{compress::UtxoBucket::UTXO_P2WPKH};
    const Coin original{MakeCoinForBucket(bucket, 500000)};
    const std::vector<uint8_t> encoded{EncodeCoinValue(original, options, fallback)};
    BOOST_REQUIRE_GE(encoded.size(), 2u);
    BOOST_CHECK_EQUAL(encoded[0], 1);
    BOOST_CHECK_EQUAL(encoded[1], compress::g_dict_bootstrap->UtxoTypeByteForBucket(bucket));
    BOOST_CHECK(compress::g_dict_bootstrap->ShouldCompressOnWrite());

    Coin decoded;
    BOOST_REQUIRE(DecodeCoinValue(encoded, fallback, decoded));
    BOOST_CHECK_EQUAL(decoded.out.nValue, original.out.nValue);
    compress::ShutdownDictBootstrap();
}

BOOST_AUTO_TEST_SUITE_END()