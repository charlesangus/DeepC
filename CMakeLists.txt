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
# Unit tests for the header-only math/kernel foundation (M1.P1.T4).
#
# Off by default so the normal plugin build is completely unaffected. The
# test target is deliberately independent of Nuke/DDImage: it only compiles
# tests/test_defocus_math.cpp (doctest + src/DeepCDefocusMath.h +
# src/DeepCDefocusKernel.h, both header-only and NDK-free), so it exists to
# let that math be tested without a Nuke SDK. Configuring this whole top-level
# CMakeLists.txt still requires Nuke_ROOT (find_package(Nuke REQUIRED) above
# is unconditional for the rest of the project), but this target itself never
# includes an NDK header or links DDImage -- see tests/test_defocus_math.cpp,
# which also builds standalone with plain `g++ -std=c++17`, no cmake/Nuke at
# all, as the no-NDK proof.
# ---------------------------------------------------------------------------
option(DEEPC_BUILD_TESTS "Build the doctest unit tests for the math/kernel foundation" OFF)
if (DEEPC_BUILD_TESTS)
    enable_testing()
    add_executable(test_defocus_math tests/test_defocus_math.cpp)
    target_compile_features(test_defocus_math PRIVATE cxx_std_17)
    add_test(NAME test_defocus_math COMMAND test_defocus_math)
endif()

# install directory
install(FILES
    python/init.py
    DESTINATION .)

file(GLOB ICONS "icons/DeepC*.png")
install(FILES ${ICONS} DESTINATION icons)

# DeepThinner icon (not prefixed "DeepC", so not matched by the glob above)
install(FILES icons/DeepThinner.png DESTINATION icons)
