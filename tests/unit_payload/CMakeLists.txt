add_executable(unit_payload
    test_main.cpp
    test_ring_buffer.cpp
)

target_include_directories(unit_payload PRIVATE
    ${CMAKE_SOURCE_DIR}/payload/src
    ${CMAKE_SOURCE_DIR}/shared
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(unit_payload PRIVATE third_party::doctest positron::project_options)

add_test(NAME unit_payload COMMAND unit_payload)
