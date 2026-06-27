// Copyright (c) 2025 The Bitcoin Swords developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/licenses/mit/.

#include <dbwrapper_leveldb_migrate.h>

#include <logging.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <util/translation.h>

#include <lmdb.h>

#include <leveldb/db.h>
#include <leveldb/iterator.h>
#include <leveldb/options.h>
#include <leveldb/write_batch.h>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace dbwrapper_leveldb_migrate {
namespace {

static constexpr size_t MIGRATION_BATCH_ENTRIES{10000};

std::string LMDBErrorString(int rc)
{
    return std::string{mdb_strerror(rc)};
}

void CheckLMDB(int rc, const std::string& where)
{
    if (rc != MDB_SUCCESS) {
        throw std::runtime_error(strprintf("LMDB error in %s: %s (%d)", where, LMDBErrorString(rc), rc));
    }
}

size_t GetEnvMapSize(MDB_env* env)
{
    MDB_envinfo info;
    if (mdb_env_info(env, &info) != MDB_SUCCESS) return 0;
    return static_cast<size_t>(info.me_mapsize);
}

void SyncMapSize(MDB_env* env, size_t& map_size)
{
    const size_t actual = GetEnvMapSize(env);
    if (actual > map_size) map_size = actual;
}

bool HasLevelDBFiles(const fs::path& path)
{
    if (fs::exists(path / "CURRENT")) {
        return true;
    }
    for (const auto& entry : fs::directory_iterator(path)) {
        const std::string filename{fs::PathToString(entry.path().filename())};
        if (filename.starts_with("MANIFEST-") || filename.ends_with(".ldb") || filename.ends_with(".log")) {
            return true;
        }
    }
    return false;
}

bool HasLMDBFiles(const fs::path& path)
{
    return fs::exists(path / "data.mdb");
}

size_t DefaultMigrationMapSize(size_t requested_map_size)
{
    if (requested_map_size > 0) return requested_map_size;
    return 1ULL << 30; // 1 GiB default for migration target env
}

size_t GrowMapSize(MDB_env* env, size_t& map_size)
{
    SyncMapSize(env, map_size);
    const size_t new_size = std::max(map_size * 2, map_size + (64 << 20));
    CheckLMDB(mdb_env_set_mapsize(env, new_size), "mdb_env_set_mapsize");
    SyncMapSize(env, map_size);
    return map_size;
}

class TempDirGuard
{
    fs::path m_path;
    bool m_released{false};

public:
    explicit TempDirGuard(fs::path path) : m_path{std::move(path)} {}
    ~TempDirGuard()
    {
        if (!m_released && !m_path.empty()) {
            std::error_code ec;
            fs::remove_all(m_path, ec);
        }
    }
    void Release() { m_released = true; }
    const fs::path& Path() const { return m_path; }
};

fs::path TemporaryLMDBPath(const fs::path& path)
{
    return fs::u8path(fs::PathToString(path) + ".lmdb-migrate-tmp");
}

fs::path BackupPath(const fs::path& path)
{
    return fs::u8path(fs::PathToString(path) + ".leveldb.bak");
}

void FinalizeMigratedDirectory(const fs::path& path, const fs::path& temp_lmdb_path)
{
    const fs::path backup_path = BackupPath(path);
    if (fs::exists(backup_path)) {
        throw std::runtime_error(strprintf(
            "Refusing to migrate %s: backup already exists at %s. Remove the backup manually before retrying.",
            fs::PathToString(path), fs::PathToString(backup_path)));
    }
    fs::rename(path, backup_path);
    try {
        fs::rename(temp_lmdb_path, path);
    } catch (const fs::filesystem_error&) {
        throw std::runtime_error(strprintf(
            "Migration partially completed for %s: LevelDB data is at %s and LMDB data is at %s. "
            "To recover, rename %s back to %s or rename %s to %s after verifying data.mdb.",
            fs::PathToString(path),
            fs::PathToString(backup_path),
            fs::PathToString(temp_lmdb_path),
            fs::PathToString(backup_path),
            fs::PathToString(path),
            fs::PathToString(temp_lmdb_path),
            fs::PathToString(path)));
    }
}

void CommitBatch(MDB_env* env, MDB_dbi dbi, const std::vector<std::pair<std::vector<std::byte>, std::vector<std::byte>>>& batch,
                 size_t& map_size)
{
    for (int attempt = 0; attempt < 32; ++attempt) {
        MDB_txn* txn{nullptr};
        int rc = mdb_txn_begin(env, nullptr, 0, &txn);
        if (rc == MDB_MAP_FULL) {
            GrowMapSize(env, map_size);
            continue;
        }
        CheckLMDB(rc, "mdb_txn_begin");

        bool failed = false;
        for (const auto& [key, value] : batch) {
            MDB_val mdb_key{.mv_size = key.size(), .mv_data = const_cast<std::byte*>(key.data())};
            MDB_val mdb_value{.mv_size = value.size(), .mv_data = const_cast<std::byte*>(value.data())};
            rc = mdb_put(txn, dbi, &mdb_key, &mdb_value, 0);
            if (rc != MDB_SUCCESS) {
                failed = true;
                break;
            }
        }

        if (failed) {
            mdb_txn_abort(txn);
            if (rc == MDB_MAP_FULL) {
                GrowMapSize(env, map_size);
                continue;
            }
            CheckLMDB(rc, "mdb_put");
        }

        rc = mdb_txn_commit(txn);
        if (rc == MDB_MAP_FULL) {
            GrowMapSize(env, map_size);
            continue;
        }
        CheckLMDB(rc, "mdb_txn_commit");
        return;
    }
    throw std::runtime_error("Failed to commit migration batch after LMDB map size growth attempts");
}

} // namespace

