# Host tests for the KisakCOD Switch port, migrated from the hand-written
# g++/gcc/python3 command lines in `./test`.  Coverage today (a growing subset):
#
#   - Every `./test host` source gate (the maintainers' local Python
#     scripts) as a plain ctest test: LABELS core;gate. Registered only when
#     KISAK_LOCAL_GATES is ON, i.e. when those scripts exist.
#   - The full host-script group (scr_parsetree/scr_yacc/scr_compile/
#     scr_save_roundtrip): LABELS script.
#   - The negative-compile family (must-NOT-compile checks): LABELS
#     core;negative, run through negative_compile_check.py since a plain
#     add_executable() can't express "this must fail to build" without
#     breaking `cmake --build`.
#   - A representative, still-growing slice of host-core's compile+run
#     sanitizer checks (~43 of them, including phys_pool_stride,
#     switch_perf, switch_crash, mark_context_layout, snd_audren x2):
#     LABELS core.
#
# NOT yet ported (still runs only through the original `./test`
# host-core/host-retail/host-corpus bash path -- see the migration report
# for the reasoning per group):
#   - The deko9 shader/fastpath/baked/gpufault/fsr checks (need prebuilt
#     mojoshader/UAM host libs, built by the local host-deps script).
#   - dynres, halfres_particles, view2d_alpha_skip, model_skin_simd (the
#     x86 SSE oracle needs a generated/sed-transformed source), snd_stream
#     (needs an ffmpeg-generated fixture).
#   - The AArch64-under-qemu cross checks (model_skin_neon, cm_area_walk,
#     hot_engine2, xmodel_static_bounds, cm_cull): each is a distinct
#     multi-toolchain shape (devkitA64 vs. aarch64-linux-gnu-g++, extra
#     runtime args, extra static-analysis greps); porting them needs its
#     own per-check driver script, not done this slice.
#   - The whole host-retail group (retail FS/IWD/FF, wire decode, UI,
#     killhouse walk/boot/reload proofs) and host-corpus: these depend on
#     the ~2,550-line Python fixture-generator heredoc
#     (retail_fs_fixture() in ./test) that has not been extracted into
#     a fixture directory yet, plus (for host-corpus) real retail game data.
#
# `./test host`/`host-core`/`host-script`/`host-retail`/`host-corpus` still
# run everything (ported or not) -- ported checks run twice today (once
# here under ctest, once through the original bash functions) until the
# bash side is trimmed once coverage is fuller; see the migration report.

set(H "${CMAKE_CURRENT_SOURCE_DIR}")               # tests/host
set(R "${KISAK_REPO_ROOT}")                        # repo root
set(S "${KISAK_REPO_ROOT}/src")                    # src/

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---------------------------------------------------------------- INTERFACE
# Shared flag sets. Sanitizer + defines + include dirs are genuinely shared
# across many checks; per-test warning suppressions (-Wno-psabi and friends)
# stay on the individual add_executable() calls below, since they vary test
# to test and forcing them into a shared library would hide which test
# actually needs which suppression.
add_library(host_asan_ubsan INTERFACE)
target_compile_options(host_asan_ubsan INTERFACE -g -fsanitize=address,undefined)
target_link_options(host_asan_ubsan INTERFACE -fsanitize=address,undefined)

add_library(host_tsan INTERFACE)
target_compile_options(host_tsan INTERFACE -g -fsanitize=thread)
target_link_options(host_tsan INTERFACE -fsanitize=thread)

add_library(host_kisak_sp INTERFACE)
target_compile_definitions(host_kisak_sp INTERFACE KISAK_SP)

add_library(host_switch_defines INTERFACE)
target_compile_definitions(host_switch_defines INTERFACE __SWITCH__)

# Windows/D3D9 compat macros every retail/script/model host TU needs (the
# `-D__cdecl= -D__stdcall= ...` block repeated across ~40 compiler
# invocations in ./test). Passed as raw -D compile options, not
# target_compile_definitions(): CMake refuses to put a function-style macro
# like `__declspec(x)=` on the command line via COMPILE_DEFINITIONS (it
# silently drops it with a warning instead), so every macro here goes
# through target_compile_options() as a literal -D flag for consistency.
add_library(host_win32_compat INTERFACE)
target_compile_options(host_win32_compat INTERFACE
    -D__cdecl= -D__stdcall= "-D__declspec(x)=" -D_vsnprintf=vsnprintf -D__unix__)

add_library(host_win32_inttypes INTERFACE)
target_compile_options(host_win32_inttypes INTERFACE
    -D__int8=char -D__int16=short -D__int32=int "-D__int64=long long" -D__forceinline=inline)

# `#include "src/universal/..."` etc.: root-relative quoted includes that
# only resolved implicitly while these sources sat at the repo root (see
# the CPATH export at the top of ./test for the bash side's fix). CMake
# targets get the equivalent via this include dir.
add_library(host_src_includes INTERFACE)
target_include_directories(host_src_includes INTERFACE ${S} ${R}/deps
    ${R}/deps/d3d9-headers/directx ${R}/deps/d3d9-headers/windows
    ${R}/deps/d3d9-headers ${R})

add_library(host_ui_compat INTERFACE)
target_compile_options(host_ui_compat INTERFACE -include ${H}/switch_retail_ui_compat.h)

# Scratch directory some tests write into. ./test exports its own per-worktree
# one; a plain checkout gets a directory inside the build tree.
if(DEFINED ENV{KISAK_TEST_STATE_DIR} AND NOT "$ENV{KISAK_TEST_STATE_DIR}" STREQUAL "")
    set(KISAK_TEST_STATE "$ENV{KISAK_TEST_STATE_DIR}")
