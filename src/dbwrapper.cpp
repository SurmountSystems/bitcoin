// Copyright (c) 2012-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <dbwrapper.h>

#include <common/args.h>
#include <dbwrapper_leveldb_migrate.h>
#include <logging.h>
#include <node/interface_ui.h>
#include <random.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/obfuscation.h>
#include <util/strencodings.h>
#include <util/translation.h>

#include <lmdb.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dbwrapper_settings {
size_t g_db_map_size{0};
bool g_auto_migrate_leveldb{true};

void InitFromArgs(const ArgsManager& args)
{
    if (auto value = args.GetIntArg("-dbmapsize")) {
        const int64_t map_mib = *value < 0 ? 0 : *value;
        g_db_map_size = static_cast<size_t>(map_mib) << 20;
    }
    if (auto value = args.GetBoolArg("-migrateleveldb")) {
        g_auto_migrate_leveldb = *value;
    }
}
} // namespace dbwrapper_settings

namespace {

constexpr size_t MIN_MAP_SIZE = 64 << 20;
constexpr size_t DEFAULT_MAP_SIZE = 512 << 20;

std::string LMDBErrorString(int rc)
{
    return std::string{mdb_strerror(rc)};
}

void HandleLMDBError(int rc, const std::string& where)
{
    if (rc == MDB_SUCCESS) return;
    const std::string errmsg = strprintf("Fatal LMDB error in %s: %s (%d)", where, LMDBErrorString(rc), rc);
    LogError("%s", errmsg);
    LogInfo("You can use -debug=leveldb or -debug=lmdb to get more complete diagnostic messages");
    throw dbwrapper_error(errmsg);
}

bool LogDBWrapperDebug()
{
    return LogAcceptCategory(BCLog::LEVELDB, BCLog::Level::Debug);
}

bool IsChainstateDatabase(const fs::path& path)
{
    const std::string name{fs::PathToString(path.filename())};
    return name == "chainstate" || name == "chainstate_snapshot";
}

size_t GetEnvMapSize(MDB_env* env)
{
    MDB_envinfo info;
    if (mdb_env_info(env, &info) != MDB_SUCCESS) return 0;
    return static_cast<size_t>(info.me_mapsize);
}

size_t GetEnvUsage(MDB_env* env)
{
    MDB_envinfo info;
    MDB_stat stat;
    if (mdb_env_info(env, &info) != MDB_SUCCESS) return 0;
    if (mdb_env_stat(env, &stat) != MDB_SUCCESS) return 0;
    return static_cast<size_t>(stat.ms_psize) * static_cast<size_t>(info.me_last_pgno);
}

size_t CalculateInitialMapSize(const DBParams& params)
{
    if (params.map_size_bytes > 0) return params.map_size_bytes;
    if ((params.use_global_map_size || IsChainstateDatabase(params.path)) &&
        dbwrapper_settings::g_db_map_size > 0) {
        return dbwrapper_settings::g_db_map_size;
    }
    const size_t cache_hint = std::max(params.cache_bytes, params.options.max_file_size);
    return std::max(MIN_MAP_SIZE, std::max(DEFAULT_MAP_SIZE, cache_hint * 16));
}

struct TLSCacheEntry {
    MDB_txn* txn{nullptr};
    uint64_t epoch{0};
};

//! Per-environment read txn cache. A single thread may hold cached read txns for
//! multiple LMDB environments (e.g. main chainstate + snapshot chainstate).
thread_local std::map<MDB_env*, TLSCacheEntry> g_tls_read_txns;

void ReleaseTLSReadTxn(MDB_env* env);

} // namespace

struct LMDBContext {
    MDB_env* env{nullptr};
    MDB_dbi dbi{0};
    size_t map_size{0};
    size_t cache_bytes{0};
    bool sync_writes{true};
    mutable std::mutex write_mutex;
    std::atomic<uint64_t> read_epoch{0};

    void SyncMapSizeFromEnv()
    {
        const size_t actual = GetEnvMapSize(env);
        if (actual > 0) map_size = actual;
    }

