// Copyright (c) 2017-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <chainparams.h>
#include <consensus/consensus.h>
#include <index/txindex.h>
#include <interfaces/chain.h>
#include <node/blockfile_format.h>
#include <node/blockstorage.h>
#include <script/script.h>
#include <test/util/index.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <vector>

BOOST_AUTO_TEST_SUITE(txindex_tests)

BOOST_FIXTURE_TEST_CASE(txindex_initial_sync, TestChain100Setup)
{
    TxIndex txindex(interfaces::MakeChain(m_node), 1 << 20, true);
    BOOST_REQUIRE(txindex.Init());

    CTransactionRef tx_disk;
    uint256 block_hash;

    // Transaction should not be found in the index before it is started.
    for (const auto& txn : m_coinbase_txns) {
        BOOST_CHECK(!txindex.FindTx(txn->GetHash(), block_hash, tx_disk));
    }

    // BlockUntilSyncedToCurrentChain should return false before txindex is started.
    BOOST_CHECK(!txindex.BlockUntilSyncedToCurrentChain());

    BOOST_REQUIRE(txindex.StartBackgroundSync());

    // Allow tx index to catch up with the block index.
    IndexWaitSynced(txindex, *Assert(m_node.shutdown_signal));

    // Check that txindex excludes genesis block transactions.
    const CBlock& genesis_block = Params().GenesisBlock();
    for (const auto& txn : genesis_block.vtx) {
        BOOST_CHECK(!txindex.FindTx(txn->GetHash(), block_hash, tx_disk));
    }

    // Check that txindex has all txs that were in the chain before it started.
    for (const auto& txn : m_coinbase_txns) {
        if (!txindex.FindTx(txn->GetHash(), block_hash, tx_disk)) {
            BOOST_ERROR("FindTx failed");
        } else if (tx_disk->GetHash() != txn->GetHash()) {
            BOOST_ERROR("Read incorrect tx");
        }
    }

    // Check that new transactions in new blocks make it into the index.
    for (int i = 0; i < 10; i++) {
        CScript coinbase_script_pub_key = GetScriptForDestination(PKHash(coinbaseKey.GetPubKey()));
        std::vector<CMutableTransaction> no_txns;
        const CBlock& block = CreateAndProcessBlock(no_txns, coinbase_script_pub_key);
        const CTransaction& txn = *block.vtx[0];

        BOOST_CHECK(txindex.BlockUntilSyncedToCurrentChain());
        if (!txindex.FindTx(txn.GetHash(), block_hash, tx_disk)) {
            BOOST_ERROR("FindTx failed");
        } else if (tx_disk->GetHash() != txn.GetHash()) {
            BOOST_ERROR("Read incorrect tx");
        }
    }

    // It is not safe to stop and destroy the index until it finishes handling
    // the last BlockConnected notification. The BlockUntilSyncedToCurrentChain()
    // call above is sufficient to ensure this, but the
    // SyncWithValidationInterfaceQueue() call below is also needed to ensure
    // TSAN always sees the test thread waiting for the notification thread, and
    // avoid potential false positive reports.
    m_node.validation_signals->SyncWithValidationInterfaceQueue();

    // shutdown sequence (c.f. Shutdown() in init.cpp)
    txindex.Stop();
}

BOOST_FIXTURE_TEST_CASE(txindex_findtx_compressed_block, TestChain100Setup)
{
    TxIndex txindex(interfaces::MakeChain(m_node), 1 << 20, true);
    BOOST_REQUIRE(txindex.Init());
    BOOST_REQUIRE(txindex.StartBackgroundSync());
    IndexWaitSynced(txindex, *Assert(m_node.shutdown_signal));

    const CMutableTransaction tx{CreateValidMempoolTransaction(
        m_coinbase_txns[0], 0, COINBASE_MATURITY, coinbaseKey,
        CScript() << OP_RETURN << std::vector<uint8_t>(2048, 0x42),
        1 * COIN,
        false)};

    const CScript coinbase_script_pub_key = GetScriptForDestination(PKHash(coinbaseKey.GetPubKey()));
    const CBlock block = CreateAndProcessBlock({tx}, coinbase_script_pub_key);

    const CBlockIndex* pindex;
    {
        LOCK(cs_main);
        pindex = m_node.chainman->m_blockman.LookupBlockIndex(block.GetHash());
        BOOST_REQUIRE(pindex);
        BOOST_REQUIRE(pindex->nStatus & BLOCK_HAVE_DATA);
    }

    // Verify the block was stored with a compressed on-disk payload when possible.
    {
        const FlatFilePos block_pos{pindex->GetBlockPos()};
        const uint32_t probe_size{node::BLOCK_SERIALIZATION_HEADER_SIZE};
        AutoFile file{m_node.chainman->m_blockman.OpenBlockFile({block_pos.nFile, block_pos.nPos - probe_size}, /*fReadOnly=*/true)};
        BOOST_REQUIRE(!file.IsNull());
        std::array<uint8_t, node::BLOCK_SERIALIZATION_HEADER_SIZE> header_bytes{};
        file.read(MakeWritableByteSpan(header_bytes));
        node::BlockDiskHeader disk_header;
        BOOST_REQUIRE(node::ParseBlockDiskHeader(Params(), block_pos.nPos, header_bytes, disk_header));
        BOOST_CHECK((disk_header.flags & node::BLOCK_SERIALIZATION_FLAG_COMPRESSED) != 0);
    }

    IndexWaitSynced(txindex, *Assert(m_node.shutdown_signal));

    CTransactionRef tx_disk;
    uint256 block_hash;
    BOOST_REQUIRE(txindex.FindTx(tx.GetHash(), block_hash, tx_disk));
    BOOST_CHECK_EQUAL(tx_disk->GetHash(), tx.GetHash());
    BOOST_CHECK_EQUAL(block_hash, block.GetHash());

    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    txindex.Stop();
}

BOOST_AUTO_TEST_SUITE_END()
