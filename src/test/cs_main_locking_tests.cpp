// Copyright (c) 2025 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <txdb.h>
#include <consensus/merkle.h>
#include <node/blockstorage.h>
#include <node/miner.h>
#include <pow.h>
#include <random.h>
#include <sync.h>
#include <validation.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include <boost/test/unit_test.hpp>
#include <test/util/coins.h>
#include <test/util/random.h>
#include <test/util/script.h>
#include <test/util/setup_common.h>
#include <util/time.h>

using node::BlockAssembler;
using node::BlockManager;

namespace {
//! Fail fast if join blocks (e.g. deadlock) instead of relying on the global test timeout.
//! On timeout the supervisor thread is detached (it may still block in join()); the worker
//! may also remain blocked. BOOST_FAIL ends the test case and process teardown is cleanup.
void JoinWithTimeout(std::thread& t, std::chrono::milliseconds timeout, const char* label)
{
    std::promise<void> done;
    std::future<void> fut = done.get_future();
    std::thread waiter{[&] {
        t.join();
        done.set_value();
    }};
    if (fut.wait_for(timeout) != std::future_status::ready) {
        waiter.detach();
        BOOST_FAIL(std::string{"thread join timed out (possible deadlock): "} + label);
    }
    waiter.join();
}

std::shared_ptr<CBlock> MakeBlockTemplate(TestChain100Setup& setup, const uint256& prev_hash)
{
    static int i = 0;
    static uint64_t time = setup.m_node.chainman->GetParams().GenesisBlock().nTime;

    BlockAssembler::Options options;
    options.coinbase_output_script = CScript{} << i++ << OP_TRUE;
    auto ptemplate = BlockAssembler{setup.m_node.chainman->ActiveChainstate(), setup.m_node.mempool.get(), options, setup.m_node}.CreateNewBlock();
    auto pblock = std::make_shared<CBlock>(ptemplate->block);
    pblock->hashPrevBlock = prev_hash;
    pblock->nTime = ++time;

    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vout.resize(2);
    txCoinbase.vout[1].scriptPubKey = P2WSH_OP_TRUE;
    txCoinbase.vout[1].nValue = txCoinbase.vout[0].nValue;
    txCoinbase.vout[0].nValue = 0;
    txCoinbase.vin[0].scriptWitness.SetNull();
    txCoinbase.vin[0].scriptSig = CScript{} << WITH_LOCK(::cs_main, return setup.m_node.chainman->m_blockman.LookupBlockIndex(prev_hash)->nHeight + 1) << OP_0;
    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));
    return pblock;
}

std::shared_ptr<const CBlock> FinalizeGoodBlock(TestChain100Setup& setup, const std::shared_ptr<CBlock>& pblock)
{
    const CBlockIndex* prev_block{WITH_LOCK(::cs_main, return setup.m_node.chainman->m_blockman.LookupBlockIndex(pblock->hashPrevBlock))};
    setup.m_node.chainman->GenerateCoinbaseCommitment(*pblock, prev_block);
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
    while (!CheckProofOfWork(pblock->GetHash(), pblock->nBits, setup.m_node.chainman->GetConsensus())) {
        ++(pblock->nNonce);
    }
    BlockValidationState ignored;
    BOOST_CHECK(Assert(setup.m_node.chainman)->ProcessNewBlockHeaders({{pblock->GetBlockHeader()}}, true, ignored));
    return pblock;
}

std::shared_ptr<const CBlock> GoodBlock(TestChain100Setup& setup, const uint256& prev_hash)
{
    return FinalizeGoodBlock(setup, MakeBlockTemplate(setup, prev_hash));
}

