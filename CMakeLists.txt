cmake_minimum_required(VERSION 3.4)

# Pick up share/cmake/ modules
# TODO: Append instead of overriding
set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/share/cmake)

# If CMAKE_INSTALL_EXEC_PREFIX is not specified, install binaries
# directly into the regular install prefix
if(NOT CMAKE_INSTALL_EXEC_PREFIX)
    message("Exec prefix not specified, defaulting to ${CMAKE_INSTALL_PREFIX}")
    set(CMAKE_INSTALL_EXEC_PREFIX ${CMAKE_INSTALL_PREFIX})
endif()

# Locate Nuke or error
find_package(Nuke REQUIRED)
message("Build Nuke plugins against ${Nuke_LIBRARY_DIR}")

if(Nuke_DDImageVersion VERSION_LESS 11)
    # No C++11 stuff until compiler update in Nuke 11
else()
    set (CMAKE_CXX_STANDARD 11)
endif()

if(APPLE)
    # Flag required on OS X to allow Nuke API symbols to be found at runtime
    set(DEEPC_LINK_FLAGS "-undefined dynamic_lookup")
else()
    set(DEEPC_LINK_FLAGS "")
endif()


# Include Nuke headers
include_directories(
    ${Nuke_INCLUDE_DIR}
    FastNoise
)

# Add plugins
add_library(DeepCAddChannels SHARED
    src/DeepCAddChannels.cpp
)
add_library(DeepCBlink SHARED
    src/DeepCBlink.cpp
)
add_library(DeepCGrade SHARED
    src/DeepCGrade.cpp
    src/DeepCWrapper.cpp
)
add_library(DeepCID SHARED
    src/DeepCID.cpp
)
add_library(DeepCPMatte SHARED
    src/DeepCPMatte.cpp
)
add_library(DeepCPNoise SHARED
   src/DeepCPNoise.cpp
)
add_library(DeepCRemoveChannels SHARED
    src/DeepCRemoveChannels.cpp
)
add_library(DeepCSaturation SHARED
    src/DeepCSaturation.cpp
    src/DeepCWrapper.cpp
)
add_library(DeepCShuffle SHARED
    src/DeepCShuffle.cpp
)
add_library(DeepCWorld SHARED
    src/DeepCWorld.cpp
)


set_target_properties(
    DeepCAddChannels
    DeepCBlink
    DeepCGrade
    DeepCID
    DeepCPMatte
    DeepCPNoise
    DeepCRemoveChannels
    DeepCSaturation
    DeepCShuffle
    DeepCWorld

    PROPERTIES
    PREFIX "" # No lib prefix
    LINK_FLAGS "${DEEPC_LINK_FLAGS}"
)

# Once built, copy the library into install location (e.g "executable-prefix/lib/nuke/6.2")
install(TARGETS
    DeepCAddChannels
    DeepCBlink
    DeepCGrade
    DeepCID
    DeepCPMatte
    DeepCPNoise
    DeepCRemoveChannels
    DeepCSaturation
    DeepCShuffle
    DeepCWorld
    DESTINATION ${CMAKE_INSTALL_EXEC_PREFIX}/plugins)
