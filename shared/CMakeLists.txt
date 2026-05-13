# Shared wire-format helper compiled once into a static lib so payload (static
# CRT) and the SDK (dynamic CRT) each pick it up with their own runtime.
add_library(positron_shared INTERFACE)
target_include_directories(positron_shared INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# Expose the wire source path so payload + sdk can compile it under their own
# CRT settings, exactly like the vcxproj files do today.
set(POSITRON_WIRE_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/wire/wire.cpp CACHE INTERNAL "")
