// Copyright (c) 2025 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <compress/dict_classify.h>

#include <coins.h>
#include <kernel/chainparams.h>
#include <primitives/block.h>
#include <script/solver.h>
#include <util/chaintype.h>

#include <algorithm>
#include <array>

namespace compress {
namespace {

static constexpr int MAINNET_INSCRIPTION_ZERO_HEIGHT{767430};

bool IsPostOrdHeight(int height, const CChainParams& params)
{
    const auto ord_height{InscriptionZeroHeight(params)};
    return ord_height && height >= *ord_height;
}

size_t TotalScriptSigBytes(const CBlock& block)
{
    size_t total{0};
    for (size_t tx_idx = 1; tx_idx < block.vtx.size(); ++tx_idx) {
        for (const CTxIn& txin : block.vtx[tx_idx]->vin) {
            total += txin.scriptSig.size();
        }
    }
    return total;
}

size_t TotalWitnessBytes(const CBlock& block)
{
    size_t total{0};
    for (size_t tx_idx = 1; tx_idx < block.vtx.size(); ++tx_idx) {
        for (const CTxIn& txin : block.vtx[tx_idx]->vin) {
            for (const auto& item : txin.scriptWitness.stack) {
                total += item.size();
            }
        }
    }
    return total;
}

/** Dominant output type by tx count; ties break toward lowest bucket id (P2WPKH first). */
BlockBucket DominantOutputBucket(int height, const CBlock& block, const CChainParams& params)
{
    std::array<size_t, 3> counts{}; // P2WPKH, P2WSH, P2TR
    for (const auto& tx : block.vtx) {
        for (const CTxOut& txout : tx->vout) {
            std::vector<std::vector<unsigned char>> solutions;
            switch (Solver(txout.scriptPubKey, solutions)) {
            case TxoutType::WITNESS_V0_KEYHASH:
                ++counts[0];
                break;
            case TxoutType::WITNESS_V0_SCRIPTHASH:
                ++counts[1];
                break;
            case TxoutType::WITNESS_V1_TAPROOT:
                ++counts[2];
                break;
            default:
                break;
            }
        }
    }

    const auto dominant{std::max_element(counts.begin(), counts.end())};
    const size_t dominant_idx{static_cast<size_t>(dominant - counts.begin())};
    if (*dominant == 0) {
        return BlockBucket::BLK_P2WPKH;
    }
    if (dominant_idx == 0) return BlockBucket::BLK_P2WPKH;
    if (dominant_idx == 1) return BlockBucket::BLK_P2WSH;
    return IsPostOrdHeight(height, params) ? BlockBucket::BLK_P2TR_POST_ORD : BlockBucket::BLK_P2TR_PRE_ORD;
}

} // namespace

std::optional<int> InscriptionZeroHeight(const CChainParams& params)
{
    if (params.GetChainType() == ChainType::MAIN) {
        return MAINNET_INSCRIPTION_ZERO_HEIGHT;
    }
    return std::nullopt;
}

BlockBucket ClassifyBlock(int height, const CBlock& block, const CChainParams& params)
{
    // Non-coinbase txs only (tx index >= 1); coinbase scriptSig/witness excluded from totals.
    const size_t scriptsig_bytes{TotalScriptSigBytes(block)};
    const size_t witness_bytes{TotalWitnessBytes(block)};
    if (scriptsig_bytes > witness_bytes) {
        return BlockBucket::BLK_SCRIPTSIG;
    }
    return DominantOutputBucket(height, block, params);
}

UtxoBucket ClassifyCoin(const Coin& coin, const CChainParams& params)
{
    std::vector<std::vector<unsigned char>> solutions;
    switch (Solver(coin.out.scriptPubKey, solutions)) {
    case TxoutType::PUBKEYHASH:
        return UtxoBucket::UTXO_P2PKH;
    case TxoutType::SCRIPTHASH:
        return UtxoBucket::UTXO_P2SH;
    case TxoutType::WITNESS_V0_KEYHASH:
        return UtxoBucket::UTXO_P2WPKH;
    case TxoutType::WITNESS_V0_SCRIPTHASH:
        return UtxoBucket::UTXO_P2WSH;
    case TxoutType::WITNESS_V1_TAPROOT:
        return IsPostOrdHeight(static_cast<int>(coin.nHeight), params) ?
                   UtxoBucket::UTXO_P2TR_POST_ORD :
                   UtxoBucket::UTXO_P2TR_PRE_ORD;
    default:
        return UtxoBucket::UTXO_OTHER;
    }
}

const char* BlockBucketName(BlockBucket bucket)
{
    switch (bucket) {
    case BlockBucket::BLK_SCRIPTSIG: return "BLK_SCRIPTSIG";
    case BlockBucket::BLK_P2WPKH: return "BLK_P2WPKH";
    case BlockBucket::BLK_P2WSH: return "BLK_P2WSH";
    case BlockBucket::BLK_P2TR_PRE_ORD: return "BLK_P2TR_PRE_ORD";
    case BlockBucket::BLK_P2TR_POST_ORD: return "BLK_P2TR_POST_ORD";
    }
    return "BLK_UNKNOWN";
}

const char* UtxoBucketName(UtxoBucket bucket)
{
    switch (bucket) {
    case UtxoBucket::UTXO_P2PKH: return "UTXO_P2PKH";
    case UtxoBucket::UTXO_P2SH: return "UTXO_P2SH";
    case UtxoBucket::UTXO_P2WPKH: return "UTXO_P2WPKH";
    case UtxoBucket::UTXO_P2WSH: return "UTXO_P2WSH";
    case UtxoBucket::UTXO_P2TR_PRE_ORD: return "UTXO_P2TR_PRE_ORD";
    case UtxoBucket::UTXO_P2TR_POST_ORD: return "UTXO_P2TR_POST_ORD";
    case UtxoBucket::UTXO_OTHER: return "UTXO_OTHER";
    }
    return "UTXO_UNKNOWN";
}

} // namespace compress