DatabaseFormat DetectDatabaseFormat(const fs::path& path)
{
    if (!fs::exists(path)) {
        return DatabaseFormat::EMPTY;
    }
    if (HasLMDBFiles(path)) {
        return DatabaseFormat::LMDB;
    }
    if (HasLevelDBFiles(path)) {
        return DatabaseFormat::LEVELDB;
    }
    return DatabaseFormat::EMPTY;
}

std::string MigrateLevelDBToLMDB(const fs::path& path, size_t map_size_bytes, size_t* final_map_size)
{
    if (!HasLevelDBFiles(path)) {
        return {};
    }
    if (HasLMDBFiles(path)) {
        return strprintf("Refusing to migrate %s: both LevelDB and LMDB files are present", fs::PathToString(path));
    }

    LogWarning("Migrating legacy LevelDB database at %s to LMDB. The node will be offline until migration completes.",
               fs::PathToString(path));

    LogPrintf("Migrating LevelDB database at %s to LMDB\n", fs::PathToString(path));
    const auto start_time{SteadyClock::now()};

    leveldb::Options options;
    options.create_if_missing = false;
    leveldb::DB* leveldb{nullptr};
    const leveldb::Status open_status = leveldb::DB::Open(options, fs::PathToString(path), &leveldb);
    if (!open_status.ok()) {
        return strprintf("Failed to open LevelDB at %s for migration: %s", fs::PathToString(path), open_status.ToString());
    }

    const fs::path temp_lmdb_path = TemporaryLMDBPath(path);
    TempDirGuard temp_guard{temp_lmdb_path};
    if (fs::exists(temp_lmdb_path)) {
        fs::remove_all(temp_lmdb_path);
    }
    TryCreateDirectories(temp_lmdb_path);

    size_t map_size = DefaultMigrationMapSize(map_size_bytes);
    MDB_env* env{nullptr};
    int rc = mdb_env_create(&env);
    if (rc != MDB_SUCCESS) {
        delete leveldb;
        return strprintf("mdb_env_create failed: %s", LMDBErrorString(rc));
    }

    const std::string lmdb_path = fs::PathToString(temp_lmdb_path);
    rc = mdb_env_set_maxdbs(env, 1);
    if (rc == MDB_SUCCESS) rc = mdb_env_set_mapsize(env, map_size);
    if (rc == MDB_SUCCESS) rc = mdb_env_open(env, lmdb_path.c_str(), MDB_NOSYNC, 0664);
    if (rc != MDB_SUCCESS) {
        mdb_env_close(env);
        delete leveldb;
        return strprintf("Failed to create LMDB environment at %s: %s (temp dir: %s)",
                         lmdb_path, LMDBErrorString(rc), fs::PathToString(temp_lmdb_path));
    }
    SyncMapSize(env, map_size);

    MDB_dbi dbi{0};
    MDB_txn* txn{nullptr};
    rc = mdb_txn_begin(env, nullptr, 0, &txn);
    if (rc != MDB_SUCCESS) {
        mdb_env_close(env);
        delete leveldb;
        return strprintf("mdb_txn_begin failed: %s (temp dir: %s)", LMDBErrorString(rc), fs::PathToString(temp_lmdb_path));
    }
    rc = mdb_dbi_open(txn, nullptr, MDB_CREATE, &dbi);
    if (rc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        mdb_env_close(env);
        delete leveldb;
        return strprintf("mdb_dbi_open failed: %s (temp dir: %s)", LMDBErrorString(rc), fs::PathToString(temp_lmdb_path));
    }
    CheckLMDB(mdb_txn_commit(txn), "initial dbi commit");

    uint64_t migrated_entries{0};
    uint64_t migrated_bytes{0};
    std::vector<std::pair<std::vector<std::byte>, std::vector<std::byte>>> batch;
    batch.reserve(MIGRATION_BATCH_ENTRIES);

    std::unique_ptr<leveldb::Iterator> it{leveldb->NewIterator(leveldb::ReadOptions{})};
    try {
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            const leveldb::Slice key = it->key();
            const leveldb::Slice value = it->value();
            batch.emplace_back(
                std::vector<std::byte>{reinterpret_cast<const std::byte*>(key.data()), reinterpret_cast<const std::byte*>(key.data()) + key.size()},
                std::vector<std::byte>{reinterpret_cast<const std::byte*>(value.data()), reinterpret_cast<const std::byte*>(value.data()) + value.size()});
            migrated_bytes += key.size() + value.size();

            if (batch.size() >= MIGRATION_BATCH_ENTRIES) {
                CommitBatch(env, dbi, batch, map_size);
                migrated_entries += batch.size();
                batch.clear();
                if (migrated_entries % 100000 == 0) {
                    LogPrintf("... migrated %llu entries (%llu bytes) from %s\n",
                              migrated_entries, migrated_bytes, fs::PathToString(path));
                }
            }
        }
        if (!batch.empty()) {
            CommitBatch(env, dbi, batch, map_size);
            migrated_entries += batch.size();
            batch.clear();
        }
    } catch (const std::exception& e) {
        mdb_dbi_close(env, dbi);
        mdb_env_close(env);
        delete leveldb;
        return strprintf("Migration failed while writing to %s: %s (temp dir: %s)",
                         fs::PathToString(path), e.what(), fs::PathToString(temp_lmdb_path));
    }

    const leveldb::Status iter_status = it->status();
    if (!iter_status.ok()) {
        it.reset();
        mdb_dbi_close(env, dbi);
        mdb_env_close(env);
        delete leveldb;
        return strprintf("LevelDB iterator failed during migration: %s (temp dir: %s)",
                         iter_status.ToString(), fs::PathToString(temp_lmdb_path));
    }

    it.reset();
    delete leveldb;
    leveldb = nullptr;

    SyncMapSize(env, map_size);
    mdb_dbi_close(env, dbi);
    mdb_env_close(env);
    env = nullptr;

    if (!fs::exists(temp_lmdb_path / "data.mdb")) {
        return strprintf("Migration failed: LMDB data file missing in %s", fs::PathToString(temp_lmdb_path));
    }

    try {
        FinalizeMigratedDirectory(path, temp_lmdb_path);
        temp_guard.Release();
    } catch (const std::exception& e) {
        return e.what();
    }

    rc = mdb_env_create(&env);
    if (rc != MDB_SUCCESS) {
        return strprintf("mdb_env_create failed after migration: %s", LMDBErrorString(rc));
    }
    const std::string final_path = fs::PathToString(path);
    rc = mdb_env_set_mapsize(env, map_size);
    if (rc == MDB_SUCCESS) rc = mdb_env_open(env, final_path.c_str(), MDB_RDONLY, 0664);
    if (rc != MDB_SUCCESS) {
        mdb_env_close(env);
        return strprintf("Failed to verify migrated LMDB at %s: %s", final_path, LMDBErrorString(rc));
    }
    SyncMapSize(env, map_size);
    mdb_env_close(env);

    if (final_map_size) *final_map_size = map_size;

    const auto elapsed{Ticks<std::chrono::milliseconds>(SteadyClock::now() - start_time)};
    LogPrintf("Finished LevelDB -> LMDB migration of %s (%llu entries, %llu bytes, map %zu MiB) in %dms; backup at %s\n",
              fs::PathToString(path), migrated_entries, migrated_bytes, map_size / (1 << 20), elapsed,
              fs::PathToString(BackupPath(path)));

    return {};
}

bool WriteLevelDBTestEntries(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& entries)
{
    leveldb::Options options;
    options.create_if_missing = true;
    leveldb::DB* db{nullptr};
    const leveldb::Status status = leveldb::DB::Open(options, fs::PathToString(path), &db);
    if (!status.ok()) return false;
    leveldb::WriteBatch batch;
    for (const auto& [key, value] : entries) {
        batch.Put(key, value);
    }
    const leveldb::Status write_status = db->Write(leveldb::WriteOptions{}, &batch);
    delete db;
    return write_status.ok();
}

} // namespace dbwrapper_leveldb_migrate