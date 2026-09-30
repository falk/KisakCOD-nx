# host-script group: the real script parser/compiler/VM run as an in-process
# host regression harness (see ./test's scr_parsetree_sanitizer_check /
# scr_yacc_sanitizer_check / scr_compile_sanitizer_check /
# scr_save_roundtrip_sanitizer_check for the "why" -- LP64 bugs in
# Scr_AllocNode, the yacc sval_u pointer-laundering bug, and the savegame
# script-segment round trip).
#
# Every target here also compiles src/platform/switch/switch_platform.c WITHOUT
# __SWITCH__ (its host branch) while every C++ source in the same target
# needs __SWITCH__ defined; a $<COMPILE_LANGUAGE:CXX> generator expression
# scopes the C++-only defines/includes so the one .c source in each target
# doesn't see them, matching the two separate compiler invocations
# (`gcc ... -c platform_file` vs. the C++ TUs) the original script used.

set(SCR_INCLUDES ${R} ${S} ${R}/deps ${R}/deps/d3d9-headers/directx
    ${R}/deps/d3d9-headers/windows ${R}/deps/d3d9-headers)
set(SCR_CXX_DEFS
    $<$<COMPILE_LANGUAGE:CXX>:__SWITCH__>
    $<$<COMPILE_LANGUAGE:CXX>:__cdecl=>
    $<$<COMPILE_LANGUAGE:CXX>:__stdcall=>
    $<$<COMPILE_LANGUAGE:CXX>:_vsnprintf=vsnprintf>
    $<$<COMPILE_LANGUAGE:CXX>:__unix__>
    $<$<COMPILE_LANGUAGE:CXX>:__int8=char>
    $<$<COMPILE_LANGUAGE:CXX>:__int16=short>
    $<$<COMPILE_LANGUAGE:CXX>:__int32=int>
    $<$<COMPILE_LANGUAGE:CXX>:__forceinline=inline>)
# Function-style macros (parens) and macros whose value has a space can't
# go through target_compile_definitions() -- CMake drops the former outright
# and mis-splits the latter into two list items -- so both go through
# target_compile_options() as literal, individually-quoted -D flags instead.
set(SCR_CXX_OPT_DEFS
    "$<$<COMPILE_LANGUAGE:CXX>:-D__declspec(x)=>"
    "$<$<COMPILE_LANGUAGE:CXX>:-D__int64=long long>")
# -Wno-class-memaccess/-Wno-deprecated-copy are C++-only warnings GCC
# rejects outright on a .c TU (an error under -Werror, not just unused), so
# every item here is scoped to CXX -- switch_platform.c's separate compile
# in the same target never asked for them either.
foreach(_flag -Wno-unused-parameter -Wno-return-type -Wno-int-to-pointer-cast
        -Wno-unused-function -Wno-unused-variable -Wno-class-memaccess -Wno-psabi
        -Wno-deprecated-copy)
    list(APPEND SCR_WARN_RELAX "$<$<COMPILE_LANGUAGE:CXX>:${_flag}>")
endforeach()

# --- scr-parsetree-sanitized -------------------------------------------
add_executable(scr-parsetree-sanitized
    ${H}/switch_script_parsetree_test.cpp
    ${S}/script/scr_parsetree.cpp
    ${S}/platform/switch/switch_hunk_user.cpp
    ${S}/platform/switch/switch_platform.c)