    ~LMDBContext()
    {
        // Authoritative TLS read-txn cleanup for this environment. CDBWrapper
        // destruction must not duplicate this; m_db_context is destroyed last.
        ReleaseTLSReadTxn(env);
        if (env) {
            if (dbi) mdb_dbi_close(env, dbi);
            mdb_env_close(env);
        }
    }
};

namespace {

void ReleaseTLSReadTxn(MDB_env* env)
{
    auto it = g_tls_read_txns.find(env);
    if (it == g_tls_read_txns.end()) return;
    if (it->second.txn) {
        mdb_txn_abort(it->second.txn);
        // mdb_txn_abort returns void in embedded LMDB; verify reader-table hygiene.
        const int rc = mdb_reader_check(env, nullptr);
        if (rc != MDB_SUCCESS) {
            LogError("Reader check after TLS read txn abort failed: %s (%d)", LMDBErrorString(rc), rc);
        }
    }
    g_tls_read_txns.erase(it);
}

MDB_txn* BeginReadTxn(const LMDBContext& ctx)
{
    const uint64_t epoch = ctx.read_epoch.load(std::memory_order_acquire);
    auto it = g_tls_read_txns.find(ctx.env);
    if (it != g_tls_read_txns.end() && it->second.txn && it->second.epoch == epoch) {
        return it->second.txn;
    }
    if (it != g_tls_read_txns.end()) {
        ReleaseTLSReadTxn(ctx.env);
    }
    HandleLMDBError(mdb_reader_check(ctx.env, nullptr), "read reader check");
    MDB_txn* txn{nullptr};
    const int rc = mdb_txn_begin(ctx.env, nullptr, MDB_RDONLY, &txn);
    HandleLMDBError(rc, "read transaction begin");
    TLSCacheEntry& entry = g_tls_read_txns[ctx.env];
    entry.txn = txn;
    entry.epoch = epoch;
    return txn;
}

void InvalidateReadTxns(LMDBContext& ctx)
{
    ctx.read_epoch.fetch_add(1, std::memory_order_release);
    ReleaseTLSReadTxn(ctx.env);
    HandleLMDBError(mdb_reader_check(ctx.env, nullptr), "reader check");
}

bool GetRaw(MDB_txn* txn, MDB_dbi dbi, std::span<const std::byte> key, std::string& value_out)
{
    MDB_val mdb_key{.mv_size = key.size(), .mv_data = const_cast<std::byte*>(key.data())};
    MDB_val mdb_value;
    const int rc = mdb_get(txn, dbi, &mdb_key, &mdb_value);
    if (rc == MDB_NOTFOUND) return false;
    HandleLMDBError(rc, "read");
    value_out.assign(static_cast<const char*>(mdb_value.mv_data), mdb_value.mv_size);
    return true;
}

} // namespace

bool DestroyDB(const std::string& path_str)
{
    const fs::path path{fs::u8path(path_str)};
    std::error_code ec;
    fs::remove_all(path, ec);
    return !ec;
}

util::Result<void> dbwrapper_SanityCheck()
{
#ifndef EMBEDDED_LMDB
    const int compiled_major = MDB_VERSION_MAJOR;
    const int compiled_minor = MDB_VERSION_MINOR;
    const int compiled_patch = MDB_VERSION_PATCH;
    const int runtime_major = mdb_version(nullptr, nullptr, nullptr);
    int runtime_minor = 0;
    int runtime_patch = 0;
    mdb_version(&runtime_minor, &runtime_patch, nullptr);
    if (compiled_major != runtime_major || compiled_minor != runtime_minor || compiled_patch != runtime_patch) {
        return util::Error{Untranslated(strprintf(
            "Compiled with LMDB %d.%d.%d, but linked with LMDB %d.%d.%d (incompatible).",
            compiled_major, compiled_minor, compiled_patch,
            runtime_major, runtime_minor, runtime_patch))};
    }
#endif
    MDB_env* env{nullptr};
    if (mdb_env_create(&env) != MDB_SUCCESS) {
        return util::Error{Untranslated("Failed to create LMDB environment for sanity check")};
    }
    const int max_key_size = mdb_env_get_maxkeysize(env);
    mdb_env_close(env);
    if (max_key_size < DBWRAPPER_MIN_MAX_KEY_SIZE) {
        return util::Error{Untranslated(strprintf(
            "LMDB max key size %d is too small (need at least %d). "
            "Use the embedded LMDB build or rebuild system LMDB with MDB_MAXKEYSIZE>=8192.",
            max_key_size, DBWRAPPER_MIN_MAX_KEY_SIZE))};
    }
    return {};
}

