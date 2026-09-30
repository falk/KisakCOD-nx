# host-core group: a representative, growing slice of ./test's ~50
# host-core compile+run checks (see this dir's CMakeLists.txt top comment
# for what is/isn't covered yet). Grouped by shared shape, cheapest first.

# Most of these sources use repo-root-relative quoted includes (e.g.
# switch_skeleton.c's `#include "src/platform/switch/switch_platform.h"`, a
# convention left over from living at the repo root, same as ./test's own
# CPATH export at the top of the script); a directory-scope include so every
# target below (however it's declared) resolves them without repeating -I
# on each kisak_host_test() call.
include_directories(${R})

set(D3D9_INCLUDES -I${R}/deps/d3d9-headers/directx -I${R}/deps/d3d9-headers/windows -I${R}/deps/d3d9-headers)

# ---- plain C unit tests (switch_platform.c + one test.c, -std=c11) ------
foreach(pair IN ITEMS
        "core-skeleton|switch_skeleton.c"
        "core-logging|switch_logging_test.c"
        "core-timing|switch_timing_test.c"
        "core-timing-wrap|switch_timing_wrap_test.c"
        "core-timing-smoke|switch_timing_smoke_test.c"
        "core-filesystem|switch_filesystem_test.c"
        "core-file-list|switch_file_list_test.c")
    string(REPLACE "|" ";" pair "${pair}")
    list(GET pair 0 tname)
    list(GET pair 1 tsrc)
    kisak_host_test(${tname}
        SOURCES ${H}/${tsrc} ${S}/platform/switch/switch_platform.c
        COMPILE_OPTIONS -std=c11 -Wall -Wextra -Werror
        DEFINES KISAK_SP
        LABELS "core")
endforeach()

# ---- plain + ASan C unit tests -------------------------------------------
foreach(pair IN ITEMS "core-paths|switch_paths_test.c" "core-virtual-memory|switch_virtual_memory_test.c")
    string(REPLACE "|" ";" pair "${pair}")
    list(GET pair 0 tname)
    list(GET pair 1 tsrc)
    kisak_host_test(${tname}
        SOURCES ${H}/${tsrc} ${S}/platform/switch/switch_platform.c
        COMPILE_OPTIONS -std=c11 -Wall -Wextra -Werror
        DEFINES KISAK_SP
        LABELS "core")
    kisak_host_test(${tname}-asan
        SOURCES ${H}/${tsrc} ${S}/platform/switch/switch_platform.c
        LIBS host_asan_ubsan
        COMPILE_OPTIONS -std=c11 -Wall -Wextra -Werror
        DEFINES KISAK_SP
        LABELS "core")
endforeach()

kisak_host_test(core-input
    SOURCES ${H}/switch_input_test.c ${S}/platform/switch/switch_input.c ${S}/platform/switch/switch_platform.c
    COMPILE_OPTIONS -std=c11 -Wall -Wextra -Werror DEFINES KISAK_SP LABELS "core")
kisak_host_test(core-input-asan
    SOURCES ${H}/switch_input_test.c ${S}/platform/switch/switch_input.c ${S}/platform/switch/switch_platform.c
    LIBS host_asan_ubsan COMPILE_OPTIONS -std=c11 -Wall -Wextra -Werror DEFINES KISAK_SP LABELS "core")

# ---- file_list_linkage: C platform.o (no __SWITCH__) + a C++ front -------
add_executable(core-file-list-linkage ${H}/switch_file_list_linkage_test.cpp ${S}/platform/switch/switch_platform.c)
set_target_properties(core-file-list-linkage PROPERTIES C_STANDARD 11 CXX_STANDARD 11)
target_compile_definitions(core-file-list-linkage PRIVATE KISAK_SP $<$<COMPILE_LANGUAGE:CXX>:__SWITCH__>)
target_compile_options(core-file-list-linkage PRIVATE -Wall -Wextra -Werror)
add_test(NAME core-file-list-linkage COMMAND core-file-list-linkage WORKING_DIRECTORY ${R})
set_tests_properties(core-file-list-linkage PROPERTIES LABELS "core" TIMEOUT 300)

