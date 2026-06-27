# Copyright (c) 2025 The Bitcoin Swords developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

#[=======================================================================[
FindZstd
--------

Finds the zstd headers and library.

This is a wrapper around find_package()/pkg_check_modules() commands that:
 - facilitates searching in various build environments
 - prints a standard log message

#]=======================================================================]

include(FindPackageHandleStandardArgs)
find_package(zstd ${Zstd_FIND_VERSION} NO_MODULE QUIET)
if(zstd_FOUND)
  find_package_handle_standard_args(Zstd
    REQUIRED_VARS zstd_DIR
    VERSION_VAR zstd_VERSION
  )
  if(TARGET zstd::libzstd_static)
    add_library(zstd ALIAS zstd::libzstd_static)
  elseif(TARGET zstd::libzstd_shared)
    add_library(zstd ALIAS zstd::libzstd_shared)
  elseif(TARGET libzstd)
    add_library(zstd ALIAS libzstd)
  endif()
  mark_as_advanced(zstd_DIR)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(libzstd QUIET
    IMPORTED_TARGET
    libzstd>=${Zstd_FIND_VERSION}
  )
  find_package_handle_standard_args(Zstd
    REQUIRED_VARS libzstd_LIBRARY_DIRS
    VERSION_VAR libzstd_VERSION
  )
  add_library(zstd ALIAS PkgConfig::libzstd)
endif()