std::vector<std::shared_ptr<const CBlock>> BuildGoodBlockChain(TestChain100Setup& setup, const uint256& root, int height)
{
    std::vector<std::shared_ptr<const CBlock>> blocks;
    blocks.reserve(height);
    uint256 prev{root};
    for (int i = 0; i < height; ++i) {
        auto block = GoodBlock(setup, prev);
        blocks.push_back(block);
        prev = block->GetHash();
    }
    return blocks;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(cs_main_locking_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(block_read_loc_snapshot)
{
    const CBlockIndex* tip;
    node::BlockReadLoc loc;
    {
        LOCK(::cs_main);
        tip = m_node.chainman->ActiveChain().Tip();
        loc = m_node.chainman->m_blockman.CopyBlockReadLocAssumingLockHeld(*tip);
    }

    BOOST_REQUIRE(tip);
    BOOST_CHECK(loc.IsValid());
    BOOST_CHECK_EQUAL(loc.hash, tip->GetBlockHash());

    CBlock block;
    BOOST_CHECK(m_node.chainman->m_blockman.ReadBlock(block, loc.pos, loc.hash));
    BOOST_CHECK_EQUAL(block.GetHash(), tip->GetBlockHash());
}

BOOST_AUTO_TEST_CASE(block_index_prepare_write_commit)
{
    BlockManager& blockman{m_node.chainman->m_blockman};
    node::BlockIndexWriteBatch batch;
    size_t num_indices{0};
    {
        LOCK(::cs_main);
        batch = blockman.PrepareBlockIndexWriteBatch();
        num_indices = batch.block_indices.size();
        BOOST_REQUIRE(!batch.empty());
    }

    LOCK(blockman.m_cs_block_index_write);
    BOOST_REQUIRE(blockman.WriteBlockIndexBatch(batch));
    {
        LOCK(::cs_main);
        blockman.CommitBlockIndexWriteBatch(batch);
    }

    node::BlockIndexWriteBatch batch2;
    {
        LOCK(::cs_main);
        batch2 = blockman.PrepareBlockIndexWriteBatch();
    }
    // Dirty flags were cleared only after successful write.
    BOOST_CHECK(batch2.block_indices.size() < num_indices);
}

//! Regression: FlushStateToDisk and WriteBlockIndexDB must not deadlock when run
//! concurrently (lock-order inversion between cs_main and m_cs_block_index_write).
BOOST_AUTO_TEST_CASE(flush_state_and_write_block_index_no_deadlock)
{
    BlockManager& blockman{m_node.chainman->m_blockman};
    Chainstate& chainstate{m_node.chainman->ActiveChainstate()};

    constexpr auto TIMEOUT{std::chrono::seconds{10}};
    const auto deadline{std::chrono::steady_clock::now() + TIMEOUT};

    std::atomic<bool> stop{false};
    std::atomic<bool> flush_failed{false};
    std::atomic<bool> write_failed{false};
    std::atomic<int> flush_iterations{0};
    std::atomic<int> write_iterations{0};

    auto flush_worker{[&] {
        BlockValidationState state;
        while (!stop.load() && std::chrono::steady_clock::now() < deadline) {
            if (!chainstate.FlushStateToDisk(state, FlushStateMode::ALWAYS)) {
                flush_failed = true;
                return;
            }
            ++flush_iterations;
        }
    }};

    auto write_worker{[&] {
        while (!stop.load() && std::chrono::steady_clock::now() < deadline) {
            if (!blockman.WriteBlockIndexDB()) {
                write_failed = true;
                return;
            }
            ++write_iterations;
        }
    }};

    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back(flush_worker);
        threads.emplace_back(write_worker);
    }

    std::this_thread::sleep_for(std::chrono::seconds{2});
    stop = true;

    for (size_t i = 0; i < threads.size(); ++i) {
        JoinWithTimeout(threads[i], TIMEOUT, "flush/write worker");
    }

    BOOST_CHECK_MESSAGE(std::chrono::steady_clock::now() < deadline, "timed out waiting for flush/write workers");
    BOOST_CHECK(!flush_failed);
    BOOST_CHECK(!write_failed);
    BOOST_CHECK(flush_iterations > 0);
    BOOST_CHECK(write_iterations > 0);
}

//! Regression: parallel ProcessNewBlock must not deadlock with ActivateBestChain flush
//! (cs_main vs m_chainstate_mutex path from processnewblock_signals_ordering).
BOOST_AUTO_TEST_CASE(parallel_process_new_block_no_deadlock)
{
    // Short unconnected fork chain (headers only) plus genesis — stresses AcceptBlock
    // under cs_main, not just idempotent genesis replay.
    std::vector<std::shared_ptr<const CBlock>> workload_blocks{
        BuildGoodBlockChain(*this, Params().GenesisBlock().GetHash(), 15)};
    workload_blocks.emplace_back(std::make_shared<CBlock>(Params().GenesisBlock()));

    Chainstate& chainstate{m_node.chainman->ActiveChainstate()};

    constexpr auto TIMEOUT{std::chrono::seconds{15}};
    const auto deadline{std::chrono::steady_clock::now() + TIMEOUT};

    std::atomic<bool> stop{false};
    std::atomic<bool> pnb_failed{false};
    std::atomic<bool> flush_failed{false};
    std::atomic<int> pnb_iterations{0};
    std::atomic<int> flush_iterations{0};

    auto pnb_worker{[&] {
        bool ignored;
        FastRandomContext insecure;
        while (!stop.load() && std::chrono::steady_clock::now() < deadline) {
            const auto& block = workload_blocks[insecure.randrange(workload_blocks.size())];
            if (!Assert(m_node.chainman)->ProcessNewBlock(block, true, true, &ignored)) {
                pnb_failed = true;
                return;
            }
            ++pnb_iterations;
        }
    }};

    auto flush_worker{[&] {
        BlockValidationState state;
        while (!stop.load() && std::chrono::steady_clock::now() < deadline) {
            if (!chainstate.FlushStateToDisk(state, FlushStateMode::ALWAYS)) {
                flush_failed = true;
                return;
            }
            ++flush_iterations;
        }
    }};

    std::vector<std::thread> threads;
    threads.reserve(10);
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back(pnb_worker);
    }
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back(flush_worker);
    }

    std::this_thread::sleep_for(std::chrono::seconds{3});
    stop = true;

    for (size_t i = 0; i < threads.size(); ++i) {
        JoinWithTimeout(threads[i], TIMEOUT, "parallel PNB/flush worker");
    }

    BOOST_CHECK_MESSAGE(std::chrono::steady_clock::now() < deadline, "timed out waiting for PNB/flush workers");
    BOOST_CHECK(!pnb_failed);
    BOOST_CHECK(!flush_failed);
    BOOST_CHECK(pnb_iterations > 0);
    BOOST_CHECK(flush_iterations > 0);
}

