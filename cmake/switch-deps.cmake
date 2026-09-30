# Shader-translation dependencies of the deko3d renderer (src/deko9):
# MojoShader (D3D9 bytecode -> GLSL) and UAM (GLSL -> DKSH), built from
# source with the current toolchain.  Included by cmake/switch.cmake for the
# NRO and by cmake/host-deps for the host tools, so both share one recipe.
#
# deko3d itself is not fetched: libdeko3d/libdeko3dd from the devkitPro
# `deko3d` package are stock.
#
# UAM ships as a meson project whose library target is an executable
# upstream, and needs one patch (cmake/patches/uam-library.patch) to compile
# a shader to memory.  It is built here directly from its meson source lists;
# it needs bison, flex and python3 with the mako module on the build host.

include(FetchContent)

set(KISAK_MOJOSHADER_REV ad5dff84830c2863c841f4b1f4e3df78c705b383)
set(KISAK_UAM_REV 5a5afc2bae8b55409ab36ba45be63fcb73f68993)

# --- MojoShader -----------------------------------------------------------
# GLSL ES output only, no effects: what deko9's shader path consumes.
foreach (_opt PROFILE_D3D PROFILE_BYTECODE PROFILE_HLSL PROFILE_GLSL120
              PROFILE_ARB1 PROFILE_ARB1_NV PROFILE_METAL PROFILE_SPIRV
              PROFILE_GLSPIRV EFFECT_SUPPORT)
    set(${_opt} OFF CACHE BOOL "" FORCE)
endforeach()
set(PROFILE_GLSLES ON CACHE BOOL "" FORCE)
set(PROFILE_GLSLES3 ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(mojoshader
    GIT_REPOSITORY https://github.com/icculus/mojoshader
    GIT_TAG ${KISAK_MOJOSHADER_REV}
    EXCLUDE_FROM_ALL)

# --- UAM ------------------------------------------------------------------
# No CMakeLists.txt upstream: MakeAvailable only populates the source tree.
FetchContent_Declare(uam
    GIT_REPOSITORY https://github.com/devkitPro/uam
    GIT_TAG ${KISAK_UAM_REV}
    EXCLUDE_FROM_ALL)

FetchContent_MakeAvailable(mojoshader uam)
target_compile_options(mojoshader PRIVATE -O3)
# Its interface include paths are dxvk-native fallbacks the renderer never uses.
set_target_properties(mojoshader PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "")

find_program(KISAK_BISON bison)
find_program(KISAK_FLEX flex)
find_program(KISAK_PYTHON NAMES python3 python)
if (NOT KISAK_BISON OR NOT KISAK_FLEX OR NOT KISAK_PYTHON)
    message(FATAL_ERROR "Building UAM needs bison, flex and python3 with mako "
        "(e.g. apt install bison flex python3-mako)")
endif()
execute_process(COMMAND "${KISAK_PYTHON}" -c "import mako"
    RESULT_VARIABLE _mako_rc OUTPUT_QUIET ERROR_QUIET)
if (_mako_rc)
    message(FATAL_ERROR "Building UAM needs the python3 mako module "
        "(apt install python3-mako, or pip install mako)")
endif()

# The patch adds DekoCompiler::OutputDkshToMemory; skip it when the tree is
# already patched.
find_package(Git REQUIRED)
set(_uam_patch "${CMAKE_CURRENT_LIST_DIR}/patches/uam-library.patch")
execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check --reverse "${_uam_patch}"
    WORKING_DIRECTORY "${uam_SOURCE_DIR}" RESULT_VARIABLE _patched OUTPUT_QUIET ERROR_QUIET)
if (_patched)
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${_uam_patch}"
        WORKING_DIRECTORY "${uam_SOURCE_DIR}" RESULT_VARIABLE _patch_rc)
    if (_patch_rc)
        message(FATAL_ERROR "cannot apply ${_uam_patch} to ${uam_SOURCE_DIR}")
    endif()
endif()

# Sources are the quoted file names in the meson.build of each directory that
# exist on disk (generated files and the custom_target inputs do not).
set(_uam_dirs source mesa-imported/codegen mesa-imported/tgsi mesa-imported/util
    mesa-imported/cso_cache mesa-imported/glsl mesa-imported/glsl/glcpp
    mesa-imported/compiler mesa-imported/program mesa-imported/state_tracker
    mesa-imported/main)
