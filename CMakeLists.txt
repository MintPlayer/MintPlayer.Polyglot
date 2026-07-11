# Portable (Linux/macOS, and CI-Windows) build for the Polyglot toolchain — PLAN §P22 slice 2.
#
# This is the POSIX build definition. The three .vcxproj + the .sln remain the untouched Visual-Studio-2026
# source of truth for day-to-day Windows development (PRD §4.14); this file mirrors them for platforms MSVC
# can't target. The two definitions must not drift — sources are globbed here (so a new .cpp is picked up
# automatically) and scripts/check-buildfile-parity.ps1 asserts the glob matches each .vcxproj's explicit
# <ClCompile> list.
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ./build/polyglot --version          # the CLI, shipped name
#   ./build/polyglot-tests              # the in-process unit/golden suite

cmake_minimum_required(VERSION 3.20)
project(MintPlayer.Polyglot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)            # -std=c++20, not gnu++20 — mirrors MSVC /permissive-

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# macOS: the oldest OS the shipped binary must run on (PRD §4.14). Set before any target is defined.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "" FORCE)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive-)  # match the .vcxproj (WarningLevel=Level4, ConformanceMode=true)
else()
  add_compile_options(-Wall -Wextra)
endif()

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(CORE_DIR ${REPO_ROOT}/src/MintPlayer.Polyglot.Core)
set(CLI_DIR ${REPO_ROOT}/src/MintPlayer.Polyglot.Cli)
set(TESTS_DIR ${REPO_ROOT}/tests/MintPlayer.Polyglot.Tests)

find_package(Threads REQUIRED)           # watch.hpp uses std::thread → libpthread on Linux

# Version injection (PRD §4.16 / PLAN §P24), single definition point: only polyglot.cpp reads POLYGLOT_VERSION.
# CI passes -DPOLYGLOT_VERSION=<tag>; a bare local build defaults to `git describe` (honest untagged version),
# falling back to 0.0.0-dev outside a git checkout. Unquoted token — stringized in polyglot.cpp. The Windows
# .vcxproj mirrors this define on the Core project; scripts/check-buildfile-parity.ps1 guards the two agree.
if(NOT POLYGLOT_VERSION)
  execute_process(COMMAND git describe --tags --dirty --always
    WORKING_DIRECTORY ${REPO_ROOT} OUTPUT_VARIABLE _pg_git_ver
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _pg_git_rc)
  if(_pg_git_rc EQUAL 0 AND _pg_git_ver)
    string(REGEX REPLACE "^v" "" POLYGLOT_VERSION "${_pg_git_ver}")
  else()
    set(POLYGLOT_VERSION "0.0.0-dev")
  endif()
endif()

# --- Core: the compiler library (lexer→parser→sema→lower→IR→backend engine) ------------------------------
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS ${CORE_DIR}/src/*.cpp)
add_library(polyglot-core STATIC ${CORE_SOURCES})
target_include_directories(polyglot-core PUBLIC ${CORE_DIR}/include)
target_link_libraries(polyglot-core PUBLIC Threads::Threads)
target_compile_definitions(polyglot-core PRIVATE POLYGLOT_VERSION=${POLYGLOT_VERSION})

# --- CLI: the `polyglot` executable (self-contained; shipped binary name) --------------------------------
file(GLOB CLI_SOURCES CONFIGURE_DEPENDS ${CLI_DIR}/src/*.cpp)
add_executable(polyglot ${CLI_SOURCES})
target_include_directories(polyglot PRIVATE ${CLI_DIR}/src)   # exe_path.hpp, watch.hpp
target_link_libraries(polyglot PRIVATE polyglot-core)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Self-contained C++ runtime (glibc stays dynamic — see PRD §4.14, the protoc/LLVM model).
  target_link_options(polyglot PRIVATE -static-libstdc++ -static-libgcc)
endif()

# --- Tests: the in-process unit/golden suite -------------------------------------------------------------
file(GLOB TESTS_SOURCES CONFIGURE_DEPENDS ${TESTS_DIR}/src/*.cpp)
add_executable(polyglot-tests ${TESTS_SOURCES})
target_include_directories(polyglot-tests PRIVATE ${CLI_DIR}/src)  # tests_main.cpp includes watch.hpp/exe_path.hpp
target_link_libraries(polyglot-tests PRIVATE polyglot-core)

# The CLI is a pure engine — no target is compiled in (P19). Both executables load their backends from a
# `plugins/` directory next to the binary at runtime, so copy it there after each build (the .vcxproj
# xcopy PostBuildEvent equivalent). $<TARGET_FILE_DIR:…> resolves per-config for multi-config generators.
foreach(exe polyglot polyglot-tests)
  add_custom_command(TARGET ${exe} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${REPO_ROOT}/plugins $<TARGET_FILE_DIR:${exe}>/plugins
    COMMENT "Copying target plugins next to ${exe}")
endforeach()

enable_testing()
add_test(NAME unit COMMAND polyglot-tests)