//! Regression: shutdown interrupt during ConnectTip must not advance chain tip or
//! CoinsTip past the flushed best block (ConnectTip checks m_interrupt before view.Flush).
BOOST_AUTO_TEST_CASE(connect_tip_interrupt_before_set_tip)
{
    Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
    BlockValidationState state;

    const CBlockIndex* tip_before_disconnect;
    uint256 flushed_best_block;
    {
        LOCK2(m_node.chainman->GetMutex(), chainstate.MempoolMutex());
        tip_before_disconnect = chainstate.m_chain.Tip();
        BOOST_REQUIRE(tip_before_disconnect->pprev);

        BOOST_REQUIRE(chainstate.DisconnectTip(state, nullptr));
        BOOST_REQUIRE(chainstate.FlushStateToDiskLocked(state, FlushStateMode::ALWAYS));
        flushed_best_block = chainstate.CoinsTip().GetBestBlock();
        BOOST_CHECK_EQUAL(flushed_best_block, chainstate.m_chain.Tip()->GetBlockHash());
    }

    const CBlockIndex* tip_at_flush = WITH_LOCK(::cs_main, return chainstate.m_chain.Tip());

    // Simulate shutdown interrupt before ConnectTip would merge into CoinsTip / SetTip.
    BOOST_REQUIRE((*m_node.shutdown_signal)());

    {
        LOCK2(m_node.chainman->GetMutex(), chainstate.MempoolMutex());
        BOOST_CHECK(chainstate.ActivateBestChain(state, nullptr));
    }

    LOCK(::cs_main);
    BOOST_CHECK_EQUAL(chainstate.m_chain.Tip(), tip_at_flush);
    BOOST_CHECK_EQUAL(chainstate.m_chain.Tip()->GetBlockHash(), flushed_best_block);
    BOOST_CHECK_EQUAL(chainstate.CoinsTip().GetBestBlock(), flushed_best_block);
    BOOST_CHECK(tip_before_disconnect->GetBlockHash() != flushed_best_block);
}