struct CDBBatch::WriteBatchImpl {
    struct Entry {
        std::vector<std::byte> key;
        std::vector<std::byte> value;
        bool erase{false};
    };
    std::vector<Entry> entries;
};

CDBBatch::CDBBatch(const CDBWrapper& _parent)
    : parent{_parent},
      m_impl_batch{std::make_unique<CDBBatch::WriteBatchImpl>()}
{
    Clear();
}

CDBBatch::~CDBBatch() = default;

void CDBBatch::Clear()
{
    m_impl_batch->entries.clear();
    size_estimate = kHeader;
}

void CDBBatch::WriteImpl(Span<const std::byte> key, DataStream& ssValue)
{
    ssValue.Xor(dbwrapper_private::GetObfuscateKey(parent));
    CDBBatch::WriteBatchImpl::Entry entry;
    entry.key.assign(key.begin(), key.end());
    entry.value.assign(ssValue.begin(), ssValue.end());
    m_impl_batch->entries.emplace_back(std::move(entry));
    size_estimate += 3 + key.size() + ssValue.size();
}

void CDBBatch::EraseImpl(Span<const std::byte> key)
{
    CDBBatch::WriteBatchImpl::Entry entry;
    entry.key.assign(key.begin(), key.end());
    entry.erase = true;
    m_impl_batch->entries.emplace_back(std::move(entry));
    size_estimate += 2 + key.size();
}

size_t CDBWrapper::RequiredMapSize() const
{
    return DBContext().map_size;
}

void CDBWrapper::GrowMapSizeUnlocked()
{
    auto& ctx = const_cast<LMDBContext&>(DBContext());
    ctx.SyncMapSizeFromEnv();
    const size_t current_size = ctx.map_size;
    const size_t new_size = std::max(current_size * 2, current_size + (64 << 20));
    LogWarning("Growing LMDB map for %s from %zu to %zu bytes", m_name, current_size, new_size);
    HandleLMDBError(mdb_env_set_mapsize(ctx.env, new_size), "map resize");
    ctx.SyncMapSizeFromEnv();
}

void CDBWrapper::GrowMapSize()
{
    auto& ctx = const_cast<LMDBContext&>(DBContext());
    std::lock_guard lock{ctx.write_mutex};
    GrowMapSizeUnlocked();
}

