# Copyright (c) 2025 The Bitcoin Swords developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

#[=======================================================================[
FindLMDB
--------

Finds the LMDB headers and library.

This is a wrapper around find_package()/pkg_check_modules() commands that:
 - facilitates searching in various build environments
 - prints a standard log message

#]=======================================================================]

include(FindPackageHandleStandardArgs)

find_path(LMDB_INCLUDE_DIR
  NAMES lmdb.h
  DOC "Path to LMDB include directory"
)

find_library(LMDB_LIBRARY
  NAMES lmdb
  DOC "Path to LMDB library"
)

find_package_handle_standard_args(LMDB
  REQUIRED_VARS LMDB_LIBRARY LMDB_INCLUDE_DIR
)

if(LMDB_FOUND AND NOT TARGET lmdb)
  add_library(lmdb UNKNOWN IMPORTED)
  set_target_properties(lmdb PROPERTIES
    IMPORTED_LOCATION "${LMDB_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LMDB_INCLUDE_DIR}"
  )
endif()

mark_as_advanced(LMDB_INCLUDE_DIR LMDB_LIBRARY)