//! Regression: interrupt during a multi-block ActivateBestChainStep batch must not skip
//! unconnected blocks (nHeight is bumped before the connect loop; fContinue must clear).
BOOST_AUTO_TEST_CASE(activate_best_chain_interrupt_multi_block_batch)
{
    Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
    BlockValidationState state;

    const CBlockIndex* tip_before_disconnect;
    uint256 flushed_best_block;
    {
        LOCK2(m_node.chainman->GetMutex(), chainstate.MempoolMutex());
        tip_before_disconnect = chainstate.m_chain.Tip();
        // >32 blocks so the first batch advances nHeight past m_chain.Tip() on interrupt.
        for (int i = 0; i < 35; ++i) {
            BOOST_REQUIRE(chainstate.DisconnectTip(state, nullptr));
        }
        BOOST_REQUIRE(chainstate.FlushStateToDiskLocked(state, FlushStateMode::ALWAYS));
        flushed_best_block = chainstate.CoinsTip().GetBestBlock();
        BOOST_CHECK_EQUAL(flushed_best_block, chainstate.m_chain.Tip()->GetBlockHash());
    }

    const CBlockIndex* tip_at_flush = WITH_LOCK(::cs_main, return chainstate.m_chain.Tip());
    BOOST_CHECK(tip_before_disconnect->GetAncestor(tip_at_flush->nHeight + 35) == tip_before_disconnect);

    BOOST_REQUIRE((*m_node.shutdown_signal)());

    {
        LOCK2(m_node.chainman->GetMutex(), chainstate.MempoolMutex());
        BOOST_CHECK(chainstate.ActivateBestChain(state, nullptr));
    }

    LOCK(::cs_main);
    BOOST_CHECK_EQUAL(chainstate.m_chain.Tip(), tip_at_flush);
    BOOST_CHECK_EQUAL(chainstate.m_chain.Tip()->GetBlockHash(), flushed_best_block);
    BOOST_CHECK_EQUAL(chainstate.CoinsTip().GetBestBlock(), flushed_best_block);
}

//! Regression: Shutdown() drain waits for in-flight ActivateBestChain without deadlock.
BOOST_AUTO_TEST_CASE(wait_for_activate_best_chain_drain_no_deadlock)
{
    Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
    BlockValidationState state;

    {
        LOCK2(m_node.chainman->GetMutex(), chainstate.MempoolMutex());
        BOOST_REQUIRE(chainstate.m_chain.Tip()->pprev);
        BOOST_REQUIRE(chainstate.DisconnectTip(state, nullptr));
    }

    constexpr auto TIMEOUT{std::chrono::seconds{15}};
    const auto deadline{std::chrono::steady_clock::now() + TIMEOUT};

    std::atomic<bool> stop{false};
    std::atomic<bool> abc_failed{false};
    std::atomic<bool> drain_completed{false};
    const CBlockIndex* tip_before_drain{nullptr};

    auto abc_worker{[&] {
        BlockValidationState abc_state;
        while (!stop.load() && std::chrono::steady_clock::now() < deadline) {
            if (bool{*m_node.shutdown_signal}) break;
            if (!chainstate.ActivateBestChain(abc_state, nullptr)) {
                if (bool{*m_node.shutdown_signal}) break;
                abc_failed = true;
                return;
            }
        }
    }};

    std::thread abc_thread{abc_worker};
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    BOOST_REQUIRE((*m_node.shutdown_signal)());
    tip_before_drain = WITH_LOCK(::cs_main, return chainstate.m_chain.Tip());
    chainstate.WaitForActivateBestChainDrain();
    drain_completed = true;

    stop = true;
    JoinWithTimeout(abc_thread, TIMEOUT, "ActivateBestChain drain worker");

    BOOST_CHECK_MESSAGE(std::chrono::steady_clock::now() < deadline, "timed out during drain test");
    BOOST_CHECK(drain_completed);
    BOOST_CHECK(!abc_failed);
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Tip()), tip_before_drain);
}

