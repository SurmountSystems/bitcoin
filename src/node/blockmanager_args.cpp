// Copyright (c) 2023 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/blockmanager_args.h>

#include <common/args.h>
#include <compress/zstd.h>
#include <kernel/blockmanager_opts.h>
#include <node/blockstorage.h>
#include <node/chainstatemanager_args.h>
#include <node/database_args.h>
#include <tinyformat.h>
#include <util/fs.h>
#include <util/result.h>
#include <util/translation.h>
#include <common/system.h>
#include <validation.h>

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace node {
util::Result<uint64_t> ParsePruneOption(const int64_t nPruneArg, const std::string_view opt_name)
{
    // block pruning; get the amount of disk space (in MiB) to allot for block & undo files
    if (nPruneArg < 0) {
        return util::Error{strprintf(_("%s cannot be configured with a negative value."), opt_name)};
    } else if (nPruneArg == 0) { // pruning disabled
        return 0;
    } else if (nPruneArg == 1) { // manual pruning: -prune=1
        return BlockManager::PRUNE_TARGET_MANUAL;
    }
    const uint64_t nPruneTarget{uint64_t(nPruneArg) * 1024 * 1024};
    if (nPruneTarget < MIN_DISK_SPACE_FOR_BLOCK_FILES) {
        return util::Error{strprintf(_("%s configured below the minimum of %d MiB.  Please use a higher number."), opt_name, MIN_DISK_SPACE_FOR_BLOCK_FILES / 1024 / 1024)};
    }
    return nPruneTarget;
}

util::Result<void> ApplyArgsManOptions(const ArgsManager& args, BlockManager::Options& opts)
{
    if (auto value{args.GetBoolArg("-blocksxor")}) opts.use_xor = *value;

    // block pruning; get the amount of disk space (in MiB) to allot for block & undo files
    int64_t nPruneArg{args.GetIntArg("-prune", opts.prune_target)};
    if (const auto prune_parsed = ParsePruneOption(nPruneArg, "Prune")) {
        opts.prune_target = *prune_parsed;
    } else {
        return util::Error{util::ErrorString(prune_parsed)};
    }

    if (const auto prune_during_init{args.GetIntArg("-pruneduringinit")}) {
        if (*prune_during_init == -1) {
            opts.prune_target_during_init = -1;
        } else if (auto prune_parsed = ParsePruneOption(*prune_during_init, "-pruneduringinit")) {
            if (!*prune_parsed) {
                // We don't actually disable pruning, just treat it as manual until sync completes
                *prune_parsed = BlockManager::PRUNE_TARGET_MANUAL;
            }
            // NOTE: PRUNE_TARGET_MANUAL is >int64 max
            opts.prune_target_during_init = std::min(std::numeric_limits<int64_t>::max(), (int64_t)*prune_parsed);
        } else {
            return util::Error{util::ErrorString(prune_parsed)};
        }
    }

    if (auto value{args.GetBoolArg("-fastprune")}) opts.fast_prune = *value;

    if (auto value{args.GetBoolArg("-blockzstd")}) opts.block_zstd = *value;
    if (auto value{args.GetIntArg("-blockzstdlevel")}) {
        if (*value < compress::BlockZstd::MIN_LEVEL || *value > compress::BlockZstd::MAX_LEVEL) {
            return util::Error{strprintf(_("Invalid level for -blockzstdlevel (%d); must be between %d and %d."),
                                         *value, compress::BlockZstd::MIN_LEVEL, compress::BlockZstd::MAX_LEVEL)};
        }
        opts.block_zstd_level = *value;
    }
    if (auto value{args.GetBoolArg("-blockzstddecompress")}) opts.block_zstd_decompress = *value;
    if (const auto dict_arg{args.GetArg("-blockzstddict")}) {
        opts.block_zstd_dict = fs::u8path(*dict_arg);
    }

    const int64_t decompress_par{args.GetIntArg("-blockdecompresspar", kernel::DEFAULT_BLOCK_DECOMPRESS_PAR)};
    if (decompress_par < 0 || decompress_par > kernel::MAX_BLOCK_DECOMPRESS_PAR) {
        return util::Error{strprintf(_("Invalid -blockdecompresspar value (%d), must be between 0 and %d."),
                                     decompress_par, kernel::MAX_BLOCK_DECOMPRESS_PAR)};
    }
    if (decompress_par == 1) {
        opts.block_decompress_workers = 0;
    } else if (decompress_par >= 2) {
        opts.block_decompress_workers = static_cast<int>(decompress_par);
    } else if (decompress_par == 0) {
        int64_t script_threads{args.GetIntArg("-par", DEFAULT_SCRIPTCHECK_THREADS)};
        if (script_threads <= 0) {
            script_threads += GetNumCores();
        }
        opts.block_decompress_workers = std::min<int>(kernel::MAX_BLOCK_DECOMPRESS_PAR, std::max<int64_t>(1, script_threads - 1));
    }

    ReadDatabaseArgs(args, opts.block_tree_db_params.options);

    return {};
}
} // namespace node
