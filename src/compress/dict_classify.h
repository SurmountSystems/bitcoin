// Copyright (c) 2025 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COMPRESS_DICT_CLASSIFY_H
#define BITCOIN_COMPRESS_DICT_CLASSIFY_H

#include <cstdint>
#include <optional>

class CBlock;
class CChainParams;
class Coin;

namespace compress {

/** Mainnet inscription-zero height (Taproot ordinals era boundary). Nullopt on non-mainnet. */
std::optional<int> InscriptionZeroHeight(const CChainParams& params);

enum class BlockBucket : uint8_t {
    BLK_SCRIPTSIG = 0,
    BLK_P2WPKH = 1,
    BLK_P2WSH = 2,
    BLK_P2TR_PRE_ORD = 3,
    BLK_P2TR_POST_ORD = 4,
};

static constexpr size_t NUM_BLOCK_BUCKETS{5};

enum class UtxoBucket : uint8_t {
    UTXO_P2PKH = 0,
    UTXO_P2SH = 1,
    UTXO_P2WPKH = 2,
    UTXO_P2WSH = 3,
    UTXO_P2TR_PRE_ORD = 4,
    UTXO_P2TR_POST_ORD = 5,
    UTXO_OTHER = 6,
};

static constexpr size_t NUM_UTXO_BUCKETS{7};

/** Classify a block payload bucket for typed dictionary training/compression.
 *
 * scriptSig/witness byte totals count non-coinbase transactions only (tx index >= 1).
 * Output-type counts include all transactions (coinbase vouts included).
 * When scriptSig bytes equal witness bytes, the dominant output bucket wins (not BLK_SCRIPTSIG).
 * Output-type ties break toward the lowest bucket id (P2WPKH < P2WSH < P2TR). */
BlockBucket ClassifyBlock(int height, const CBlock& block, const CChainParams& params);

/** Classify a UTXO entry bucket from its scriptPubKey and creation height. */
UtxoBucket ClassifyCoin(const Coin& coin, const CChainParams& params);

const char* BlockBucketName(BlockBucket bucket);
const char* UtxoBucketName(UtxoBucket bucket);

} // namespace compress

#endif // BITCOIN_COMPRESS_DICT_CLASSIFY_H