else()
    set(KISAK_TEST_STATE "${CMAKE_BINARY_DIR}/test-state")
endif()
file(MAKE_DIRECTORY "${KISAK_TEST_STATE}")

# ------------------------------------------------------------------ helpers
# kisak_host_test(<name>
#     SOURCES <src...> LIBS <interface-lib...>
#     [DEFINES <-D...>] [COMPILE_OPTIONS <...>] [LINK_OPTIONS <...>]
#     [ARGS <...>] [LABELS <label...>] [TIMEOUT <seconds>])
# One target = one compile+run ctest test, matching the task's "one target
# per host test" ask. Default TIMEOUT 300s (the task's default); pass a
# longer one only where the check genuinely needs it.
function(kisak_host_test name)
    set(oneValueArgs TIMEOUT)
    set(multiValueArgs SOURCES LIBS DEFINES COMPILE_OPTIONS LINK_OPTIONS ARGS LABELS ENV)
    cmake_parse_arguments(HT "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    add_executable(${name} ${HT_SOURCES})
    if(HT_LIBS)
        target_link_libraries(${name} PRIVATE ${HT_LIBS})
    endif()
    if(HT_DEFINES)
        target_compile_definitions(${name} PRIVATE ${HT_DEFINES})
    endif()
    if(HT_COMPILE_OPTIONS)
        target_compile_options(${name} PRIVATE ${HT_COMPILE_OPTIONS})
    endif()
    if(HT_LINK_OPTIONS)
        target_link_options(${name} PRIVATE ${HT_LINK_OPTIONS})
    endif()
    if(NOT HT_TIMEOUT)
        set(HT_TIMEOUT 300)
    endif()
    add_test(NAME ${name} COMMAND ${name} ${HT_ARGS} WORKING_DIRECTORY ${R})
    set_tests_properties(${name} PROPERTIES TIMEOUT ${HT_TIMEOUT})
    if(HT_LABELS)
        set_tests_properties(${name} PROPERTIES LABELS "${HT_LABELS}")
    endif()
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "KISAK_TEST_STATE_DIR=${KISAK_TEST_STATE};${HT_ENV}")
endfunction()

# kisak_gate_test(<name> <script> [ARGS ...] [LABELS ...] [TIMEOUT n])
# A verify-*.py source/doc gate: no compile step, just `python3 <script>`
# from the repo root, exactly like ./test's `python3 "$xxx_gate" >>"$log"`.
function(kisak_gate_test name script)
    set(oneValueArgs TIMEOUT)
    set(multiValueArgs ARGS LABELS)
    cmake_parse_arguments(GT "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(NOT GT_TIMEOUT)
        set(GT_TIMEOUT 120)
    endif()
    if(NOT GT_LABELS)
        set(GT_LABELS "core;gate")
    endif()
    add_test(NAME ${name} COMMAND Python3::Interpreter ${script} ${GT_ARGS} WORKING_DIRECTORY ${R})
    set_tests_properties(${name} PROPERTIES TIMEOUT ${GT_TIMEOUT} LABELS "${GT_LABELS}")
endfunction()

# kisak_negative_test(<name> CC <gcc|g++> STD <c11|c++11|c++20>
#     SOURCE <src> [INCLUDES <dir...>] [COMMON_FLAGS <...>]
#     FAIL_VARIANT <flags...> END [FAIL_VARIANT ... END]...
#     [PASS_VARIANT <flags...> END]...
# "This must not compile" (or, for PASS_VARIANT, "this must compile
# cleanly") checks, run through negative_compile_check.py -- see that
# script's docstring for why a plain add_executable() can't express this.
function(kisak_negative_test name)
    set(oneValueArgs CC STD SOURCE TIMEOUT)
    set(multiValueArgs INCLUDES COMMON_FLAGS LABELS EXTRA_ARGS)
    cmake_parse_arguments(NT "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(NOT NT_TIMEOUT)
        set(NT_TIMEOUT 120)
    endif()
    if(NOT NT_LABELS)
        set(NT_LABELS "core;negative")
    endif()
    set(_cc ${CMAKE_C_COMPILER})
    if(NT_CC STREQUAL "g++")
        set(_cc ${CMAKE_CXX_COMPILER})
    endif()
    set(_includes)
    foreach(dir ${NT_INCLUDES})
        list(APPEND _includes -I ${dir})
    endforeach()
    add_test(NAME ${name}
        COMMAND Python3::Interpreter ${H}/negative_compile_check.py
            --cc ${_cc} --std ${NT_STD} --source ${NT_SOURCE} ${_includes}
            ${NT_EXTRA_ARGS}
        WORKING_DIRECTORY ${R})
    set_tests_properties(${name} PROPERTIES TIMEOUT ${NT_TIMEOUT} LABELS "${NT_LABELS}")
endfunction()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# The source gates are Python scripts kept outside the published tree (the
# maintainers' own tooling), registered by a local CMake file. They run only
# where that file exists, so a plain checkout configures and passes on the
# self-contained tests alone. Force with -DKISAK_LOCAL_GATES=ON/OFF.
set(KISAK_LOCAL_GATES_FILE "${R}/scripts/automation/host-gates.cmake")
if(EXISTS "${KISAK_LOCAL_GATES_FILE}")
    set(_local_gates_default ON)
else()
    set(_local_gates_default OFF)
endif()
option(KISAK_LOCAL_GATES "Register the source-gate tests that need the maintainers' local scripts" ${_local_gates_default})
if(KISAK_LOCAL_GATES)
    include("${KISAK_LOCAL_GATES_FILE}")
endif()
include(script_group.cmake)
include(negative_checks.cmake)
include(core_group.cmake)
