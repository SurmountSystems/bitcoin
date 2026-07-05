// Copyright (c) 2012-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DBWRAPPER_H
#define BITCOIN_DBWRAPPER_H

#include <attributes.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>
#include <util/check.h>
#include <util/fs.h>
#include <util/result.h>

#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

util::Result<void> dbwrapper_SanityCheck();

static const size_t DBWRAPPER_PREALLOC_KEY_SIZE = 64;
static const size_t DBWRAPPER_PREALLOC_VALUE_SIZE = 1024;
//! Legacy LevelDB-era name; used only as an LMDB map-size heuristic via -dbfilesize.
static const size_t DBWRAPPER_MAX_FILE_SIZE = 32 << 20; // 32 MiB

static constexpr size_t DEFAULT_DB_FILE_SIZE{64};
//! Minimum LMDB max key size required (embedded build uses 8192).
static constexpr int DBWRAPPER_MIN_MAX_KEY_SIZE{512};

class ArgsManager;

namespace dbwrapper_settings {
//! Global LMDB map size override from -dbmapsize (bytes). Zero means derive per database.
extern size_t g_db_map_size;
//! When true, automatically migrate LevelDB directories to LMDB on open.
extern bool g_auto_migrate_leveldb;
void InitFromArgs(const ArgsManager& args);
} // namespace dbwrapper_settings

//! User-controlled performance and debug options.
struct DBOptions {
    //! Compact database on startup.
    bool force_compact = false;
    //! Legacy LevelDB-era file size hint (-dbfilesize); used only for LMDB map-size heuristics.
    size_t max_file_size{DEFAULT_DB_FILE_SIZE << 20};
};

//! Application-specific storage settings.
struct DBParams {
    //! Location in the filesystem where database data will be stored.
    fs::path path;
    //! Configures LMDB map sizing and reader pool tuning.
    size_t cache_bytes;
    //! If true, use an in-memory database (temporary directory).
    bool memory_only = false;
    //! If true, remove all existing data.
    bool wipe_data = false;
    //! If true, store data obfuscated via simple XOR. If false, XOR with a
    //! zero'd byte array.
    bool obfuscate = false;
    //! Passed-through options.
    DBOptions options{};
    //! Optional per-database LMDB map size override (bytes). Zero uses global/default sizing.
    size_t map_size_bytes{0};
    //! When true, apply the global -dbmapsize override to this database.
    bool use_global_map_size{false};
    //! Override LMDB max reader slots (0 = derive from cache_bytes). Test-only.
    unsigned int max_readers{0};
};

class dbwrapper_error : public std::runtime_error
{
public:
    explicit dbwrapper_error(const std::string& msg) : std::runtime_error(msg) {}
};

//! True when an LMDB error is MDB_READERS_FULL (HandleLMDBError formats rc as "… (%d)").
inline bool IsLMDBReadersFullError(const dbwrapper_error& e)
{
    const std::string_view msg{e.what()};
    return msg.find("(-30790)") != std::string_view::npos
        || msg.find("maxreaders limit reached") != std::string_view::npos;
}

class CDBWrapper;

/** These should be considered an implementation detail of the specific database.
 */
namespace dbwrapper_private {

/** Work around circular dependency, as well as for testing in dbwrapper_tests.
 * Database obfuscation should be considered an implementation detail of the
 * specific database.
 */
const Obfuscation& GetObfuscateKey(const CDBWrapper&);

/** Records fSync arguments passed to WriteBatch when active (unit tests only). */
struct WriteBatchSyncLog {
    bool active{false};
    std::vector<bool> calls;
    void Reset()
    {
        calls.clear();
    }
};
WriteBatchSyncLog& TestWriteBatchSyncLog();

}; // namespace dbwrapper_private

bool DestroyDB(const std::string& path_str);

/** Batch of changes queued to be written to a CDBWrapper */
class CDBBatch
{
    friend class CDBWrapper;

private:
    static constexpr size_t kHeader{12};

    const CDBWrapper &parent;

    struct WriteBatchImpl;
    const std::unique_ptr<WriteBatchImpl> m_impl_batch;

    DataStream ssKey{};
    DataStream ssValue{};

    size_t size_estimate{0};

    void WriteImpl(Span<const std::byte> key, DataStream& ssValue);
    void EraseImpl(Span<const std::byte> key);

public:
    /**
     * @param[in] _parent   CDBWrapper that this batch is to be submitted to
     */
    explicit CDBBatch(const CDBWrapper& _parent);
    ~CDBBatch();
    void Clear();

    template <typename K, typename V>
    void Write(const K& key, const V& value)
    {
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssValue.reserve(DBWRAPPER_PREALLOC_VALUE_SIZE);
        ssKey << key;
        ssValue << value;
        WriteImpl(ssKey, ssValue);
        ssKey.clear();
        ssValue.clear();
    }

    template <typename K>
    void Erase(const K& key)
    {
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        EraseImpl(ssKey);
        ssKey.clear();
    }

    size_t SizeEstimate() const { return size_estimate; }
};

