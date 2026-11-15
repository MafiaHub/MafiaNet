# Written in 2016 by Henrik Steffen Gaßmann <henrik@gassmann.onl>
#
# To the extent possible under law, the author(s) have dedicated all copyright
# and related and neighboring rights to this software to the public domain
# worldwide. This software is distributed without any warranty.
#
# You should have received a copy of the CC0 Public Domain Dedication along with
# this software. If not, see
#
# http://creativecommons.org/publicdomain/zero/1.0/
#
# ##############################################################################
# MODIFIED for MafiaNet from the copy shipped with GameNetworkingSockets
# (cmake/Findsodium.cmake). Changes, also CC0:
#  - pkg-config is optional on Unix: without it the library is looked up by
#    name (an unset sodium_PKG_LIBRARIES used to compare as its own name, and
#    find_library then got no names);
#  - MSVC: the target architecture comes from CMake variables instead of a
#    try_compile of a C file, which fails in a CXX-only consumer project;
#    x64, Win32 and ARM64 are supported;
#  - the compiler check also accepts a CXX-only project;
#  - the imported "sodium" target is only created when libsodium was found and
#    no target of that name exists yet.
# ##############################################################################
# Tries to find the local libsodium installation.
#
# On Windows the sodium_DIR environment variable is used as a default hint which
# can be overridden by setting the corresponding cmake variable.
#
# Once done the following variables will be defined:
#
# sodium_FOUND sodium_INCLUDE_DIR sodium_LIBRARY_DEBUG sodium_LIBRARY_RELEASE
# sodium_VERSION_STRING
#
# Furthermore an imported "sodium" target is created.
#

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang"
   OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(_GCC_COMPATIBLE 1)
endif()

# static library option
if(NOT DEFINED sodium_USE_STATIC_LIBS)
  option(sodium_USE_STATIC_LIBS "enable to statically link against sodium" OFF)
endif()
if(NOT (sodium_USE_STATIC_LIBS EQUAL sodium_USE_STATIC_LIBS_LAST))
  unset(sodium_LIBRARY CACHE)
  unset(sodium_LIBRARY_DEBUG CACHE)
  unset(sodium_LIBRARY_RELEASE CACHE)
  unset(sodium_DLL_DEBUG CACHE)
  unset(sodium_DLL_RELEASE CACHE)
  set(sodium_USE_STATIC_LIBS_LAST
      ${sodium_USE_STATIC_LIBS}
      CACHE INTERNAL "internal change tracking variable")
endif()

# ##############################################################################
# UNIX
if(UNIX)
  # import pkg-config
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(sodium_PKG QUIET libsodium)
  endif()

  if(sodium_USE_STATIC_LIBS)
    if(sodium_PKG_STATIC_LIBRARIES)
      foreach(_libname ${sodium_PKG_STATIC_LIBRARIES})
        if(NOT _libname MATCHES "^lib.*\\.a$") # ignore strings already ending
                                               # with .a
          list(INSERT sodium_PKG_STATIC_LIBRARIES 0 "lib${_libname}.a")
        endif()
      endforeach()
      list(REMOVE_DUPLICATES sodium_PKG_STATIC_LIBRARIES)
    else()
      # if pkgconfig for libsodium doesn't provide static lib info, then
      # override PKG_STATIC here..
      set(sodium_PKG_STATIC_LIBRARIES libsodium.a)
    endif()

    set(XPREFIX sodium_PKG_STATIC)
  else()
    # Also covers pkg-config being absent: the variable is then unset, and an
    # unset variable does not compare equal to "" (it compares as its name).
    if(NOT sodium_PKG_LIBRARIES)
      set(sodium_PKG_LIBRARIES sodium)
    endif()

    set(XPREFIX sodium_PKG)
  endif()

  find_path(sodium_INCLUDE_DIR sodium.h HINTS ${${XPREFIX}_INCLUDE_DIRS})
  find_library(sodium_LIBRARY_DEBUG
               NAMES ${${XPREFIX}_LIBRARIES}
               HINTS ${${XPREFIX}_LIBRARY_DIRS})
  find_library(sodium_LIBRARY_RELEASE
               NAMES ${${XPREFIX}_LIBRARIES}
               HINTS ${${XPREFIX}_LIBRARY_DIRS})

  # ############################################################################
  # Windows