//! Regression: FlushStateToDisk releases cs_main during UTXO LMDB writes; concurrent
//! block reads that snapshot BlockReadLoc under cs_main must not deadlock.
BOOST_AUTO_TEST_CASE(flush_state_and_block_read_no_deadlock)
{
    BlockManager& blockman{m_node.chainman->m_blockman};
    Chainstate& chainstate{m_node.chainman->ActiveChainstate()};

    constexpr auto TIMEOUT{std::chrono::seconds{10}};
    const auto deadline{std::chrono::steady_clock::now() + TIMEOUT};

    std::atomic<bool> stop{false};
    std::atomic<bool> flush_failed{false};
    std::atomic<bool> read_failed{false};
    std::atomic<int> flush_iterations{0};
    std::atomic<int> read_iterations{0};

    auto flush_worker{[&] {
        BlockValidationState state;
        while (!stop.load() && std::chrono::steady_clock::now() < deadline) {
            if (!chainstate.FlushStateToDisk(state, FlushStateMode::ALWAYS)) {
                flush_failed = true;
                return;
            }
            ++flush_iterations;
        }
    }};

    auto read_worker{[&] {
        while (!stop.load() && std::chrono::steady_clock::now() < deadline) {
            node::BlockReadLoc loc;
            {
                LOCK(::cs_main);
                const CBlockIndex* tip{chainstate.m_chain.Tip()};
                if (!tip) {
                    read_failed = true;
                    return;
                }
                loc = blockman.CopyBlockReadLocAssumingLockHeld(*tip);
            }
            CBlock block;
            if (!blockman.ReadBlock(block, loc.pos, loc.hash)) {
                read_failed = true;
                return;
            }
            ++read_iterations;
        }
    }};

    std::vector<std::thread> threads;
    threads.reserve(6);
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back(flush_worker);
    }
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(read_worker);
    }

    std::this_thread::sleep_for(std::chrono::seconds{2});
    stop = true;

    for (size_t i = 0; i < threads.size(); ++i) {
        JoinWithTimeout(threads[i], TIMEOUT, "flush/block-read worker");
    }

    BOOST_CHECK_MESSAGE(std::chrono::steady_clock::now() < deadline, "timed out waiting for flush/block-read workers");
    BOOST_CHECK(!flush_failed);
    BOOST_CHECK(!read_failed);
    BOOST_CHECK(flush_iterations > 0);
    BOOST_CHECK(read_iterations > 0);
}

