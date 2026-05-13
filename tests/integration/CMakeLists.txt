add_executable(integration
    fixture_runner.cpp
    test_main_eval.cpp
)

target_include_directories(integration PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(integration PRIVATE
    positron_sdk
    third_party::doctest
    positron::project_options
)

# Integration tests spin up the Electron fixture by absolute path; tagging the
# test as DISABLED by default keeps `ctest` from failing on dev machines
# without the fixture set up.
add_test(NAME integration COMMAND integration)
set_tests_properties(integration PROPERTIES DISABLED TRUE)