CDBWrapper::CDBWrapper(const DBParams& params)
    : m_db_context{std::make_unique<LMDBContext>()},
      m_name{fs::PathToString(params.path.stem())},
      m_path{params.path},
      m_is_memory{params.memory_only}
{
    auto& ctx = *m_db_context;
    ctx.cache_bytes = params.cache_bytes;
    ctx.map_size = CalculateInitialMapSize(params);
    ctx.sync_writes = true;

    m_storage_path = params.path;
    if (params.memory_only) {
        m_storage_path = fs::path{fs::temp_directory_path() / fs::u8path(FastRandomContext{}.rand256().ToString())};
        TryCreateDirectories(m_storage_path);
        LogPrintf("Using in-memory LMDB directory %s\n", fs::PathToString(m_storage_path));
    } else {
        if (params.wipe_data) {
            LogPrintf("Wiping LMDB in %s\n", fs::PathToString(params.path));
            fs::remove_all(params.path);
        }
        const auto format = dbwrapper_leveldb_migrate::DetectDatabaseFormat(params.path);
        if (format == dbwrapper_leveldb_migrate::DatabaseFormat::LEVELDB) {
            if (!dbwrapper_settings::g_auto_migrate_leveldb) {
                throw dbwrapper_error(strprintf(
                    "LevelDB database found at %s but automatic migration is disabled; restart with -migrateleveldb=1",
                    fs::PathToString(params.path)));
            }
            size_t migrated_map_size{ctx.map_size};
            const std::string migration_error = dbwrapper_leveldb_migrate::MigrateLevelDBToLMDB(
                params.path, ctx.map_size, &migrated_map_size);
            if (!migration_error.empty()) {
                throw dbwrapper_error(migration_error);
            }
            ctx.map_size = migrated_map_size;
        }
        TryCreateDirectories(params.path);
        LogPrintf("Opening LMDB in %s\n", fs::PathToString(params.path));
    }

    HandleLMDBError(mdb_env_create(&ctx.env), "env create");
    HandleLMDBError(mdb_env_set_maxdbs(ctx.env, 1), "set maxdbs");
    HandleLMDBError(mdb_env_set_mapsize(ctx.env, ctx.map_size), "set mapsize");
    HandleLMDBError(mdb_env_set_maxreaders(ctx.env, std::max<size_t>(64, params.cache_bytes / (2 << 20))), "set maxreaders");

    // MDB_NOTLS ties reader slots to txn objects instead of pthread TLS, which
    // avoids MDB_BAD_RSLOT when a thread uses multiple LMDB environments (e.g.
    // main chainstate + assumeutxo snapshot chainstate).
    //
    // This flag applies to every CDBWrapper/LMDB environment in the node, not
    // only chainstate. With MDB_NOTLS, each active read txn or iterator holds a
    // reader slot until aborted; LMDB documents extra reader-table locking
    // overhead vs the default pthread-TLS model. maxreaders is set above from
    // cache_bytes; if MDB_READERS_FULL is observed under heavy RPC + validation
    // concurrency, consider raising -dbcache (which scales maxreaders).
    unsigned int env_flags = MDB_NORDAHEAD | MDB_NOTLS;
    if (!params.memory_only) {
        env_flags |= MDB_NOSYNC;
    }
    const int open_rc = mdb_env_open(ctx.env, fs::PathToString(m_storage_path).c_str(), env_flags, 0664);
    HandleLMDBError(open_rc, "env open");
    ctx.SyncMapSizeFromEnv();

    MDB_txn* txn{nullptr};
    HandleLMDBError(mdb_txn_begin(ctx.env, nullptr, 0, &txn), "initial txn begin");
    HandleLMDBError(mdb_dbi_open(txn, nullptr, MDB_CREATE, &ctx.dbi), "dbi open");
    HandleLMDBError(mdb_txn_commit(txn), "initial txn commit");

    LogPrintf("Opened LMDB successfully (map size %zu MiB)\n", ctx.map_size / (1 << 20));

    if (params.options.force_compact) {
        InitWarning(Untranslated(strprintf(
            "LMDB does not support compaction; -forcecompactdb ignored for %s",
            fs::PathToString(params.path))));
    }

    assert(!obfuscate_key);

    const bool key_exists = Read(OBFUSCATE_KEY_KEY, obfuscate_key);

    if (!key_exists && params.obfuscate && IsEmpty()) {
        std::vector<unsigned char> new_key = CreateObfuscateKey();
        Write(OBFUSCATE_KEY_KEY, new_key);
        Read(CDBWrapper::OBFUSCATE_KEY_KEY, obfuscate_key);
        LogDebug(BCLog::LEVELDB, "Wrote new obfuscation key for %s\n", fs::PathToString(params.path));
    }
    LogDebug(BCLog::LEVELDB, "Database %s obfuscation is %s\n",
              fs::PathToString(params.path), obfuscate_key ? "enabled" : "disabled");
}

CDBWrapper::~CDBWrapper()
{
    if (m_is_memory && !m_storage_path.empty()) {
        std::error_code ec;
        fs::remove_all(m_storage_path, ec);
    }
}

