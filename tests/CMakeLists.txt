# Host test suite for the KisakCOD (CoD4 SP) Switch port.
#
# This is a separate, standalone CMake project from the game's own
# scripts/*/CMakeLists.txt (which cross-configures for Switch/Windows/Linux
# game targets). It builds and runs the host-side tests, proofs, and source
# gates that `./test host*` used to drive with hand-written g++/python3
# command lines.
#
# Configure/build/test with the "host" CMakePresets.json preset at the repo
# root (binary dir under the per-worktree state dir so concurrent worktrees
# never share objects):
#   cmake --preset host
#   cmake --build --preset host
#   ctest --preset host
# `./test host` / `host-core` / `host-script` / `host-retail` / `host-corpus`
# remain the documented entry points; they drive this project underneath
# for the groups that have been ported (see tests/host/CMakeLists.txt's
# top comment for what that is today).
cmake_minimum_required(VERSION 3.21)
project(kisak_host_tests LANGUAGES C CXX)

include(CTest)
enable_testing()

# Repo root, one level up from this file.
get_filename_component(KISAK_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# Deliberately no default CMAKE_BUILD_TYPE: ./test's own compile lines never
# passed an -O flag (so plain -O0) except where a check adds its own -O1/-O2
# explicitly (see the per-target COMPILE_OPTIONS below), and a handful of
# checks (e.g. scr_parsetree/scr_yacc's LODWORD-adjacent code) only stay
# -Wall -Wextra -Werror clean at -O0 -- GCC's strict-aliasing analysis (part
# of -O2) flags the decompiled union-punning macros that -O0 leaves alone.
# A CMAKE_BUILD_TYPE (even RelWithDebInfo) would inject -O2 -DNDEBUG here.
#
# Plain, non-GNU standard versions (-std=c++20, not -std=gnu++20) to match
# every explicit `-std=...` in ./test exactly.
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(host)
