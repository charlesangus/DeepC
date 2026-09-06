cmake_minimum_required(VERSION 3.15 FATAL_ERROR)
project(DeepC)

# Default to Release for single-config generators only (multi-config ones
# ignore CMAKE_BUILD_TYPE), and only when the user has not chosen explicitly.
# Without this the project passes no -O flag at all.
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

cmake_policy(SET CMP0074 NEW)

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
find_package(Nuke REQUIRED)

message("Using Nuke ${NUKE_VERSION_MAJOR}.${NUKE_VERSION_MINOR}v${NUKE_VERSION_RELEASE}")

if (UNIX)
    add_compile_options(
        -DUSE_GLEW -fPIC -msse -msse2 -msse3 -mssse3 -msse4 -msse4.1 -msse4.2 -mavx
    )
endif()

find_package(OpenGL)
if (NOT OpenGL_FOUND)
    message(WARNING "Couldn't find OpenGL dev libraries, skipping DeepCPMatte Plugin")   
endif()

if (NUKE_VERSION_MAJOR VERSION_GREATER_EQUAL 15.0)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_GLIBCXX_USE_CXX11_ABI=1")
    set(CMAKE_CXX_STANDARD 17)
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_GLIBCXX_USE_CXX11_ABI=0")
    set(CMAKE_CXX_STANDARD 14)
endif()

set(FASTNOISE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/FastNoise)
add_library(FastNoise OBJECT ${FASTNOISE_DIR}/FastNoise.cpp)
if (WIN32)
    target_compile_definitions(FastNoise PRIVATE NOMINMAX _USE_MATH_DEFINES)
endif()

# add sub directory
add_subdirectory(src)

# Off by default, so the normal plugin build is unaffected. Neither target
# includes an NDK header or links DDImage: the scatter core is compiled in
# directly rather than linked from the plugin, so the tests do not require
# DeepCDefocus.so.
option(DEEPC_BUILD_TESTS "Build the doctest unit tests for the math/kernel foundation and the scatter core" OFF)
if (DEEPC_BUILD_TESTS)
    enable_testing()

    add_executable(test_defocus_math tests/test_defocus_math.cpp)
    target_compile_features(test_defocus_math PRIVATE cxx_std_17)
    add_test(NAME test_defocus_math COMMAND test_defocus_math)

    add_executable(test_defocus_scatter
        tests/test_defocus_scatter.cpp
        src/DeepCDefocusScatter.cpp)
    target_compile_features(test_defocus_scatter PRIVATE cxx_std_17)
    find_package(Threads REQUIRED)
    target_link_libraries(test_defocus_scatter PRIVATE Threads::Threads)
    add_test(NAME test_defocus_scatter COMMAND test_defocus_scatter)
endif()

# install directory
install(FILES
    python/init.py
    DESTINATION .)

file(GLOB ICONS "icons/DeepC*.png")
install(FILES ${ICONS} DESTINATION icons)

# DeepThinner icon (not prefixed "DeepC", so not matched by the glob above)
install(FILES icons/DeepThinner.png DESTINATION icons)