bool CDBWrapper::WriteBatch(CDBBatch& batch, bool fSync)
{
    auto& ctx = const_cast<LMDBContext&>(DBContext());
    const bool log_memory = LogDBWrapperDebug();
    const double mem_before = log_memory ? DynamicMemoryUsage() / 1024.0 / 1024 : 0;
    const int max_key_size = mdb_env_get_maxkeysize(ctx.env);

    for (int attempt = 0; attempt < 32; ++attempt) {
        std::lock_guard lock{ctx.write_mutex};
        MDB_txn* txn{nullptr};
        int rc = mdb_txn_begin(ctx.env, nullptr, 0, &txn);
        if (rc == MDB_MAP_FULL) {
            GrowMapSizeUnlocked();
            continue;
        }
        HandleLMDBError(rc, "write transaction begin");

        bool failed = false;
        for (const auto& entry : batch.m_impl_batch->entries) {
            if (entry.key.size() > static_cast<size_t>(max_key_size)) {
                throw dbwrapper_error(strprintf(
                    "Database key size %zu exceeds LMDB max key size %d for %s",
                    entry.key.size(), max_key_size, m_name));
            }
            MDB_val mdb_key{.mv_size = entry.key.size(), .mv_data = const_cast<std::byte*>(entry.key.data())};
            if (entry.erase) {
                rc = mdb_del(txn, ctx.dbi, &mdb_key, nullptr);
                if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
                    failed = true;
                    break;
                }
            } else {
                MDB_val mdb_value{.mv_size = entry.value.size(), .mv_data = const_cast<std::byte*>(entry.value.data())};
                rc = mdb_put(txn, ctx.dbi, &mdb_key, &mdb_value, 0);
                if (rc != MDB_SUCCESS) {
                    failed = true;
                    break;
                }
            }
        }

        if (failed) {
            mdb_txn_abort(txn);
            if (rc == MDB_MAP_FULL) {
                GrowMapSizeUnlocked();
                continue;
            }
            HandleLMDBError(rc, "write batch");
        }

        rc = mdb_txn_commit(txn);
        if (rc == MDB_MAP_FULL) {
            GrowMapSizeUnlocked();
            continue;
        }
        HandleLMDBError(rc, "write batch commit");

        if (fSync) {
            HandleLMDBError(mdb_env_sync(ctx.env, 1), "write batch sync");
        }

        InvalidateReadTxns(ctx);

        if (log_memory) {
            const double mem_after = DynamicMemoryUsage() / 1024.0 / 1024;
            LogDebug(BCLog::LEVELDB, "WriteBatch LMDB usage: db=%s, before=%.1fMiB, after=%.1fMiB (map %zu MiB)\n",
                     m_name, mem_before, mem_after, ctx.map_size / (1 << 20));
        }
        return true;
    }

    ctx.SyncMapSizeFromEnv();
    throw dbwrapper_error(strprintf(
        "Failed to commit write batch for %s after LMDB map growth attempts (map size %zu bytes, usage %zu bytes)",
        m_name, ctx.map_size, GetEnvUsage(ctx.env)));
}

size_t CDBWrapper::DynamicMemoryUsage() const
{
    const auto& ctx = DBContext();
    const size_t usage = GetEnvUsage(ctx.env);
    if (usage == 0) {
        LogDebug(BCLog::LEVELDB, "Failed to get LMDB environment usage\n");
    }
    return usage;
}

const std::string CDBWrapper::OBFUSCATE_KEY_KEY("\000obfuscate_key", 14);

std::vector<unsigned char> CDBWrapper::CreateObfuscateKey() const
{
    return FastRandomContext{}.randbytes(Obfuscation::KEY_SIZE);
}

std::optional<std::string> CDBWrapper::ReadImpl(Span<const std::byte> key) const
{
    const auto& ctx = DBContext();
    MDB_txn* txn = BeginReadTxn(ctx);
    std::string strValue;
    if (!GetRaw(txn, ctx.dbi, key, strValue)) {
        return std::nullopt;
    }
    return strValue;
}

bool CDBWrapper::ExistsImpl(Span<const std::byte> key) const
{
    const auto& ctx = DBContext();
    MDB_txn* txn = BeginReadTxn(ctx);
    std::string strValue;
    return GetRaw(txn, ctx.dbi, key, strValue);
}

