# positron_sdk: static library exposing the public C++ SDK and pulling in the
# host-side modules that injectors / process enumeration / pipe client rely on.
# Built with the default dynamic CRT so consumers (host, examples, tests) can
# link without runtime mismatches.
add_library(positron_sdk STATIC
    src/process.cpp
    src/session.cpp
    src/bootstrap_v2.cpp
    # Reused host modules — kept here so the SDK is self-contained for external
    # consumers and the host exe just adds main + REPL on top.
    ../host/src/injector/injector.cpp
    ../host/src/pipe_client/pipe_client.cpp
    ../host/src/process_enum/process_enum.cpp
    # Shared wire framing.
    ${POSITRON_WIRE_SOURCE}
)

target_include_directories(positron_sdk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(positron_sdk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/host/src
    ${CMAKE_SOURCE_DIR}/shared
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/Blackbone/src
)

target_link_libraries(positron_sdk
    PUBLIC
        third_party::nlohmann_json
    PRIVATE
        third_party::BlackBone
        positron::project_options
)

# Public link deps so callers don't need to remember every Win32 import.
target_link_libraries(positron_sdk PUBLIC dbghelp Ws2_32)