# ---- hunk_user / hunk_core / pmem: C++11, __SWITCH__ (CXX-only) ---------
foreach(spec IN ITEMS
        "core-hunk-user-asan|switch_hunk_user_test.cpp|platform/switch/switch_hunk_user.cpp"
        "core-hunk-core-asan|switch_hunk_core_test.cpp|platform/switch/switch_hunk_core.cpp"
        "core-pmem-asan|switch_pmem_test.cpp|platform/switch/switch_pmem.cpp")
    string(REPLACE "|" ";" spec "${spec}")
    list(GET spec 0 tname)
    list(GET spec 1 tsrc)
    list(GET spec 2 impl)
    add_executable(${tname} ${H}/${tsrc} ${S}/${impl} ${S}/platform/switch/switch_platform.c)
    set_target_properties(${tname} PROPERTIES C_STANDARD 11 CXX_STANDARD 11)
    target_compile_definitions(${tname} PRIVATE KISAK_SP $<$<COMPILE_LANGUAGE:CXX>:__SWITCH__>)
    target_compile_options(${tname} PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
    target_link_options(${tname} PRIVATE -fsanitize=address,undefined)
    add_test(NAME ${tname} COMMAND ${tname} WORKING_DIRECTORY ${R})
    set_tests_properties(${tname} PROPERTIES LABELS "core" TIMEOUT 300)
endforeach()

# ---- critical_section / event / thread: pure C++, no platform.c ---------
kisak_host_test(core-critical-section-asan
    SOURCES ${H}/switch_critical_section_test.cpp ${S}/platform/switch/switch_critical_section.cpp
    LIBS host_asan_ubsan
    COMPILE_OPTIONS -std=c++11 -Wall -Wextra -Werror -I${S}
    DEFINES __SWITCH__ KISAK_SP KISAK_SWITCH_CRITICAL_SECTION_PROOF_HOST
    LABELS "core")

kisak_host_test(core-event-asan
    SOURCES ${H}/switch_event_test.cpp ${S}/platform/switch/switch_event.cpp
    LIBS host_asan_ubsan
    COMPILE_OPTIONS -std=c++11 -Wall -Wextra -Werror
    DEFINES __SWITCH__ KISAK_SP KISAK_SWITCH_EVENT_PROOF_HOST
    LABELS "core")

set(THREAD_SOURCES ${H}/switch_thread_test.cpp ${S}/platform/switch/switch_thread.cpp
    ${S}/platform/switch/switch_thread_sync.cpp ${S}/platform/switch/switch_event.cpp ${S}/platform/switch/switch_critical_section.cpp)
set(THREAD_DEFINES __SWITCH__ KISAK_SP __cdecl= __stdcall=
    KISAK_SWITCH_THREAD_PROOF_HOST KISAK_SWITCH_EVENT_PROOF_HOST KISAK_SWITCH_CRITICAL_SECTION_PROOF_HOST)
kisak_host_test(core-thread-asan
    SOURCES ${THREAD_SOURCES} LIBS host_asan_ubsan
    COMPILE_OPTIONS -std=c++11 -Wall -Wextra -Werror -I${S}
    DEFINES ${THREAD_DEFINES} LABELS "core")
kisak_host_test(core-thread-tsan
    SOURCES ${THREAD_SOURCES} LIBS host_tsan
    COMPILE_OPTIONS -std=c++11 -Wall -Wextra -Werror -I${S}
    DEFINES ${THREAD_DEFINES} LABELS "core")

# ---- header-only C++17 sanitizer checks (pure algorithm/math headers) ---
kisak_host_test(core-sv-smoothing-asan
    SOURCES ${H}/switch_sv_smoothing_test.cpp LIBS host_asan_ubsan
    COMPILE_OPTIONS -std=c++17 -O1 -Wall -Wextra -Werror LABELS "core"
    ENV "ASAN_OPTIONS=detect_leaks=0")
kisak_host_test(core-msg-bits-asan
    SOURCES ${H}/switch_msg_bits_test.cpp LIBS host_asan_ubsan
    COMPILE_OPTIONS -std=c++17 -O1 -Wall -Wextra -Werror -I${S}
        -D__int8=char -D__int16=short -D__int32=int "-D__int64=long long" -D__cdecl= -D__stdcall=
    DEFINES KISAK_SP
    LABELS "core"
    ENV "ASAN_OPTIONS=detect_leaks=0")
kisak_host_test(core-gyro-asan
    SOURCES ${H}/switch_gyro_test.cpp LIBS host_asan_ubsan
    COMPILE_OPTIONS -std=c++17 -O1 -Wall -Wextra -Werror LABELS "core"
    ENV "ASAN_OPTIONS=detect_leaks=0")
kisak_host_test(core-rumble-asan
    SOURCES ${H}/switch_rumble_test.cpp LIBS host_asan_ubsan
    COMPILE_OPTIONS -std=c++17 -O1 -Wall -Wextra -Werror LABELS "core"
    ENV "ASAN_OPTIONS=detect_leaks=0")
kisak_host_test(core-handheld-perfconfig-asan
    SOURCES ${H}/switch_handheld_perfconfig_test.cpp LIBS host_asan_ubsan
    COMPILE_OPTIONS -std=c++11 -Wall -Wextra -Werror
    DEFINES KISAK_SP __SWITCH__ LABELS "core")

# ---- registry_size / scr_save_layout: single C++20 TU --------------------
kisak_host_test(core-registry-size-asan
    SOURCES ${H}/switch_registry_size_proof.cpp LIBS host_asan_ubsan
    COMPILE_OPTIONS -std=c++20 -Wall -Wextra -Werror -I${S} -I${R}/deps ${D3D9_INCLUDES}
        -Wno-unused-parameter -Wno-return-type -Wno-int-to-pointer-cast -Wno-unused-function
        -Wno-unused-variable -Wno-class-memaccess -Wno-psabi
        -D__cdecl= -D__stdcall= "-D__declspec(x)=" -D_vsnprintf=vsnprintf -D__unix__
    DEFINES __SWITCH__ KISAK_SP
    LABELS "core"
    ENV "ASAN_OPTIONS=detect_leaks=0")

kisak_host_test(core-scr-save-layout-asan
    SOURCES ${H}/switch_script_save_layout_test.cpp LIBS host_asan_ubsan
    COMPILE_OPTIONS -std=c++20 -Wall -Wextra -Werror -I${R} -I${S} -Wno-unused-parameter -Wno-unused-variable
        -D__cdecl= -D__stdcall= "-D__declspec(x)=" -D__forceinline=inline
    DEFINES __SWITCH__ KISAK_SP
    LABELS "core"
    ENV "ASAN_OPTIONS=detect_leaks=0")

# ---- fx_restore / snd_restore / save_field_layout: prod .cpp + test.cpp -
kisak_host_test(core-fx-restore-asan
    SOURCES ${R}/src/EffectsCore/fx_archive.cpp ${H}/switch_fx_restore_test.cpp
    LIBS host_asan_ubsan
    COMPILE_OPTIONS -std=c++20 -Wall -Wextra -ffunction-sections -fdata-sections -I${R} -I${S} -I${R}/deps ${D3D9_INCLUDES}
        -D_iobuf=FILE "SHELL:-include cstdio" "SHELL:-include ${H}/switch_retail_ui_compat.h"
        -Wno-return-type -Wno-int-to-pointer-cast -Wno-unused-function
        -Wno-unused-variable -Wno-unused-parameter -Wno-class-memaccess -Wno-psabi
        -D__cdecl= -D__stdcall= "-D__declspec(x)=" -D_vsnprintf=vsnprintf -D__unix__
    DEFINES KISAK_SP __SWITCH__ WIN32
    LINK_OPTIONS -Wl,--gc-sections
    LABELS "core"
    ENV "ASAN_OPTIONS=detect_leaks=0")

kisak_host_test(core-snd-restore-asan
    SOURCES ${R}/src/sound/snd.cpp ${H}/switch_snd_restore_test.cpp
    LIBS host_asan_ubsan
    COMPILE_OPTIONS -std=c++20 -Wall -Wextra -ffunction-sections -fdata-sections -I${R} -I${S} -I${R}/deps ${D3D9_INCLUDES}
        -idirafter /opt/devkitpro/portlibs/switch/include
        -D_iobuf=FILE "SHELL:-include cstdio" "SHELL:-include windows_base.h" "SHELL:-include ${H}/switch_retail_ui_compat.h"
        -Wno-return-type -Wno-int-to-pointer-cast -Wno-unused-function
        -Wno-unused-variable -Wno-unused-parameter -Wno-class-memaccess -Wno-psabi
        -D__cdecl= -D__stdcall= "-D__declspec(x)=" -D_vsnprintf=vsnprintf -D__unix__
    DEFINES KISAK_SP __SWITCH__ WIN32
    LINK_OPTIONS -Wl,--gc-sections
    LABELS "core"
    ENV "ASAN_OPTIONS=detect_leaks=0")

kisak_host_test(core-save-field-layout-asan
    SOURCES ${R}/src/game/g_save.cpp ${H}/switch_save_field_layout_test.cpp
    LIBS host_asan_ubsan
    COMPILE_OPTIONS -std=c++20 -Wall -Wextra -ffunction-sections -fdata-sections -I${R} -I${S} -I${R}/deps ${D3D9_INCLUDES}
        -D_iobuf=FILE "SHELL:-include cstdio" "SHELL:-include ${H}/switch_retail_ui_compat.h"
        -Wno-return-type -Wno-int-to-pointer-cast -Wno-unused-function
        -Wno-unused-variable -Wno-class-memaccess -Wno-psabi
        -D__cdecl= -D__stdcall= "-D__declspec(x)=" -D_vsnprintf=vsnprintf -D__unix__
    DEFINES KISAK_SP __SWITCH__ WIN32
    LINK_OPTIONS -Wl,--gc-sections
    LABELS "core"
    ENV "ASAN_OPTIONS=detect_leaks=0")

# ---- phys_pool_stride / switch_perf / switch_crash / mark_context_layout -
kisak_host_test(core-phys-pool-stride-asan
    SOURCES ${H}/switch_phys_pool_stride_test.cpp ${S}/universal/pool_allocator.cpp
    LIBS host_asan_ubsan
    COMPILE_OPTIONS -std=c++20 -Wall -Wextra -I${S} -I${R}/deps
        -Wno-unused-parameter -Wno-return-type
    DEFINES KISAK_SP __cdecl= __unix__
    ENV "ASAN_OPTIONS=detect_leaks=0"
    LABELS "core")

kisak_host_test(core-switch-perf-asan
    SOURCES ${H}/switch_perf_test.cpp ${S}/port/switch_perf.cpp
    LIBS host_asan_ubsan
    COMPILE_OPTIONS -std=c++11 -Wall -Wextra -Werror
    DEFINES KISAK_SP
    LABELS "core")

kisak_host_test(core-switch-crash-asan
    SOURCES ${H}/switch_crash_test.cpp ${S}/platform/switch/switch_crash.cpp
    LIBS host_asan_ubsan
    COMPILE_OPTIONS -std=c++17 -Wall -Wextra -Werror
    DEFINES KISAK_SP
    LABELS "core")
set_tests_properties(core-switch-crash-asan PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS:SWITCH_CRASH_FORMAT")

kisak_host_test(core-mark-context-layout-asan
    SOURCES ${H}/switch_mark_context_layout_test.cpp
    LIBS host_asan_ubsan
    COMPILE_OPTIONS -std=c++11 -Wall -Wextra -Werror
    LABELS "core")

# ---- snd_audren: real Switch sound backend vs. a fake audio renderer ----
foreach(spec IN ITEMS "core-snd-audren-asan|address,undefined|" "core-snd-audren-tsan|thread|--concurrency")
    string(REPLACE "|" ";" spec "${spec}")
    list(GET spec 0 tname)
    list(GET spec 1 saniti)
    list(GET spec 2 runarg)
    add_executable(${tname} ${H}/switch_snd_audren_test.cpp ${R}/src/sound/snd_audren_al.cpp)
    target_compile_definitions(${tname} PRIVATE KISAK_SND_AUDREN_HOST_TEST)
    target_compile_options(${tname} PRIVATE -std=c++17 -O1 -g -Wall -Wextra -Werror
        -fsanitize=${saniti} -idirafter /opt/devkitpro/portlibs/switch/include)
    if(saniti STREQUAL "address,undefined")
        target_compile_options(${tname} PRIVATE -fno-sanitize-recover=all)
    endif()
    target_link_options(${tname} PRIVATE -fsanitize=${saniti})
    target_link_libraries(${tname} PRIVATE dl pthread)
    if(runarg)
        add_test(NAME ${tname} COMMAND ${tname} ${runarg} WORKING_DIRECTORY ${R})
    else()
        add_test(NAME ${tname} COMMAND ${tname} WORKING_DIRECTORY ${R})
    endif()
    set_tests_properties(${tname} PROPERTIES LABELS "core" TIMEOUT 300)
endforeach()
