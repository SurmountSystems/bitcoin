// Copyright (c) 2026 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <bench/data/block413567.raw.h>
#include <coins.h>
#include <dbwrapper.h>
#include <node/blockfile_format.h>
#include <node/blockstorage.h>
#include <primitives/block.h>
#include <serialize.h>
#include <streams.h>
#include <test/util/coins.h>
#include <test/util/setup_common.h>
#include <txdb.h>
#include <sync.h>
#include <validation.h>

#include <cassert>
#include <vector>

namespace {

CBlock CreateTestBlock()
{
    DataStream stream{benchmark::data::block413567};
    CBlock block;
    stream >> TX_WITH_WITNESS(block);
    return block;
}

BlockManager& MakeBenchBlockman(const TestingSetup& setup)
{
    return setup.m_node.chainman->m_blockman;
}

std::vector<uint8_t> LoadCompressedPayload(const TestingSetup& setup, const FlatFilePos& pos, node::BlockDiskHeader& header)
{
    auto& blockman{MakeBenchBlockman(setup)};
    const uint32_t probe_size{BLOCK_SERIALIZATION_HEADER_SIZE};
    AutoFile file{blockman.OpenBlockFile({pos.nFile, pos.nPos - probe_size}, /*fReadOnly=*/true)};
    std::vector<uint8_t> header_bytes(probe_size);
    file.read(MakeWritableByteSpan(header_bytes));
    ParseBlockDiskHeader(setup.m_node.chainman->GetParams(), pos.nPos, header_bytes, header);
    std::vector<uint8_t> stored(header.stored_size);
    file.read(MakeWritableByteSpan(stored));
    return stored;
}

} // namespace

static void DecompressBlockPayloadSerial(benchmark::Bench& bench)
{
    const auto setup{MakeNoLogFileContext<const TestingSetup>(ChainType::MAIN)};
    auto& blockman{MakeBenchBlockman(*setup)};
    const auto pos{blockman.WriteBlock(CreateTestBlock(), 413'567)};
    node::BlockDiskHeader header;
    const auto compressed{LoadCompressedPayload(*setup, pos, header)};
    std::vector<uint8_t> out;
    blockman.ConfigureDecompressPool(0);
    blockman.SetIbdParallelReadsAllowed(true);
    bench.run([&] {
        const bool ok{blockman.ReadRawBlockFromStored(out, header, compressed, /*allow_parallel_decompress=*/true)};
        assert(ok);
    });
}

static void DecompressBlockPayloadParallel(benchmark::Bench& bench)
{
    const auto setup{MakeNoLogFileContext<const TestingSetup>(ChainType::MAIN)};
    auto& blockman{MakeBenchBlockman(*setup)};
    const auto pos{blockman.WriteBlock(CreateTestBlock(), 413'567)};
    node::BlockDiskHeader header;
    const auto compressed{LoadCompressedPayload(*setup, pos, header)};
    std::vector<uint8_t> out;
    blockman.ConfigureDecompressPool(4);
    blockman.SetIbdParallelReadsAllowed(true);
    bench.run([&] {
        const bool ok{blockman.ReadRawBlockFromStored(out, header, compressed, /*allow_parallel_decompress=*/true)};
        assert(ok);
    });
}

static void ReadRawBlockCompressed(benchmark::Bench& bench)
{
    const auto setup{MakeNoLogFileContext<const TestingSetup>(ChainType::MAIN)};
    auto& blockman{MakeBenchBlockman(*setup)};
    const auto pos{blockman.WriteBlock(CreateTestBlock(), 413'567)};
    std::vector<uint8_t> block_data;
    blockman.ReadRawBlock(block_data, pos);
    blockman.SetIbdParallelReadsAllowed(true);
    bench.run([&] {
        const bool ok{blockman.ReadRawBlock(block_data, pos)};
        assert(ok);
    });
}

static void ReadRawBlockUncompressed(benchmark::Bench& bench)
{
    const auto setup{MakeNoLogFileContext<const TestingSetup>(ChainType::MAIN)};
    auto& blockman{MakeBenchBlockman(*setup)};
    CBlock block;
    block.nVersion = 3;
    const auto pos{blockman.WriteBlock(block, 1)};
    std::vector<uint8_t> block_data;
    blockman.ReadRawBlock(block_data, pos);
    blockman.SetIbdParallelReadsAllowed(true);
    bench.run([&] {
        const bool ok{blockman.ReadRawBlock(block_data, pos)};
        assert(ok);
    });
}

static void PrefetchCoinsCold(benchmark::Bench& bench)
{
    const auto setup{MakeNoLogFileContext<const TestingSetup>(ChainType::REGTEST)};
    FastRandomContext rng;
    CCoinsViewDB db{DBParams{.path = setup->m_args.GetDataDirNet() / "prefetch_bench", .cache_bytes = 512 << 20, .wipe_data = true},
                    CoinsViewOptions{}};
    std::vector<COutPoint> prevouts;
    prevouts.reserve(512);
    for (int i = 0; i < 512; ++i) {
        CCoinsViewCache cache{&db};
        AddTestCoin(rng, cache);
        cache.SetBestBlock(uint256::ONE);
        assert(cache.Flush());
        prevouts.push_back(COutPoint{uint256::ONE, static_cast<uint32_t>(i)});
    }
    std::vector<PrefetchedCoin> out;
    bench.run([&] {
        out.clear();
        const bool ok{ParallelPrefetchCoins(db, prevouts, out, 4)};
        assert(ok);
        assert(out.size() == prevouts.size());
    });
}

static void PrefetchCoinsWarm(benchmark::Bench& bench)
{
    const auto setup{MakeNoLogFileContext<const TestingSetup>(ChainType::REGTEST)};
    bench.run([&] {
        LOCK(::cs_main);
        CCoinsViewCache cache{&setup->m_node.chainman->ActiveChainstate().CoinsDB()};
        const CBlock block{CreateTestBlock()};
        const std::vector<COutPoint> prevouts{CollectBlockPrevouts(block, cache)};
        const std::vector<COutPoint> again{CollectBlockPrevouts(block, cache)};
        assert(again.size() <= prevouts.size());
    });
}

BENCHMARK(DecompressBlockPayloadSerial, benchmark::PriorityLevel::HIGH);
BENCHMARK(DecompressBlockPayloadParallel, benchmark::PriorityLevel::HIGH);
BENCHMARK(ReadRawBlockCompressed, benchmark::PriorityLevel::HIGH);
BENCHMARK(ReadRawBlockUncompressed, benchmark::PriorityLevel::HIGH);
BENCHMARK(PrefetchCoinsCold, benchmark::PriorityLevel::HIGH);
BENCHMARK(PrefetchCoinsWarm, benchmark::PriorityLevel::HIGH);