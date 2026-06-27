# Copyright (c) 2025 The Bitcoin Swords developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

add_library(lmdb STATIC EXCLUDE_FROM_ALL
  ${PROJECT_SOURCE_DIR}/src/lmdb/mdb.c
  ${PROJECT_SOURCE_DIR}/src/lmdb/midl.c
)

target_include_directories(lmdb
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/lmdb>
)

target_compile_definitions(lmdb
  PRIVATE
    MDB_MAXKEYSIZE=8192
    $<$<BOOL:${WIN32}>:_WIN32>
)

if(UNIX AND NOT APPLE)
  find_library(RT_LIBRARY rt)
  if(RT_LIBRARY)
    target_link_libraries(lmdb PRIVATE ${RT_LIBRARY})
  endif()
endif()

target_link_libraries(lmdb
  PRIVATE
    core_interface
)

set_target_properties(lmdb PROPERTIES
  EXPORT_COMPILE_COMMANDS OFF
)