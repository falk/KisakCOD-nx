# Horizon (Nintendo Switch) build of KisakCOD-sp.  Included from
# scripts/platform/switch/platform.cmake while scripts/sp is configured, so
# every Switch-specific decision lives here and scripts/sp/CMakeLists.txt
# stays upstream's.  Configure with `cmake --preset switch-sp`.

if (NOT KISAK_PLATFORM STREQUAL "switch")
    message(FATAL_ERROR "KISAK_PLATFORM is incorrect for building switch.")
endif()
if (NOT NINTENDO_SWITCH)
    message(FATAL_ERROR "The Switch build needs the devkitPro toolchain: "
        "set DEVKITPRO and run `cmake --preset switch-sp`.")
endif()

set(_devkitpro "$ENV{DEVKITPRO}")
set(_libnx "${_devkitpro}/libnx")
set(_portlibs "${_devkitpro}/portlibs/switch")

set(KISAK_SWITCH_LOG_HOST "" CACHE STRING
    "Optional IPv4 host that receives the nxlink stdout stream")
set(KISAK_SWITCH_NACP_NAME "KisakCOD SP" CACHE STRING "Title shown by the Homebrew Menu")
option(KISAK_DEKO3D_DEBUG "Link the validating libdeko3dd" OFF)
option(KISAK_SWITCH_HEAP_CHECK "Link the redzone/quarantine heap checker" OFF)

# Missing devkitPro packages fail here with the package name, not at link.
foreach (_required
    "${_libnx}/lib/libnx.a"
    "${_libnx}/lib/libdeko3d.a"
    "${_libnx}/lib/libdeko3dd.a"
    "${_portlibs}/include/AL/al.h"
    "${_portlibs}/lib/libavcodec.a")
    if (NOT EXISTS "${_required}")
        message(FATAL_ERROR "Missing ${_required}; install the devkitPro packages "
            "libnx deko3d switch-openal-soft switch-ffmpeg (dkp-pacman -S switch-dev "
            "deko3d switch-openal-soft switch-ffmpeg)")
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/switch-deps.cmake")

# -fsigned-char: AArch64 GCC defaults to unsigned char, but the decompiled
# retail sources and the wire structs treat plain char as signed (q_shared.h
# carries a matching #error guard).
add_library(switch_libnx INTERFACE)
target_compile_options(switch_libnx INTERFACE
    -march=armv8-a+crc+crypto -mtp=soft -fPIE -fsigned-char)
target_link_options(switch_libnx INTERFACE -fPIE)

# --- deko3d renderer: the D3D9 interface the engine calls, on deko3d --------
set(_deko9_dk_lib "${_libnx}/lib/libdeko3d.a")
if (KISAK_DEKO3D_DEBUG)
    # Bad inputs reach deko9's debug callback as FAIL:DEKO9_DK lines.
    set(_deko9_dk_lib "${_libnx}/lib/libdeko3dd.a")
endif()

add_library(switch_deko9 STATIC
    "${SRC_DIR}/deko9/deko9_drawcensus.cpp"
    "${SRC_DIR}/deko9/deko9_callcensus.cpp"
    "${SRC_DIR}/deko9/deko9_d3d.cpp"
    "${SRC_DIR}/deko9/deko9_device.cpp"
    "${SRC_DIR}/deko9/deko9_draw.cpp"
    "${SRC_DIR}/deko9/deko9_fsr.cpp"
    "${SRC_DIR}/deko9/deko9_fsr_shaders.cpp"
    "${SRC_DIR}/deko9/deko9_gpufault.cpp"
    "${SRC_DIR}/deko9/deko9_memory.cpp"
    "${SRC_DIR}/deko9/deko9_resources.cpp"
    "${SRC_DIR}/deko9/deko9_shader.cpp"
    "${SRC_DIR}/deko9/deko9_state_map.cpp"
    "${SRC_DIR}/deko9/deko9_zcull.cpp")
target_include_directories(switch_deko9 PRIVATE
    "${DEPS_DIR}/d3d9-headers/directx"
    "${DEPS_DIR}/d3d9-headers/windows"
    "${DEPS_DIR}/d3d9-headers"
    "${mojoshader_SOURCE_DIR}"
    "${SRC_DIR}")