set(_uam_sources)
foreach (_dir IN LISTS _uam_dirs)
    file(READ "${uam_SOURCE_DIR}/${_dir}/meson.build" _meson)
    string(REGEX MATCHALL "'[A-Za-z0-9_./-]+\\.(cpp|c)'" _names "${_meson}")
    foreach (_quoted IN LISTS _names)
        string(REPLACE "'" "" _name "${_quoted}")
        if (EXISTS "${uam_SOURCE_DIR}/${_dir}/${_name}")
            list(APPEND _uam_sources "${uam_SOURCE_DIR}/${_dir}/${_name}")
        endif()
    endforeach()
endforeach()
# main.cpp is the command-line front end.
list(REMOVE_ITEM _uam_sources "${uam_SOURCE_DIR}/source/main.cpp")

# Generated files mirror the source layout: the lexers include
# "glsl/glsl_parser.h" and "glsl/glcpp/glcpp-parse.h" relative to the root.
set(_gen "${CMAKE_CURRENT_BINARY_DIR}/uam-generated")
set(_glsl "${uam_SOURCE_DIR}/mesa-imported/glsl")
file(MAKE_DIRECTORY "${_gen}/glsl/glcpp")
function(_uam_generate output)
    if (NOT EXISTS "${_gen}/${output}")
        get_filename_component(_dir "${_gen}/${output}" DIRECTORY)
        execute_process(${ARGN} WORKING_DIRECTORY "${_dir}" RESULT_VARIABLE _rc)
        if (_rc)
            file(REMOVE "${_gen}/${output}")
            message(FATAL_ERROR "UAM: generating ${output} failed")
        endif()
    endif()
endfunction()
_uam_generate(glsl/glsl_parser.cpp COMMAND "${KISAK_BISON}" -o glsl_parser.cpp -p _mesa_glsl_
    --defines=glsl_parser.h "${_glsl}/glsl_parser.yy")
_uam_generate(glsl/glsl_lexer.cpp COMMAND "${KISAK_FLEX}" -o glsl_lexer.cpp "${_glsl}/glsl_lexer.ll")
_uam_generate(glsl/glcpp/glcpp-parse.c COMMAND "${KISAK_BISON}" -o glcpp-parse.c -p glcpp_parser_
    --defines=glcpp-parse.h "${_glsl}/glcpp/glcpp-parse.y")
_uam_generate(glsl/glcpp/glcpp-lex.c COMMAND "${KISAK_FLEX}" -o glcpp-lex.c
    "${_glsl}/glcpp/glcpp-lex.l")
foreach (_gen_pair enum:ir_expression_operation.h
                   constant:ir_expression_operation_constant.h
                   strings:ir_expression_operation_strings.h)
    string(REPLACE ":" ";" _gen_pair "${_gen_pair}")
    list(GET _gen_pair 0 _kind)
    list(GET _gen_pair 1 _header)
    _uam_generate(glsl/${_header}
        COMMAND "${KISAK_PYTHON}" "${_glsl}/ir_expression_operation.py" ${_kind}
        OUTPUT_FILE "${_gen}/glsl/${_header}")
endforeach()

add_library(uam STATIC ${_uam_sources}
    "${_gen}/glsl/glsl_parser.cpp" "${_gen}/glsl/glsl_lexer.cpp"
    "${_gen}/glsl/glcpp/glcpp-parse.c" "${_gen}/glsl/glcpp/glcpp-lex.c")
target_include_directories(uam PRIVATE
    "${_gen}" "${_gen}/glsl" "${_gen}/glsl/glcpp"
    "${uam_SOURCE_DIR}" "${uam_SOURCE_DIR}/mesa-imported"
    "${uam_SOURCE_DIR}/mesa-imported/glsl" "${uam_SOURCE_DIR}/mesa-imported/glsl/glcpp")
target_compile_definitions(uam PRIVATE
    "PACKAGE_STRING=\"uam 1.1.0\"" DESKTOP _USE_MATH_DEFINES _GNU_SOURCE
    HAVE_POSIX_MEMALIGN _FILE_OFFSET_BITS=64)
target_compile_options(uam PRIVATE -O3 -ffunction-sections
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-class-memaccess>
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-non-virtual-dtor>
    $<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>)
set_target_properties(uam PROPERTIES
    C_STANDARD 99 C_EXTENSIONS OFF CXX_STANDARD 11 CXX_EXTENSIONS OFF)
# Archives in one place, for the self-test Makefile and the host tools.
set_target_properties(uam mojoshader PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Header directories for consumers of the compiler interface.
set(KISAK_UAM_INCLUDE_DIRS "${uam_SOURCE_DIR}/source" "${uam_SOURCE_DIR}/mesa-imported")