elseif(WIN32)
  set(sodium_DIR "$ENV{sodium_DIR}" CACHE FILEPATH "sodium install directory")
  mark_as_advanced(sodium_DIR)

  find_path(sodium_INCLUDE_DIR sodium.h
            HINTS ${sodium_DIR}
            PATH_SUFFIXES include)

  if(MSVC)
    # Target architecture, from what CMake already knows (no try_compile: it
    # needs the C language enabled, which a CXX-only consumer does not have).
    # Visual Studio generators: the -A platform. Ninja/NMake: the compiler's
    # architecture id (x64, X86, ARM64), then the pointer size.
    set(_TARGET_ARCH "")
    if(CMAKE_VS_PLATFORM_NAME)
      set(_TARGET_ARCH "${CMAKE_VS_PLATFORM_NAME}")
    elseif(CMAKE_GENERATOR_PLATFORM)
      set(_TARGET_ARCH "${CMAKE_GENERATOR_PLATFORM}")
    elseif(CMAKE_CXX_COMPILER_ARCHITECTURE_ID)
      set(_TARGET_ARCH "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
    elseif(CMAKE_C_COMPILER_ARCHITECTURE_ID)
      set(_TARGET_ARCH "${CMAKE_C_COMPILER_ARCHITECTURE_ID}")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(_TARGET_ARCH "x64")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
      set(_TARGET_ARCH "Win32")
    endif()
    string(TOUPPER "${_TARGET_ARCH}" _TARGET_ARCH)

    # construct library path (libsodium's MSVC layout: <platform>/<config>/v<toolset>/<linkage>)
    if(_TARGET_ARCH STREQUAL "X64" OR _TARGET_ARCH STREQUAL "AMD64" OR _TARGET_ARCH STREQUAL "X86_64")
      string(APPEND _PLATFORM_PATH "x64")
    elseif(_TARGET_ARCH STREQUAL "WIN32" OR _TARGET_ARCH STREQUAL "X86" OR _TARGET_ARCH STREQUAL "I386")
      string(APPEND _PLATFORM_PATH "Win32")
    elseif(_TARGET_ARCH STREQUAL "ARM64")
      string(APPEND _PLATFORM_PATH "ARM64")
    else()
      message(
        FATAL_ERROR
          "the '${_TARGET_ARCH}' architecture is not supported by Findsodium.cmake."
        )
    endif()
    string(APPEND _PLATFORM_PATH "/$$CONFIG$$")

    if(MSVC_VERSION LESS 1900)
      math(EXPR _VS_VERSION "${MSVC_VERSION} / 10 - 60")
    else()
      math(EXPR _VS_VERSION "${MSVC_VERSION} / 10 - 50")
    endif()
    string(APPEND _PLATFORM_PATH "/v${_VS_VERSION}")

    if(sodium_USE_STATIC_LIBS)
      string(APPEND _PLATFORM_PATH "/static")
    else()
      string(APPEND _PLATFORM_PATH "/dynamic")
    endif()

    string(REPLACE "$$CONFIG$$"
                   "Debug"
                   _DEBUG_PATH_SUFFIX
                   "${_PLATFORM_PATH}")
    string(REPLACE "$$CONFIG$$"
                   "Release"
                   _RELEASE_PATH_SUFFIX
                   "${_PLATFORM_PATH}")

    find_library(sodium_LIBRARY_DEBUG libsodium.lib
                 HINTS ${sodium_DIR}
                 PATH_SUFFIXES ${_DEBUG_PATH_SUFFIX})
    find_library(sodium_LIBRARY_RELEASE libsodium.lib
                 HINTS ${sodium_DIR}
                 PATH_SUFFIXES ${_RELEASE_PATH_SUFFIX})
    if(NOT sodium_USE_STATIC_LIBS)
      set(CMAKE_FIND_LIBRARY_SUFFIXES_BCK ${CMAKE_FIND_LIBRARY_SUFFIXES})
      set(CMAKE_FIND_LIBRARY_SUFFIXES ".dll")
      find_library(sodium_DLL_DEBUG libsodium
                   HINTS ${sodium_DIR}
                   PATH_SUFFIXES ${_DEBUG_PATH_SUFFIX})
      find_library(sodium_DLL_RELEASE libsodium
                   HINTS ${sodium_DIR}
                   PATH_SUFFIXES ${_RELEASE_PATH_SUFFIX})
      set(CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES_BCK})
    endif()

  elseif(_GCC_COMPATIBLE)
    if(sodium_USE_STATIC_LIBS)
      find_library(sodium_LIBRARY_DEBUG libsodium.a
                   HINTS ${sodium_DIR}
                   PATH_SUFFIXES lib)
      find_library(sodium_LIBRARY_RELEASE libsodium.a
                   HINTS ${sodium_DIR}
                   PATH_SUFFIXES lib)
    else()
      find_library(sodium_LIBRARY_DEBUG libsodium.dll.a
                   HINTS ${sodium_DIR}
                   PATH_SUFFIXES lib)
      find_library(sodium_LIBRARY_RELEASE libsodium.dll.a
                   HINTS ${sodium_DIR}
                   PATH_SUFFIXES lib)

      file(GLOB _DLL
           LIST_DIRECTORIES false
           RELATIVE "${sodium_DIR}/bin"
           "${sodium_DIR}/bin/libsodium*.dll")
      find_library(sodium_DLL_DEBUG ${_DLL} libsodium
                   HINTS ${sodium_DIR}
                   PATH_SUFFIXES bin)
      find_library(sodium_DLL_RELEASE ${_DLL} libsodium
                   HINTS ${sodium_DIR}
                   PATH_SUFFIXES bin)
    endif()
  else()
    message(FATAL_ERROR "this platform is not supported by FindSodium.cmake")
  endif()

  # ############################################################################
  # unsupported