size_t CDBWrapper::EstimateSizeImpl(Span<const std::byte> key1, Span<const std::byte> key2) const
{
    const auto& ctx = DBContext();
    ReleaseTLSReadTxn(ctx.env);
    HandleLMDBError(mdb_reader_check(ctx.env, nullptr), "estimate reader check");
    MDB_txn* txn{nullptr};
    HandleLMDBError(mdb_txn_begin(ctx.env, nullptr, MDB_RDONLY, &txn), "estimate txn begin");
    MDB_cursor* cursor{nullptr};
    HandleLMDBError(mdb_cursor_open(txn, ctx.dbi, &cursor), "estimate cursor open");

    MDB_val mdb_key{.mv_size = key1.size(), .mv_data = const_cast<std::byte*>(key1.data())};
    MDB_val value{};
    size_t size = 0;
    if (mdb_cursor_get(cursor, &mdb_key, &value, MDB_SET_RANGE) == MDB_SUCCESS) {
        do {
            const auto current_key = std::span<const std::byte>{static_cast<const std::byte*>(mdb_key.mv_data), mdb_key.mv_size};
            if (!key2.empty() && current_key.size() >= key2.size() &&
                std::lexicographical_compare(key2.begin(), key2.end(), current_key.begin(), current_key.end()) <= 0) {
                break;
            }
            size += mdb_key.mv_size + value.mv_size;
        } while (mdb_cursor_get(cursor, &mdb_key, &value, MDB_NEXT) == MDB_SUCCESS);
    }

    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);
    return size;
}

bool CDBWrapper::IsEmpty()
{
    std::unique_ptr<CDBIterator> it{NewIterator()};
    it->SeekToFirst();
    return !it->Valid();
}

struct CDBIterator::IteratorImpl {
    MDB_txn* txn{nullptr};
    MDB_cursor* cursor{nullptr};
    MDB_val key{};
    MDB_val value{};
    bool valid{false};

    IteratorImpl(MDB_env* env, MDB_dbi dbi)
    {
        ReleaseTLSReadTxn(env);
        HandleLMDBError(mdb_reader_check(env, nullptr), "iterator reader check");
        HandleLMDBError(mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn), "iterator txn begin");
        HandleLMDBError(mdb_cursor_open(txn, dbi, &cursor), "iterator cursor open");
    }

    ~IteratorImpl()
    {
        if (cursor) mdb_cursor_close(cursor);
        if (txn) mdb_txn_abort(txn);
    }

    void Seek(std::span<const std::byte> key_bytes)
    {
        MDB_val mdb_key{.mv_size = key_bytes.size(), .mv_data = const_cast<std::byte*>(key_bytes.data())};
        valid = mdb_cursor_get(cursor, &mdb_key, &value, MDB_SET_RANGE) == MDB_SUCCESS;
        if (valid) key = mdb_key;
    }

    void SeekToFirst()
    {
        key.mv_size = 0;
        valid = mdb_cursor_get(cursor, &key, &value, MDB_FIRST) == MDB_SUCCESS;
    }

    void Next()
    {
        if (!valid) return;
        valid = mdb_cursor_get(cursor, &key, &value, MDB_NEXT) == MDB_SUCCESS;
    }
};

CDBIterator::CDBIterator(const CDBWrapper& _parent, std::unique_ptr<IteratorImpl> _piter)
    : parent(_parent), m_impl_iter(std::move(_piter)) {}

CDBIterator* CDBWrapper::NewIterator()
{
    const auto& ctx = DBContext();
    return new CDBIterator{*this, std::make_unique<CDBIterator::IteratorImpl>(ctx.env, ctx.dbi)};
}

void CDBIterator::SeekImpl(Span<const std::byte> key)
{
    m_impl_iter->Seek(key);
}

Span<const std::byte> CDBIterator::GetKeyImpl() const
{
    return {static_cast<const std::byte*>(m_impl_iter->key.mv_data), m_impl_iter->key.mv_size};
}

Span<const std::byte> CDBIterator::GetValueImpl() const
{
    return {static_cast<const std::byte*>(m_impl_iter->value.mv_data), m_impl_iter->value.mv_size};
}

CDBIterator::~CDBIterator() = default;
bool CDBIterator::Valid() const { return m_impl_iter->valid; }
void CDBIterator::SeekToFirst() { m_impl_iter->SeekToFirst(); }
void CDBIterator::Next() { m_impl_iter->Next(); }

namespace dbwrapper_private {

const Obfuscation& GetObfuscateKey(const CDBWrapper& w)
{
    return w.obfuscate_key;
}

} // namespace dbwrapper_private