//! Regression: concurrent FlushStateToDisk must not regress DB_BEST_BLOCK.
//! m_coins_flush_mutex serializes out-of-lock LMDB writes; disk best must track cache/chain tip.
BOOST_AUTO_TEST_CASE(concurrent_flush_utxo_db_consistency)
{
    Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
    BlockManager& blockman{m_node.chainman->m_blockman};

    constexpr auto TIMEOUT{std::chrono::seconds{10}};
    const auto deadline{std::chrono::steady_clock::now() + TIMEOUT};

    std::atomic<bool> stop{false};
    std::atomic<bool> flush_failed{false};
    std::atomic<int> flush_iterations{0};

    std::mutex disk_bests_mutex;
    std::vector<uint256> disk_bests_seen;
    disk_bests_seen.reserve(64);

    auto flush_worker{[&] {
        BlockValidationState state;
        while (!stop.load() && std::chrono::steady_clock::now() < deadline) {
            if (!chainstate.FlushStateToDisk(state, FlushStateMode::ALWAYS)) {
                flush_failed = true;
                return;
            }
            uint256 disk_best;
            uint256 cache_best;
            {
                LOCK(::cs_main);
                disk_best = chainstate.CoinsDB().GetBestBlock();
                cache_best = chainstate.CoinsTip().GetBestBlock();
            }
            if (disk_best != cache_best) {
                flush_failed = true;
                return;
            }
            const CBlockIndex* prev_idx{nullptr};
            const CBlockIndex* cur_idx{nullptr};
            uint256 prev_hash;
            {
                std::lock_guard<std::mutex> lock{disk_bests_mutex};
                if (!disk_bests_seen.empty()) prev_hash = disk_bests_seen.back();
                disk_bests_seen.push_back(disk_best);
            }
            if (!prev_hash.IsNull()) {
                LOCK(::cs_main);
                prev_idx = blockman.LookupBlockIndex(prev_hash);
                cur_idx = blockman.LookupBlockIndex(disk_best);
                if (!prev_idx || !cur_idx || cur_idx->nHeight < prev_idx->nHeight) {
                    flush_failed = true;
                    return;
                }
            }
            ++flush_iterations;
        }
    }};

    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(flush_worker);
    }

    std::this_thread::sleep_for(std::chrono::seconds{2});
    stop = true;

    for (size_t i = 0; i < threads.size(); ++i) {
        JoinWithTimeout(threads[i], TIMEOUT, "concurrent flush worker");
    }

    BOOST_CHECK_MESSAGE(std::chrono::steady_clock::now() < deadline, "timed out during concurrent flush test");
    BOOST_CHECK(!flush_failed);
    BOOST_CHECK(flush_iterations > 0);
    BOOST_REQUIRE(!disk_bests_seen.empty());

    LOCK(::cs_main);
    const uint256 tip_hash{chainstate.m_chain.Tip()->GetBlockHash()};
    BOOST_CHECK_EQUAL(chainstate.CoinsTip().GetBestBlock(), tip_hash);
    BOOST_CHECK_EQUAL(chainstate.CoinsDB().GetBestBlock(), tip_hash);
}

//! Regression: ActivateBestChain (ConnectTip) during flush must not corrupt UTXO DB state.
BOOST_AUTO_TEST_CASE(flush_during_activate_best_chain_consistency)
{
    Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
    BlockValidationState state;

    const CBlockIndex* tip_before;
    {
        LOCK2(m_node.chainman->GetMutex(), chainstate.MempoolMutex());
        tip_before = chainstate.m_chain.Tip();
        BOOST_REQUIRE(tip_before->pprev);
        BOOST_REQUIRE(tip_before->pprev->pprev);
        BOOST_REQUIRE(chainstate.DisconnectTip(state, nullptr));
        BOOST_REQUIRE(chainstate.DisconnectTip(state, nullptr));
    }

    constexpr auto TIMEOUT{std::chrono::seconds{15}};
    const auto deadline{std::chrono::steady_clock::now() + TIMEOUT};

    std::atomic<bool> stop{false};
    std::atomic<bool> abc_failed{false};
    std::atomic<bool> flush_failed{false};
    std::atomic<int> abc_iterations{0};
    std::atomic<int> flush_iterations{0};

    auto abc_worker{[&] {
        BlockValidationState abc_state;
        while (!stop.load() && std::chrono::steady_clock::now() < deadline) {
            {
                LOCK2(m_node.chainman->GetMutex(), chainstate.MempoolMutex());
                if (!chainstate.ActivateBestChain(abc_state, nullptr)) {
                    abc_failed = true;
                    return;
                }
            }
            ++abc_iterations;
        }
    }};

    auto flush_worker{[&] {
        BlockValidationState flush_state;
        while (!stop.load() && std::chrono::steady_clock::now() < deadline) {
            if (!chainstate.FlushStateToDisk(flush_state, FlushStateMode::ALWAYS)) {
                flush_failed = true;
                return;
            }
            ++flush_iterations;
        }
    }};

    std::vector<std::thread> threads;
    threads.reserve(6);
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(abc_worker);
    }
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back(flush_worker);
    }

    std::this_thread::sleep_for(std::chrono::seconds{3});
    stop = true;

    for (size_t i = 0; i < threads.size(); ++i) {
        JoinWithTimeout(threads[i], TIMEOUT, "ABC/flush consistency worker");
    }

    BOOST_CHECK_MESSAGE(std::chrono::steady_clock::now() < deadline, "timed out during ABC/flush test");
    BOOST_CHECK(!abc_failed);
    BOOST_CHECK(!flush_failed);
    BOOST_CHECK(abc_iterations > 0);
    BOOST_CHECK(flush_iterations > 0);

    LOCK(::cs_main);
    const uint256 tip_hash{chainstate.m_chain.Tip()->GetBlockHash()};
    BOOST_CHECK_EQUAL(chainstate.CoinsTip().GetBestBlock(), tip_hash);
    BOOST_CHECK_EQUAL(chainstate.CoinsDB().GetBestBlock(), tip_hash);
    BOOST_CHECK_EQUAL(chainstate.m_chain.Tip(), tip_before);
}