else()
  message(FATAL_ERROR "this platform is not supported by FindSodium.cmake")
endif()

# ##############################################################################
# common stuff

# extract sodium version
if(sodium_INCLUDE_DIR)
  set(_VERSION_HEADER "${sodium_INCLUDE_DIR}/sodium/version.h")
  if(EXISTS "${_VERSION_HEADER}")
    file(READ "${_VERSION_HEADER}" _VERSION_HEADER_CONTENT)
    string(REGEX
           REPLACE ".*define[ \t]+SODIUM_VERSION_STRING[^\"]+\"([^\"]+)\".*"
                   "\\1"
                   sodium_VERSION_STRING
                   "${_VERSION_HEADER_CONTENT}")
    set(sodium_VERSION_STRING "${sodium_VERSION_STRING}")
  endif()
endif()

# communicate results
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(sodium
                                  REQUIRED_VARS
                                  sodium_LIBRARY_RELEASE
                                  sodium_LIBRARY_DEBUG
                                  sodium_INCLUDE_DIR
                                  VERSION_VAR
                                  sodium_VERSION_STRING)

# mark file paths as advanced
mark_as_advanced(sodium_INCLUDE_DIR)
mark_as_advanced(sodium_LIBRARY_DEBUG)
mark_as_advanced(sodium_LIBRARY_RELEASE)
if(WIN32)
  mark_as_advanced(sodium_DLL_DEBUG)
  mark_as_advanced(sodium_DLL_RELEASE)
endif()

# create imported target
if(sodium_USE_STATIC_LIBS)
  set(_LIB_TYPE STATIC)
else()
  set(_LIB_TYPE SHARED)
endif()
if(NOT sodium_FOUND OR TARGET sodium)
  # Not found (a QUIET find must not leave a target pointing at NOTFOUND
  # paths), or another find module or a parent project already provides one.
  return()
endif()
add_library(sodium ${_LIB_TYPE} IMPORTED)

set_target_properties(sodium
                      PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                 "${sodium_INCLUDE_DIR}"
                                 IMPORTED_LINK_INTERFACE_LANGUAGES
                                 "C")

if(sodium_USE_STATIC_LIBS)
  set_target_properties(sodium
                        PROPERTIES INTERFACE_COMPILE_DEFINITIONS
                                   "SODIUM_STATIC"
                                   IMPORTED_LOCATION
                                   "${sodium_LIBRARY_RELEASE}"
                                   IMPORTED_LOCATION_DEBUG
                                   "${sodium_LIBRARY_DEBUG}")
else()
  if(UNIX)
    set_target_properties(sodium
                          PROPERTIES IMPORTED_LOCATION
                                     "${sodium_LIBRARY_RELEASE}"
                                     IMPORTED_LOCATION_DEBUG
                                     "${sodium_LIBRARY_DEBUG}")
  elseif(WIN32)
    set_target_properties(sodium
                          PROPERTIES IMPORTED_IMPLIB
                                     "${sodium_LIBRARY_RELEASE}"
                                     IMPORTED_IMPLIB_DEBUG
                                     "${sodium_LIBRARY_DEBUG}")
    if(NOT (sodium_DLL_DEBUG MATCHES ".*-NOTFOUND"))
      set_target_properties(sodium
                            PROPERTIES IMPORTED_LOCATION_DEBUG
                                       "${sodium_DLL_DEBUG}")
    endif()
    if(NOT (sodium_DLL_RELEASE MATCHES ".*-NOTFOUND"))
      set_target_properties(sodium
                            PROPERTIES IMPORTED_LOCATION_RELWITHDEBINFO
                                       "${sodium_DLL_RELEASE}"
                                       IMPORTED_LOCATION_MINSIZEREL
                                       "${sodium_DLL_RELEASE}"
                                       IMPORTED_LOCATION_RELEASE
                                       "${sodium_DLL_RELEASE}")
    endif()
  endif()
endif()