target_include_directories(switch_deko9 SYSTEM PRIVATE ${KISAK_UAM_INCLUDE_DIRS})
target_compile_definitions(switch_deko9 PRIVATE __cdecl= __stdcall=)
target_compile_options(switch_deko9 PRIVATE
    "-D__declspec(x)=" -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(switch_deko9 PRIVATE uam switch_libnx)
if (KISAK_DEKO3D_DEBUG)
    # Canary reads of GPU-mapped memory cost ~8% of the main thread.
    target_compile_definitions(switch_deko9 PRIVATE DEKO9_CANARY_CHECKS)
endif()

add_library(switch_d3d9_renderer INTERFACE)
target_link_libraries(switch_d3d9_renderer INTERFACE
    switch_deko9 uam mojoshader "${_deko9_dk_lib}" switch_libnx)

# --- optimisation: LTO (default) and PGO (opt-in) ---------------------------
# ON adds -flto=auto to the game and the deko9 archive; gc is an alias of ON.
set(KISAK_SWITCH_LTO "ON" CACHE STRING
    "SP Switch build LTO: ON (-flto=auto), OFF (per-TU codegen), or gc (same as ON)")
set_property(CACHE KISAK_SWITCH_LTO PROPERTY STRINGS OFF ON gc)
# gen: instrumented build; libgcov writes .gcda files to KISAK_SWITCH_PGO_SDDIR
#      on the SD card when the switch_pgoDump console command runs.
# use: -fprofile-use from KISAK_SWITCH_PGO_DIR (the .gcda files copied off SD).
set(KISAK_SWITCH_PGO "OFF" CACHE STRING "SP Switch build PGO: OFF, gen (instrumented), use")
set_property(CACHE KISAK_SWITCH_PGO PROPERTY STRINGS OFF gen use)
set(KISAK_SWITCH_PGO_SDDIR "/switch/kisakcod/pgo" CACHE STRING
    "gen: absolute .gcda directory on the Switch default device (sdmc:)")
set(KISAK_SWITCH_PGO_DIR "" CACHE PATH "use: host directory holding the training .gcda files")
if (NOT KISAK_SWITCH_PGO MATCHES "^(OFF|gen|use)$")
    message(FATAL_ERROR "KISAK_SWITCH_PGO must be OFF, gen or use (got ${KISAK_SWITCH_PGO})")
endif()
if (KISAK_SWITCH_PGO STREQUAL "use" AND NOT IS_DIRECTORY "${KISAK_SWITCH_PGO_DIR}")
    message(FATAL_ERROR "KISAK_SWITCH_PGO=use needs KISAK_SWITCH_PGO_DIR (a .gcda directory)")
endif()

# Sources Horizon does not build (the boundary rule: MSS audio, Bink, the
# Win32 shell, the x86 skinning and the Win32 thread/PMem backends), captured
# here because the scripts/sp lists are out of scope inside the deferred call.
set_property(GLOBAL PROPERTY KISAK_SWITCH_ENGINE_LISTS
    "${MSSLIB};${SPEEX};${BINKLIB};${WIN32_SRC}")

# Runs once scripts/sp has created the KisakCOD-sp target.
function(kisak_switch_configure_sp)
    set(target KisakCOD-sp)
    get_property(_engine_lists GLOBAL PROPERTY KISAK_SWITCH_ENGINE_LISTS)
    get_target_property(SP_SOURCES ${target} SOURCES)

    list(APPEND SP_SOURCES
        "${SRC_DIR}/platform/switch/switch_fast_critical_section.cpp"
        "${SRC_DIR}/platform/switch/switch_input.c"
        "${SRC_DIR}/platform/switch/switch_input_lifecycle.cpp"
        "${SRC_DIR}/platform/switch/switch_thread.cpp"
        "${SRC_DIR}/platform/switch/switch_thread_sync.cpp"
        # LEvent owner behind switch_thread.cpp's Sys_*Event surface;
        # qcommon/threads.cpp (the Win32 backend) is excluded below.
        "${SRC_DIR}/platform/switch/switch_event.cpp"
        "${SRC_DIR}/platform/switch/switch_platform.c"
        "${SRC_DIR}/platform/switch/switch_sys_event.cpp"
        "${SRC_DIR}/platform/switch/switch_critical_section.cpp"
        "${SRC_DIR}/platform/switch/switch_pmem.cpp"
        "${SRC_DIR}/platform/switch/switch_pmem_stats.cpp"
        "${SRC_DIR}/port/switch_snapvector.cpp"
        "${SRC_DIR}/port/switch_quicksave.cpp"
        "${SRC_DIR}/port/switch_gyro.cpp"
        "${SRC_DIR}/port/switch_rumble.cpp"
        "${SRC_DIR}/platform/switch/switch_clocks.cpp"
        # Statistical stack sampler behind `switch_pcSample` (off by default).
        "${SRC_DIR}/port/switch_pcsample.cpp"
        # In-process crash reporter: prints CRASH: lines to the nxlink stream.
        "${SRC_DIR}/platform/switch/switch_crash.cpp"
        "${SRC_DIR}/platform/switch/switch_sp_main.cpp"
        "${SRC_DIR}/port/switch_screenshot.cpp"
        "${SRC_DIR}/port/switch_cinematic_decode.cpp"
        # Cinematics decode the retail .bik through the FFmpeg portlib (the
        # Bink SDK is never ported) behind the R_Cinematic_* interface.
        "${SRC_DIR}/port/switch_cinematic_ffmpeg.cpp"
        "${SRC_DIR}/port/switch_excluded_boundaries.cpp"
        "${SRC_DIR}/port/switch_material_residue.cpp"
        "${SRC_DIR}/port/switch_misc_stubs.cpp"
        # r_model_skin_simd.cpp is r_model_skin_sse.cpp with GCC vector
        # extensions (NEON), byte-identical by switch_model_skin_simd_test.
        "${SRC_DIR}/gfx_d3d/r_model_skin_simd.cpp")
    if (KISAK_SWITCH_HEAP_CHECK)
        list(APPEND SP_SOURCES "${SRC_DIR}/port/switch_heapcheck.cpp")
    endif()
    if (KISAK_SWITCH_PGO STREQUAL "gen")
        list(APPEND SP_SOURCES "${SRC_DIR}/platform/switch/switch_pgo.cpp")
    endif()

    # gfx_d3d/r_cinematic.cpp and r_screenshot.cpp are replaced by the port
    # files above.  r_material_load_obj.cpp is the loose-file material
    # compiler, reachable only when IsFastFileLoad() is false, and needs
    # d3dx9shader.h.  qcommon/threads.cpp and universal/physicalmemory.cpp
    # are Win32 backends that switch_thread.cpp / switch_pmem.cpp replace.
    # r_model_skin_sse.cpp needs x86 intrinsics.
    list(REMOVE_ITEM SP_SOURCES
        "${SRC_DIR}/gfx_d3d/r_cinematic.cpp"
        "${SRC_DIR}/gfx_d3d/r_screenshot.cpp"
        "${SRC_DIR}/gfx_d3d/r_material_load_obj.cpp"
        "${SRC_DIR}/qcommon/threads.cpp"
        "${SRC_DIR}/universal/physicalmemory.cpp"
        "${SRC_DIR}/gfx_d3d/r_model_skin_sse.cpp"
        ${_engine_lists}
        "${SRC_DIR}/universal/win_common.cpp"
        "${SRC_DIR}/universal/win_shared.cpp")
    set_property(TARGET ${target} PROPERTY SOURCES ${SP_SOURCES})
    add_dependencies(${target} update_build_number)

    target_compile_definitions(${target} PUBLIC
        __SWITCH__ __cdecl= __stdcall= __thiscall= __fastcall= STRICT
        _vsnprintf=vsnprintf)
    # CMake drops function-like macro definitions, so this rides as an option.
    target_compile_options(${target} PUBLIC "-D__declspec(x)="
        -include "${SRC_DIR}/platform/switch/switch_compat.h")
    # The stubs shadow the Win32-only headers under deps/ (binklib/), so they
    # must precede DEPS_DIR.
    target_include_directories(${target} PUBLIC
        "${SRC_DIR}" "${SRC_DIR}/platform/switch/include" "${DEPS_DIR}"
        "${SRC_DIR}/DynEntity" "${SRC_DIR}/gfx_d3d" "${SRC_DIR}/qcommon"
        "${SRC_DIR}/universal"
        "${DEPS_DIR}/d3d9-headers/directx"
        "${DEPS_DIR}/d3d9-headers/windows"
        "${DEPS_DIR}/d3d9-headers")

    target_link_libraries(${target} PRIVATE switch_d3d9_renderer)
    if (KISAK_SWITCH_LOG_HOST)
        target_compile_definitions(${target} PRIVATE
            KISAK_SWITCH_LOG_HOST="${KISAK_SWITCH_LOG_HOST}")
    endif()

    # Sound: only the switch-openal-soft headers are used, for the OpenAL
    # types the shared code is written against; every al* call is routed to
    # the audio renderer, and libopenal is never linked.  libnx's audrv has no
    # effect section, so snd_audren_switch.cpp wraps its one renderer call to
    # splice the reverb effect in.
    target_compile_definitions(${target} PUBLIC KISAK_OPENAL AL_LIBTYPE_STATIC)
    # The dr_wav/dr_mp3 implementations belong in exactly this one TU.
    set_source_files_properties("${SRC_DIR}/sound/snd_driver_load_obj.cpp" PROPERTIES
        COMPILE_DEFINITIONS "DR_WAV_IMPLEMENTATION;DR_MP3_IMPLEMENTATION")
    target_include_directories(${target} PRIVATE "${_portlibs}/include")
    target_link_options(${target} PRIVATE -Wl,--wrap=audrenRequestUpdateAudioRenderer)

    # FFmpeg: static archives, dependents before providers.
    target_link_directories(${target} PRIVATE "${_portlibs}/lib")
    target_link_libraries(${target} PRIVATE
        avformat avcodec swscale swresample avutil bz2 dav1d z)

    if (KISAK_SWITCH_HEAP_CHECK)
        # Wraps newlib's allocator with redzones and a free quarantine to name
        # the writer behind heap corruption.  Diagnostic builds only.
        target_compile_definitions(${target} PRIVATE KISAK_SWITCH_HEAP_CHECK)
        target_link_options(${target} PRIVATE
            -Wl,--wrap=_malloc_r -Wl,--wrap=_free_r -Wl,--wrap=_realloc_r
            -Wl,--wrap=_calloc_r -Wl,--wrap=_memalign_r -Wl,--wrap=_malloc_usable_size_r)
    endif()

    if (NOT KISAK_SWITCH_LTO STREQUAL "OFF")
        target_compile_options(${target} PRIVATE -flto=auto -flto-partition=balanced)
        # LTRANS takes its optimisation level from the link command.
        target_link_options(${target} PRIVATE -flto=auto -flto-partition=balanced -O2)
        # IPO (not a bare -flto) so the archive is built with gcc-ar.
        set_property(TARGET switch_deko9 PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
        if (KISAK_SWITCH_LTO STREQUAL "gc")
            target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections)
            target_link_options(${target} PRIVATE -Wl,--gc-sections)
        endif()
    endif()

    if (KISAK_SWITCH_PGO STREQUAL "gen")
        # -fprofile-prefix-path keeps the .gcda names relative to the build
        # dir, so the gen and use builds may live in different directories.
        set(_pgo_flags "-fprofile-generate=${KISAK_SWITCH_PGO_SDDIR}"
            -fprofile-update=prefer-atomic "-fprofile-prefix-path=${CMAKE_BINARY_DIR}"
            -ftest-coverage)
        # switch_pgo.cpp mirrors libnx's thread pointer into TPIDR_EL0 at
        # every thread start for libgcov's hardware-TLS profiler.
        target_link_options(${target} PRIVATE -Wl,--wrap=threadCreate)
        target_compile_definitions(${target} PRIVATE KISAK_SWITCH_PGO_GEN
            "KISAK_SWITCH_PGO_SDDIR=\"${KISAK_SWITCH_PGO_SDDIR}\"")
    elseif (KISAK_SWITCH_PGO STREQUAL "use")
        # Partial training keeps functions the training run never reached
        # optimised normally; stale profiles report, not fail.
        set(_pgo_flags "-fprofile-use=${KISAK_SWITCH_PGO_DIR}" -fprofile-partial-training
            "-fprofile-prefix-path=${CMAKE_BINARY_DIR}" -Wno-error=coverage-mismatch)
    endif()
    if (KISAK_SWITCH_PGO STREQUAL "gen" OR KISAK_SWITCH_PGO STREQUAL "use")
        target_compile_options(${target} PRIVATE ${_pgo_flags})
        target_compile_options(switch_deko9 PRIVATE ${_pgo_flags})
        target_link_options(${target} PRIVATE ${_pgo_flags})
    endif()

    # ELF beside the NRO rather than in <repo>/bin.
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    nx_generate_nacp(OUTPUT "${CMAKE_BINARY_DIR}/${target}.nacp"
        NAME "${KISAK_SWITCH_NACP_NAME}" AUTHOR kisak-switch VERSION 0.1.0)
    nx_create_nro(${target}
        OUTPUT "${CMAKE_BINARY_DIR}/${target}.nro"
        NACP "${CMAKE_BINARY_DIR}/${target}.nacp")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" CALL kisak_switch_configure_sp)
