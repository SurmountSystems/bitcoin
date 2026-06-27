// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/coins_view_args.h>

#include <common/args.h>
#include <compress/zstd.h>
#include <logging.h>
#include <txdb.h>

#include <algorithm>

namespace node {
void ReadCoinsViewArgs(const ArgsManager& args, CoinsViewOptions& options)
{
    if (auto value = args.GetIntArg("-dbbatchsize")) options.batch_write_bytes = *value;
    if (auto value = args.GetIntArg("-dbcrashratio")) options.simulate_crash_ratio = *value;

    if (auto value{args.GetBoolArg("-utxozstd")}) options.utxo_zstd = *value;
    if (auto value{args.GetIntArg("-utxozstdlevel")}) {
        options.utxo_zstd_level = std::clamp<int>(*value, compress::UtxoZstd::MIN_LEVEL, compress::UtxoZstd::MAX_LEVEL);
    }
    if (const auto dict_arg{args.GetArg("-utxozstddict")}) {
        options.utxo_zstd_dict_path = fs::u8path(*dict_arg);
    }
}
} // namespace node