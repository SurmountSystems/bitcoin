# Copyright (c) 2025 The Bitcoin Swords developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# LevelDB is retained only for one-time LevelDB -> LMDB migration.
include(CheckCXXSymbolExists)
check_cxx_symbol_exists(F_FULLFSYNC "fcntl.h" HAVE_FULLFSYNC)

add_library(leveldb_migration STATIC EXCLUDE_FROM_ALL
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/builder.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/db_impl.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/db_iter.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/dbformat.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/filename.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/log_reader.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/log_writer.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/memtable.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/repair.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/table_cache.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/version_edit.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/version_set.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/write_batch.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/table/block.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/table/block_builder.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/table/filter_block.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/table/format.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/table/iterator.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/table/merger.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/table/table.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/table/table_builder.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/table/two_level_iterator.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/arena.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/bloom.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/cache.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/coding.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/comparator.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/crc32c.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/env.cc
  $<$<NOT:$<BOOL:${WIN32}>>:${PROJECT_SOURCE_DIR}/src/leveldb/util/env_posix.cc>
  $<$<BOOL:${WIN32}>:${PROJECT_SOURCE_DIR}/src/leveldb/util/env_windows.cc>
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/filter_policy.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/hash.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/logging.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/options.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/status.cc
)

target_compile_definitions(leveldb_migration
  PRIVATE
    HAVE_SNAPPY=0
    HAVE_CRC32C=1
    HAVE_FDATASYNC=$<BOOL:${HAVE_FDATASYNC}>
    HAVE_FULLFSYNC=$<BOOL:${HAVE_FULLFSYNC}>
    HAVE_O_CLOEXEC=$<BOOL:${HAVE_O_CLOEXEC}>
    FALLTHROUGH_INTENDED=[[fallthrough]]
    $<$<NOT:$<BOOL:${WIN32}>>:LEVELDB_PLATFORM_POSIX>
    $<$<BOOL:${WIN32}>:LEVELDB_PLATFORM_WINDOWS>
    $<$<BOOL:${WIN32}>:_UNICODE;UNICODE>
)

target_include_directories(leveldb_migration
  PRIVATE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/leveldb>
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/leveldb/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/leveldb/helpers/memenv>
)

target_link_libraries(leveldb_migration
  PRIVATE
    core_interface
    crc32c
)

set_target_properties(leveldb_migration PROPERTIES
  EXPORT_COMPILE_COMMANDS OFF
)