set_target_properties(scr-parsetree-sanitized PROPERTIES C_STANDARD 11 CXX_STANDARD 20)
target_include_directories(scr-parsetree-sanitized PRIVATE ${SCR_INCLUDES})
target_compile_definitions(scr-parsetree-sanitized PRIVATE KISAK_SP ${SCR_CXX_DEFS})
target_compile_options(scr-parsetree-sanitized PRIVATE ${SCR_CXX_OPT_DEFS})
target_compile_options(scr-parsetree-sanitized PRIVATE -Wall -Wextra -Werror
    -fsanitize=address,undefined ${SCR_WARN_RELAX}
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${H}/switch_script_parsetree_test_preamble.h>")
target_link_options(scr-parsetree-sanitized PRIVATE -fsanitize=address,undefined)
add_test(NAME scr-parsetree-sanitized COMMAND scr-parsetree-sanitized WORKING_DIRECTORY ${R})
set_tests_properties(scr-parsetree-sanitized PROPERTIES LABELS "script" TIMEOUT 300
    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")

# --- scr-yacc-sanitized --------------------------------------------------
add_executable(scr-yacc-sanitized
    ${H}/switch_script_yacc_test.cpp
    ${S}/script/scr_yacc2.cpp
    ${S}/script/scr_parsetree.cpp
    ${S}/platform/switch/switch_hunk_user.cpp
    ${S}/platform/switch/switch_platform.c)
set_target_properties(scr-yacc-sanitized PROPERTIES C_STANDARD 11 CXX_STANDARD 20)
target_include_directories(scr-yacc-sanitized PRIVATE ${SCR_INCLUDES})
target_compile_definitions(scr-yacc-sanitized PRIVATE KISAK_SP ${SCR_CXX_DEFS})
target_compile_options(scr-yacc-sanitized PRIVATE ${SCR_CXX_OPT_DEFS})
target_compile_options(scr-yacc-sanitized PRIVATE -Wall -Wextra -Werror
    -fsanitize=address,undefined ${SCR_WARN_RELAX} -Wno-unknown-pragmas -Wno-unused-label
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${H}/switch_script_yacc_test_preamble.h>")
target_link_options(scr-yacc-sanitized PRIVATE -fsanitize=address,undefined)
add_test(NAME scr-yacc-sanitized COMMAND scr-yacc-sanitized WORKING_DIRECTORY ${R})
set_tests_properties(scr-yacc-sanitized PROPERTIES LABELS "script" TIMEOUT 300
    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")

# --- scr-compile-sanitized (real parser+compiler+VM end to end) --------
set(SCR_COMPILE_SOURCES
    ${S}/script/scr_main.cpp
    ${S}/script/scr_compiler2.cpp
    ${S}/script/scr_parser.cpp
    ${S}/script/scr_parsetree.cpp
    ${S}/script/scr_yacc2.cpp
    ${S}/script/scr_evaluate.cpp
    ${S}/script/scr_variable.cpp
    ${S}/script/scr_vm.cpp
    ${S}/script/scr_stringlist.cpp
    ${S}/script/scr_memorytree.cpp
    ${S}/platform/switch/switch_hunk_user.cpp
    ${S}/platform/switch/switch_hunk_core.cpp
    ${S}/universal/com_memory.cpp)
set(SCR_COMPILE_INCLUDES ${SCR_INCLUDES} ${R}/src/platform/switch/include)
# scr_compile_cxxflags has no -Werror (unlike parsetree/yacc): the compat
# headers below (windows.h shim, xanim/UI host preludes) are noisier.
set(SCR_COMPILE_WARN_RELAX ${SCR_WARN_RELAX} -Wno-unknown-pragmas -Wno-unused-label
    -Wno-switch -Wno-sign-compare -Wno-parentheses -Wno-unused-value -Wno-volatile
    -Wno-format-security -Wno-missing-field-initializers)
set(SCR_COMPILE_INCLUDE_FLAGS
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${H}/switch_retail_ui_compat.h>"
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${H}/switch_retail_xanim_sl_host.h>"
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${H}/switch_retail_ui_host_prelude.h>")

add_executable(scr-compile-sanitized
    ${H}/switch_script_compile_test.cpp ${SCR_COMPILE_SOURCES} ${S}/platform/switch/switch_platform.c)
set_target_properties(scr-compile-sanitized PROPERTIES C_STANDARD 11 CXX_STANDARD 20)
target_include_directories(scr-compile-sanitized PRIVATE ${SCR_COMPILE_INCLUDES})
target_compile_definitions(scr-compile-sanitized PRIVATE KISAK_SP ${SCR_CXX_DEFS})
target_compile_options(scr-compile-sanitized PRIVATE ${SCR_CXX_OPT_DEFS})
target_compile_options(scr-compile-sanitized PRIVATE -Wall -Wextra -fsanitize=address,undefined
    ${SCR_COMPILE_WARN_RELAX} ${SCR_COMPILE_INCLUDE_FLAGS}
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${H}/switch_script_compile_test_preamble.h>")
target_compile_options(scr-compile-sanitized PRIVATE $<$<COMPILE_LANGUAGE:C>:-Werror>)
target_link_options(scr-compile-sanitized PRIVATE -fsanitize=address,undefined)
add_test(NAME scr-compile-sanitized COMMAND scr-compile-sanitized WORKING_DIRECTORY ${R})
set_tests_properties(scr-compile-sanitized PROPERTIES LABELS "script" TIMEOUT 300
    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")

# --- scr-save-roundtrip-sanitized (script save/load, real VM) ----------
# Same recipe as scr-compile, minus the UI/xanim/compile-test preludes, plus
# the save/read-write TUs, the vendored zlib deflate/inflate objects
# (memfile.cpp compresses/decompresses through them), and -fno-sanitize=vptr
# (scr_readwrite.cpp's debugger hooks call virtuals on debugger-UI classes
# this harness never links; those hooks are never reached at runtime since
# nothing here sets scrVarPub.developer, but UBSan's vptr check still wants
# their typeinfo at compile/link time).
add_library(scr_save_roundtrip_zlib OBJECT
    ${R}/deps/zlib/adler32.c ${R}/deps/zlib/crc32.c ${R}/deps/zlib/deflate.c
    ${R}/deps/zlib/trees.c ${R}/deps/zlib/inflate.c ${R}/deps/zlib/inffast.c
    ${R}/deps/zlib/inftrees.c ${R}/deps/zlib/zutil.c)
target_include_directories(scr_save_roundtrip_zlib PRIVATE ${R}/deps)
target_compile_options(scr_save_roundtrip_zlib PRIVATE -fsanitize=address,undefined)
set_target_properties(scr_save_roundtrip_zlib PROPERTIES C_STANDARD 11)

add_executable(scr-save-roundtrip-sanitized
    ${H}/switch_script_save_roundtrip_test.cpp
    ${H}/switch_script_save_roundtrip_globals.cpp
    ${SCR_COMPILE_SOURCES}
    ${S}/script/scr_readwrite.cpp
    ${S}/universal/memfile.cpp
    ${S}/platform/switch/switch_platform.c
    $<TARGET_OBJECTS:scr_save_roundtrip_zlib>)
set_target_properties(scr-save-roundtrip-sanitized PROPERTIES C_STANDARD 11 CXX_STANDARD 20)
target_include_directories(scr-save-roundtrip-sanitized PRIVATE ${SCR_COMPILE_INCLUDES})
target_compile_definitions(scr-save-roundtrip-sanitized PRIVATE KISAK_SP ${SCR_CXX_DEFS})
target_compile_options(scr-save-roundtrip-sanitized PRIVATE ${SCR_CXX_OPT_DEFS})
target_compile_options(scr-save-roundtrip-sanitized PRIVATE -Wall -Wextra
    -fsanitize=address,undefined -fno-sanitize=vptr ${SCR_COMPILE_WARN_RELAX}
    ${SCR_COMPILE_INCLUDE_FLAGS}
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${H}/switch_script_compile_test_preamble.h>")
target_compile_options(scr-save-roundtrip-sanitized PRIVATE $<$<COMPILE_LANGUAGE:C>:-Werror>)
target_link_options(scr-save-roundtrip-sanitized PRIVATE -fsanitize=address,undefined -fno-sanitize=vptr)
add_test(NAME scr-save-roundtrip-sanitized COMMAND scr-save-roundtrip-sanitized WORKING_DIRECTORY ${R})
set_tests_properties(scr-save-roundtrip-sanitized PROPERTIES LABELS "script" TIMEOUT 300
    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
# NOTE: not yet ported -- the second invocation
# (`scr-save-roundtrip-sanitized --load-savegame <newest .svg under
# $KISAK_SAVE_PROBE_DIR>`), which SKIPs when no savegame is staged and
# otherwise round-trips real production save data. Still runs via
# `./test host-script`'s scr_save_roundtrip_sanitizer_check.
