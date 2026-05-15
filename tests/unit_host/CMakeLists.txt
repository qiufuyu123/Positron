# Unit tests for host-side modules. Sources are re-compiled here so the test
# binary doesn't drag in BlackBone or the wider SDK link surface.
add_executable(unit_host
    test_main.cpp
    test_wire.cpp
    test_pe_resolver.cpp
    test_process_enum.cpp
    ${CMAKE_SOURCE_DIR}/shared/wire/wire.cpp
    ${CMAKE_SOURCE_DIR}/payload/src/pe_resolver/pe_resolver.cpp
    ${CMAKE_SOURCE_DIR}/host/src/process_enum/process_enum.cpp
)

target_include_directories(unit_host PRIVATE
    ${CMAKE_SOURCE_DIR}/shared
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/payload/src
    ${CMAKE_SOURCE_DIR}/host/src
)

target_link_libraries(unit_host PRIVATE
    third_party::doctest
    third_party::nlohmann_json
    positron::project_options
)

add_test(NAME unit_host COMMAND unit_host)