BOOST_AUTO_TEST_CASE(prefetch_enqueue_does_not_block_cs_main)
{
    BlockManager& blockman{m_node.chainman->m_blockman};
    node::BlockReadLoc loc;
    {
        LOCK(::cs_main);
        const CBlockIndex* tip{m_node.chainman->ActiveChain().Tip()};
        BOOST_REQUIRE(tip);
        loc = blockman.CopyBlockReadLocAssumingLockHeld(*tip);
    }
    BOOST_REQUIRE(loc.IsValid());
    blockman.SetIbdParallelReadsAllowed(true);

    LOCK(::cs_main);
    const auto start{std::chrono::steady_clock::now()};
    blockman.PrefetchQueue().Enqueue(loc, blockman, m_node.chainman->m_interrupt);
    const auto elapsed{std::chrono::steady_clock::now() - start};
    BOOST_CHECK(elapsed < std::chrono::milliseconds{200});
    blockman.PrefetchQueue().Invalidate();

    blockman.PrefetchQueue().WaitForIdle();
}

BOOST_AUTO_TEST_CASE(coin_prefetch_release_locks_raii_under_lock2)
{
    FastRandomContext rng;
    Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
    CCoinsViewDB& db{WITH_LOCK(::cs_main, return chainstate.CoinsDB())};
    db.SetCoinPrefetchWorkers(4);

    std::vector<COutPoint> prevouts;
    prevouts.reserve(128);
    for (int i = 0; i < 128; ++i) {
        CCoinsViewCache seed{&db};
        AddTestCoin(rng, seed);
        seed.SetBestBlock(uint256::ONE);
        BOOST_REQUIRE(seed.Flush());
        prevouts.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), static_cast<uint32_t>(i)});
    }

    std::atomic<bool> acquired_while_prefetching{false};
    std::atomic<bool> prefetch_started{false};
    std::thread contender{[&] {
        while (!prefetch_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{5}};
        while (std::chrono::steady_clock::now() < deadline) {
            TRY_LOCK(::cs_main, lock);
            if (lock) {
                acquired_while_prefetching = true;
                return;
            }
            std::this_thread::yield();
        }
    }};

    std::vector<PrefetchedCoin> out;
    bool prefetch_ok{false};
    {
        LOCK2(m_node.chainman->GetMutex(), chainstate.MempoolMutex());
        prefetch_started.store(true, std::memory_order_release);
        ReleaseLocksForBlockIo unlock_for_prefetch{m_node.mempool.get()};
        prefetch_ok = ParallelPrefetchCoins(db, prevouts, out, 4);
    }

    JoinWithTimeout(contender, std::chrono::seconds{5}, "coin prefetch contender");
    BOOST_REQUIRE(prefetch_ok);
    BOOST_CHECK(acquired_while_prefetching.load());
}

