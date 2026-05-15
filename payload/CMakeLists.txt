# payload.dll — the injected library. Uses the static CRT so it can survive
# the manual-mapping / self-unmap sequence without dragging in the host's
# CRT state.
add_library(payload SHARED
    dllmain.cpp
    pch.cpp
    src/bootstrap/init.cpp
    src/logging/log.cpp
    src/pe_resolver/pe_resolver.cpp
    src/ipc_server/ipc_server.cpp
    src/hook_engine/hook_engine.cpp
    src/v8_bridge/v8_bridge.cpp
    src/teardown/teardown.cpp
    ${POSITRON_WIRE_SOURCE}
)

target_include_directories(payload PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/shared
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann/single_include
)

target_compile_definitions(payload PRIVATE
    POSITRON_EXPORTS
    _USRDLL
)

# Static CRT — see comment above and dllmain.cpp.
set_property(TARGET payload PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Precompiled header: matches the vcxproj layout (pch.h built from pch.cpp).
# Files that intentionally bypass the PCH need it disabled — see below.
target_precompile_headers(payload PRIVATE pch.h)

# These translation units don't use the PCH in the existing build either —
# pe_resolver pulls in NTSTATUS headers that conflict with framework.h, the
# wire and minhook sources are plain C / freestanding C++.
set_source_files_properties(
    src/pe_resolver/pe_resolver.cpp
    ${POSITRON_WIRE_SOURCE}
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON
)

target_link_libraries(payload PRIVATE third_party::minhook positron::project_options)

# WindowsApp-style imports the DLL relies on. dllmain uses CreateEventW /
# CreateThread which are in kernel32 (default), but v8_bridge / ipc_server
# end up touching Winsock + dbghelp transitively via project headers.
target_link_libraries(payload PRIVATE Ws2_32)

# DLL needs a stable PDB name independent of intermediate dir for the
# self-unmap teardown to locate it during debugging.
set_target_properties(payload PROPERTIES OUTPUT_NAME "payload")
