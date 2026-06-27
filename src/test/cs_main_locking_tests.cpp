// Copyright (c) 2025 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
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
#include <thread>
#include <vector>

#include <boost/test/unit_test.hpp>
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