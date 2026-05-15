# ============================================================================
# BlackBone — vendored copy, builds via the upstream CMakeLists.txt.
# Upstream's CMakeLists doesn't enable ASM_MASM for 32-bit and doesn't link
# diaguids / Ole32, so we wrap it.
# ============================================================================
set(BLACKBONE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/Blackbone/src)

enable_language(ASM_MASM)

# Upstream CMakeLists creates `BlackBone` target by reading source lists. It
# already adds the AsmJit/Asm/PE/Process/Misc/etc. sources and tells MASM to
# build Syscall64.asm for 64-bit. Skip the Samples subdir — it pulls in
# WinHttp and assumes a UI build we don't need.
add_subdirectory(${BLACKBONE_ROOT}/BlackBone ${CMAKE_BINARY_DIR}/blackbone)

target_include_directories(BlackBone PUBLIC ${BLACKBONE_ROOT})

# Wire up the DIA SDK that ships with Visual Studio so PDBHelper.cpp can
# resolve `__uuidof(IDiaDataSource)` etc.
if(MSVC)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(DIA_LIB_SUBDIR "amd64")
    else()
        set(DIA_LIB_SUBDIR "")
    endif()
    # VS_INSTALL_DIR is set by the VS generator; fall back to $ENV{VSINSTALLDIR}
    # when generating outside a VS prompt.
    set(_dia_root "$ENV{VSINSTALLDIR}DIA SDK")
    if(NOT EXISTS "${_dia_root}/include/dia2.h")
        # Probe common locations + the compiler's own install dir.
        get_filename_component(_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        get_filename_component(_vs_root "${_cl_dir}/../../../../../../.." ABSOLUTE)
        set(_dia_probes
            "${_vs_root}/DIA SDK"
            "C:/Program Files/Microsoft Visual Studio/2022/Community/DIA SDK"
            "C:/Program Files/Microsoft Visual Studio/2022/Professional/DIA SDK"
            "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/DIA SDK"
            "C:/Program Files/Microsoft Visual Studio/2022/BuildTools/DIA SDK"
        )
        foreach(_p ${_dia_probes})
            if(EXISTS "${_p}/include/dia2.h")
                set(_dia_root "${_p}")
                break()
            endif()
        endforeach()
    endif()
    if(EXISTS "${_dia_root}/include/dia2.h")
        message(STATUS "BlackBone DIA SDK: ${_dia_root}")
        target_include_directories(BlackBone PUBLIC "${_dia_root}/include")
        target_link_directories(BlackBone PUBLIC "${_dia_root}/lib/${DIA_LIB_SUBDIR}")
        target_link_libraries(BlackBone PUBLIC diaguids Ole32)
    else()
        message(WARNING
            "DIA SDK not found. BlackBone will fail to link without diaguids.lib. "
            "Set VSINSTALLDIR or install the 'C++ profiling tools' VS workload.")
    endif()
endif()

# BlackBone has noisy warnings — keep level 3 (the upstream vcxproj uses /W4
# with /wd4100, which is roughly /W3 for our purposes) and silence the cases
# we know about.
target_compile_options(BlackBone PRIVATE /wd4100 /wd4244 /wd4267 /wd4324 /wd4456 /wd4458)

# BlackBone is a C++17 codebase; under C++20 strict mode (/permissive-) a
# couple of templates need explicit `typename` keywords. Keep it on C++17.
set_target_properties(BlackBone PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON      # MSVC default — wow64ext.cpp expects Windows.h
                           # to expose stdlib.h transitively, which needs the
                           # non-strict (/Ze) mode.
)

# Re-export so consumers can `target_link_libraries(... third_party::BlackBone)`.
add_library(third_party::BlackBone ALIAS BlackBone)

# ============================================================================
# minhook — static library compiled from the vendored sources. Plain C so it
# can be linked into any CRT flavor; we build with the dynamic CRT and pick
# up the static one via target_link_libraries when payload imports it.
# ============================================================================
set(MINHOOK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/minhook)
set(MINHOOK_SOURCES
    ${MINHOOK_ROOT}/src/buffer.c
    ${MINHOOK_ROOT}/src/hook.c
    ${MINHOOK_ROOT}/src/trampoline.c
)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    list(APPEND MINHOOK_SOURCES ${MINHOOK_ROOT}/src/hde/hde64.c)
else()
    list(APPEND MINHOOK_SOURCES ${MINHOOK_ROOT}/src/hde/hde32.c)
endif()

add_library(positron_minhook STATIC ${MINHOOK_SOURCES})
target_include_directories(positron_minhook PUBLIC ${MINHOOK_ROOT}/include)
# minhook is consumed by payload, which uses the static CRT. Make minhook
# match so the object files mix cleanly when linked together.
set_property(TARGET positron_minhook PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_library(third_party::minhook ALIAS positron_minhook)

# ============================================================================
# replxx — built as a static library from upstream sources. The vcxproj used
# to compile these into host.exe directly; pulling them into a lib is cleaner
# and lets us reuse the same compile flags in one place.
# ============================================================================
set(REPLXX_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/replxx)
set(REPLXX_SOURCES
    ${REPLXX_ROOT}/src/ConvertUTF.cpp
    ${REPLXX_ROOT}/src/wcwidth.cpp
    ${REPLXX_ROOT}/src/conversion.cxx
    ${REPLXX_ROOT}/src/escape.cxx
    ${REPLXX_ROOT}/src/history.cxx
    ${REPLXX_ROOT}/src/prompt.cxx
    ${REPLXX_ROOT}/src/replxx.cxx
    ${REPLXX_ROOT}/src/replxx_impl.cxx
    ${REPLXX_ROOT}/src/terminal.cxx
    ${REPLXX_ROOT}/src/util.cxx
    ${REPLXX_ROOT}/src/windows.cxx
)
add_library(positron_replxx STATIC ${REPLXX_SOURCES})
target_include_directories(positron_replxx PUBLIC ${REPLXX_ROOT}/include)
target_include_directories(positron_replxx PRIVATE ${REPLXX_ROOT}/src)
target_compile_definitions(positron_replxx PUBLIC REPLXX_STATIC)
# Upstream is noisy under /W3; turn off SDL extras and a few warning classes.
target_compile_options(positron_replxx PRIVATE /W1 /wd4244 /wd4267)
add_library(third_party::replxx ALIAS positron_replxx)
