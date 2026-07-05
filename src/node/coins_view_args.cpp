// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/coins_view_args.h>

#include <common/args.h>
#include <common/system.h>
#include <compress/zstd.h>
#include <logging.h>
#include <node/chainstatemanager_args.h>
#include <txdb.h>

#include <algorithm>

#include <util/translation.h>

namespace node {
util::Result<void> ReadCoinsViewArgs(const ArgsManager& args, CoinsViewOptions& options)
{
    if (auto value = args.GetIntArg("-dbbatchsize")) options.batch_write_bytes = *value;
    if (auto value = args.GetBoolArg("-lmdbsync")) options.lmdbsync = *value;
    if (auto value = args.GetIntArg("-dbcrashratio")) options.simulate_crash_ratio = *value;
    if (auto value = args.GetBoolArg("-flushsnapshot")) options.flush_snapshot = *value;

    if (auto value{args.GetBoolArg("-utxozstd")}) options.utxo_zstd = *value;
    if (auto value{args.GetIntArg("-utxozstdlevel")}) {
        options.utxo_zstd_level = std::clamp<int>(*value, compress::UtxoZstd::MIN_LEVEL, compress::UtxoZstd::MAX_LEVEL);
    }
    if (const auto dict_arg{args.GetArg("-utxozstddict")}) {
        options.utxo_zstd_dict_path = fs::u8path(*dict_arg);
    }

    const int64_t encode_par{args.GetIntArg("-utxoencodepar", DEFAULT_UTXO_ENCODE_PAR)};
    if (encode_par < 0 || encode_par > MAX_UTXO_ENCODE_PAR) {
        return util::Error{Untranslated(strprintf("Invalid -utxoencodepar value (%d), must be between 0 and %d.",
                                                  encode_par, MAX_UTXO_ENCODE_PAR))};
    }
    if (encode_par == 1) {
        options.utxo_encode_workers = 0;
    } else if (encode_par >= 2) {
        options.utxo_encode_workers = static_cast<int>(encode_par);
    } else if (encode_par == 0) {
        int64_t script_threads{args.GetIntArg("-par", DEFAULT_SCRIPTCHECK_THREADS)};
        if (script_threads <= 0) {
            script_threads += GetNumCores();
        }
        options.utxo_encode_workers = std::min<int>(MAX_UTXO_ENCODE_PAR, std::max<int64_t>(1, script_threads - 1));
    }

    const int64_t prefetch_par{args.GetIntArg("-coinprefetchpar", DEFAULT_COIN_PREFETCH_PAR)};
    if (prefetch_par < 0 || prefetch_par > MAX_COIN_PREFETCH_PAR) {
        return util::Error{Untranslated(strprintf("Invalid -coinprefetchpar value (%d), must be between 0 and %d.",
                                                  prefetch_par, MAX_COIN_PREFETCH_PAR))};
    }
    if (prefetch_par == 1) {
        options.coin_prefetch_workers = 0;
    } else if (prefetch_par >= 2) {
        options.coin_prefetch_workers = static_cast<int>(prefetch_par);
    } else if (prefetch_par == 0) {
        int64_t script_threads{args.GetIntArg("-par", DEFAULT_SCRIPTCHECK_THREADS)};
        if (script_threads <= 0) {
            script_threads += GetNumCores();
        }
        options.coin_prefetch_workers = std::min<int>(MAX_COIN_PREFETCH_PAR, std::max<int64_t>(1, script_threads - 1));
    }
    return {};
}
} // namespace node