/**
 * Database iterator. Holds a dedicated LMDB read transaction and cursor.
 *
 * Iterators must not outlive their parent CDBWrapper: destroy all iterators
 * before the wrapper is destroyed, or behavior is undefined (use-after-close
 * on the underlying MDB_env).
 */
class CDBIterator
{
public:
    struct IteratorImpl;

private:
    const CDBWrapper &parent;
    const std::unique_ptr<IteratorImpl> m_impl_iter;

    void SeekImpl(Span<const std::byte> key);
    Span<const std::byte> GetKeyImpl() const;
    Span<const std::byte> GetValueImpl() const;

public:

    /**
     * @param[in] _parent          Parent CDBWrapper instance.
     * @param[in] _piter           The original iterator implementation.
     */
    CDBIterator(const CDBWrapper& _parent, std::unique_ptr<IteratorImpl> _piter);
    ~CDBIterator();

    bool Valid() const;

    void SeekToFirst();

    template<typename K> void Seek(const K& key) {
        DataStream ssKey{};
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        SeekImpl(ssKey);
    }

    void Next();

    template<typename K> bool GetKey(K& key) {
        try {
            DataStream ssKey{GetKeyImpl()};
            ssKey >> key;
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }

    template<typename V> bool GetValue(V& value) {
        try {
            DataStream ssValue{GetValueImpl()};
            ssValue.Xor(dbwrapper_private::GetObfuscateKey(parent));
            ssValue >> value;
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }
};

struct LMDBContext;

class CDBWrapper
{
    friend const Obfuscation& dbwrapper_private::GetObfuscateKey(const CDBWrapper&);
    friend class CDBBatch;
    friend class CDBIterator;
private:
    //! holds all LMDB-specific fields of this class
    std::unique_ptr<LMDBContext> m_db_context;

    //! the name of this database
    std::string m_name;

    //! optional XOR-obfuscation of the database
    Obfuscation obfuscate_key;

    //! the key under which the obfuscation key is stored
    static const std::string OBFUSCATE_KEY_KEY;

    std::vector<unsigned char> CreateObfuscateKey() const;

    //! path to filesystem storage (logical path for memory-only databases)
    const fs::path m_path;

    //! actual on-disk LMDB environment path
    fs::path m_storage_path;

    //! whether or not the database resides in memory
    bool m_is_memory;

    std::optional<std::string> ReadImpl(Span<const std::byte> key) const;
    bool ExistsImpl(Span<const std::byte> key) const;
    size_t EstimateSizeImpl(Span<const std::byte> key1, Span<const std::byte> key2) const;
    auto& DBContext() const LIFETIMEBOUND { return *Assert(m_db_context); }
    void GrowMapSize();
    void GrowMapSizeUnlocked();
    size_t RequiredMapSize() const;

public:
    CDBWrapper(const DBParams& params);
    ~CDBWrapper();

    CDBWrapper(const CDBWrapper&) = delete;
    CDBWrapper& operator=(const CDBWrapper&) = delete;

    template <typename K, typename V>
    bool Read(const K& key, V& value) const
    {
        DataStream ssKey{};
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        std::optional<std::string> strValue{ReadImpl(ssKey)};
        if (!strValue) {
            return false;
        }
        try {
            DataStream ssValue{MakeByteSpan(*strValue)};
            ssValue.Xor(obfuscate_key);
            ssValue >> value;
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }

    template <typename K, typename V>
    bool Write(const K& key, const V& value, bool fSync = false)
    {
        CDBBatch batch(*this);
        batch.Write(key, value);
        return WriteBatch(batch, fSync);
    }

    //! @returns filesystem path to the on-disk data.
    std::optional<fs::path> StoragePath() {
        if (m_is_memory) {
            return {};
        }
        return m_path;
    }

    template <typename K>
    bool Exists(const K& key) const
    {
        DataStream ssKey{};
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        return ExistsImpl(ssKey);
    }

    template <typename K>
    bool Erase(const K& key, bool fSync = false)
    {
        CDBBatch batch(*this);
        batch.Erase(key);
        return WriteBatch(batch, fSync);
    }

    bool WriteBatch(CDBBatch& batch, bool fSync = false);

    // Get an estimate of LMDB map usage (in bytes).
    size_t DynamicMemoryUsage() const;

    //! Configured LMDB reader slot limit for this environment.
    unsigned int GetMaxReaders() const;

    /**
     * Return a new iterator. The caller owns the returned pointer and must
     * delete it before destroying this CDBWrapper.
     */
    CDBIterator* NewIterator();

    /**
     * Return true if the database managed by this class contains no entries.
     */
    bool IsEmpty();

    template<typename K>
    size_t EstimateSize(const K& key_begin, const K& key_end) const
    {
        DataStream ssKey1{}, ssKey2{};
        ssKey1.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey2.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey1 << key_begin;
        ssKey2 << key_end;
        return EstimateSizeImpl(ssKey1, ssKey2);
    }
};

#endif // BITCOIN_DBWRAPPER_H