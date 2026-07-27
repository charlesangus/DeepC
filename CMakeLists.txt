cmake_minimum_required(VERSION 3.15 FATAL_ERROR)
project(DeepC)

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

# ---------------------------------------------------------------------------
# Unit tests for the header-only math/kernel foundation (M1.P1.T4) and for the
# POD scatter core (M1.P3.T4).
#
# Off by default so the normal plugin build is completely unaffected. Both
# test targets are deliberately independent of Nuke/DDImage:
#
#   test_defocus_math    — doctest + src/DeepCDefocusMath.h +
#                          src/DeepCDefocusKernel.h, both header-only.
#   test_defocus_scatter — the same, plus src/DeepCDefocusScatter.cpp, which is
#                          the M3 CUDA seam and by contract carries no NDK type
#                          either (it is compiled into this target directly
#                          rather than linked from the plugin, so the tests do
#                          not depend on DeepCDefocus.so having been built).
#
# Configuring this whole top-level CMakeLists.txt still requires Nuke_ROOT
# (find_package(Nuke REQUIRED) above is unconditional for the rest of the
# project), but neither target includes an NDK header or links DDImage -- both
# test files also build standalone with plain `g++ -std=c++17`, no cmake and no
# Nuke at all, as the no-NDK proof.
# ---------------------------------------------------------------------------
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
