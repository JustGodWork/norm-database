cmake_minimum_required(VERSION 3.16)

project(norm_database LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS 1)

include(FetchContent)

# --- sol2: header-only Lua binding (the "comfort" layer over the C API) -------
# We only need its headers, so we populate (not MakeAvailable) to skip sol2's own
# CMake (which would try to find a Lua of its own).
FetchContent_Declare(sol2
    GIT_REPOSITORY https://github.com/ThePhD/sol2.git
    GIT_TAG v3.3.0)
FetchContent_GetProperties(sol2)
if(NOT sol2_POPULATED)
    FetchContent_Populate(sol2)
endif()

# --- SQLite amalgamation (single-file C, no system dependency) ----------------
FetchContent_Declare(sqlite3
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(sqlite3)
if(NOT sqlite3_POPULATED)
    FetchContent_Populate(sqlite3)
endif()

add_library(sqlite3 STATIC "${sqlite3_SOURCE_DIR}/sqlite3.c")
target_include_directories(sqlite3 PUBLIC "${sqlite3_SOURCE_DIR}")
# THREADSAFE=2: multi-thread mode (a connection is used by a single thread) — fits
# our one-connection-per-worker-thread design.
target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=2)

# --- the module ---------------------------------------------------------------
add_library(norm_database SHARED
    src/module.cpp
    src/database.cpp)

# nanos loads "<name>.dll" / "<name>.so" — drop the platform "lib" prefix.
set_target_properties(norm_database PROPERTIES PREFIX "")

target_include_directories(norm_database PRIVATE
    deps/module-sdk/include
    "${sol2_SOURCE_DIR}/include")

target_compile_features(norm_database PRIVATE cxx_std_17)
target_link_libraries(norm_database PRIVATE sqlite3)

# Lua runtime.
# Windows: link the SDK import lib (the loader needs every symbol resolved).
# Linux: do NOT link liblua.a — the SDK ships it non-PIC, so it can't go into a
# .so. The nanos server already exports the Lua symbols, so we leave lua_*/luaL_*
# undefined and let dlopen resolve them from the host at load time.
if(WIN32)
    target_link_libraries(norm_database PRIVATE "${CMAKE_SOURCE_DIR}/deps/module-sdk/lib/lua.lib")
elseif(UNIX AND NOT APPLE)
    target_link_libraries(norm_database PRIVATE pthread dl)
endif()
