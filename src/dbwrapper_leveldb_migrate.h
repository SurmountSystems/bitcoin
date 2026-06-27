// Copyright (c) 2025 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_DBWRAPPER_LEVELDB_MIGRATE_H
#define BITCOIN_DBWRAPPER_LEVELDB_MIGRATE_H

#include <util/fs.h>

#include <string>
#include <utility>
#include <vector>

namespace dbwrapper_leveldb_migrate {

enum class DatabaseFormat {
    EMPTY,
    LEVELDB,
    LMDB,
};

//! Detect whether @p path contains LevelDB, LMDB, or no database files.
DatabaseFormat DetectDatabaseFormat(const fs::path& path);

//! Migrate a LevelDB directory at @p path to LMDB in-place.
//! On success the original LevelDB files are moved to @p path + ".leveldb.bak".
//! @param[out] final_map_size If non-null, set to the LMDB map size after migration.
//! @returns an error message on failure, or empty string on success.
std::string MigrateLevelDBToLMDB(const fs::path& path, size_t map_size_bytes, size_t* final_map_size = nullptr);

//! Write raw key/value pairs into a new on-disk LevelDB database (for tests/migration tooling).
bool WriteLevelDBTestEntries(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& entries);

} // namespace dbwrapper_leveldb_migrate

#endif // BITCOIN_DBWRAPPER_LEVELDB_MIGRATE_H