BOOST_AUTO_TEST_CASE(prefetch_enqueue_does_not_block_under_cs_main)
{
    BlockManager& blockman{m_node.chainman->m_blockman};
    node::BlockReadLoc loc;
    {
        LOCK(::cs_main);
        const CBlockIndex* tip{m_node.chainman->ActiveChain().Tip()};
        BOOST_REQUIRE(tip);
        loc = blockman.CopyBlockReadLocAssumingLockHeld(*tip);
    }
    blockman.SetIbdParallelReadsAllowed(true);
    const auto start{std::chrono::steady_clock::now()};
    LOCK(::cs_main);
    blockman.PrefetchQueue().Enqueue(loc, blockman, m_node.chainman->m_interrupt);
    const auto elapsed{std::chrono::steady_clock::now() - start};
    BOOST_CHECK(elapsed < std::chrono::milliseconds{200});
    blockman.PrefetchQueue().Invalidate();
    LEAVE_CRITICAL_SECTION(::cs_main);
    blockman.PrefetchQueue().WaitForIdle();
    ENTER_CRITICAL_SECTION(::cs_main);
}

BOOST_AUTO_TEST_CASE(coin_prefetch_parallel_path_releases_cs_main)
{
    FastRandomContext rng;
    Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
    CCoinsViewDB& db{WITH_LOCK(::cs_main, return chainstate.CoinsDB())};
    std::vector<COutPoint> prevouts;
    prevouts.reserve(512);
    for (int i = 0; i < 512; ++i) {
        CCoinsViewCache seed{&db};
        const COutPoint outpoint{AddTestCoin(rng, seed)};
        seed.SetBestBlock(uint256::ONE);
        BOOST_REQUIRE(seed.Flush());
        prevouts.push_back(outpoint);
    }

    std::atomic<bool> prefetch_started{false};
    std::atomic<bool> acquired{false};
    std::thread contender{[&] {
        while (!prefetch_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int i = 0; i < 10'000 && !acquired.load(); ++i) {
            TRY_LOCK(::cs_main, lock);
            if (lock) {
                acquired = true;
                return;
            }
            std::this_thread::yield();
        }
    }};

    std::vector<PrefetchedCoin> out;
    bool prefetch_ok{false};
    {
        LOCK(::cs_main);
        prefetch_started.store(true, std::memory_order_release);
        LEAVE_CRITICAL_SECTION(::cs_main);
        prefetch_ok = ParallelPrefetchCoins(db, prevouts, out, 4);
        ENTER_CRITICAL_SECTION(::cs_main);
    }
    BOOST_REQUIRE(prefetch_ok);
    BOOST_CHECK(acquired.load());
    contender.join();
}

BOOST_AUTO_TEST_CASE(block_index_write_mutex_serializes)
{
    BlockManager& blockman{m_node.chainman->m_blockman};
    std::atomic<bool> holder_running{false};
    std::atomic<bool> waiter_acquired{false};

    std::thread holder{[&] {
        LOCK(blockman.m_cs_block_index_write);
        holder_running = true;
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }};

    while (!holder_running) {
        std::this_thread::yield();
    }

    std::thread waiter{[&] {
        LOCK(blockman.m_cs_block_index_write);
        waiter_acquired = true;
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    BOOST_CHECK(!waiter_acquired);

    JoinWithTimeout(holder, std::chrono::seconds{5}, "write mutex holder");
    JoinWithTimeout(waiter, std::chrono::seconds{5}, "write mutex waiter");
    BOOST_CHECK(waiter_acquired);
}

BOOST_AUTO_TEST_SUITE_END()