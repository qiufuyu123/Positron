add_executable(host
    src/main.cpp
    src/repl/repl.cpp
)

target_include_directories(host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(host PRIVATE
    positron_sdk
    third_party::cli11
    third_party::replxx
    positron::project_options
)

# host.exe is the friendly name shipped in build/<plat>/<cfg>/host.exe.
set_target_properties(host PROPERTIES OUTPUT_NAME "host")

# ------------------------------------------------------------------
# bootstrap.js minification (matches the MinifyBootstrapJs MSBuild target).
# The SDK reads the minified copy at attach time from the same directory as
# host.exe / the consuming binary.
# ------------------------------------------------------------------
set(_bootstrap_src ${CMAKE_SOURCE_DIR}/sdk/bootstrap.js)

# Resolve npx at configure time so the build doesn't depend on PATH being
# inherited by Visual Studio's build server. The shim on Windows is npx.cmd,
# which cmake's find_program picks up as long as Node is installed.
find_program(NPX_EXECUTABLE NAMES npx.cmd npx)
if(NPX_EXECUTABLE)
    add_custom_command(TARGET host POST_BUILD
        COMMAND ${NPX_EXECUTABLE} terser "${_bootstrap_src}"
                --compress --mangle
                --output "$<TARGET_FILE_DIR:host>/bootstrap.js"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Minify bootstrap.js -> host.exe directory"
        VERBATIM
    )
else()
    message(WARNING
        "npx not found on PATH. bootstrap.js will not be minified into the "
        "build dir. Run `npm install` and ensure node + npx are on PATH, then "
        "rebuild the host target.")
endif()
