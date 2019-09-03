cmake_minimum_required(VERSION 3.2)

project(DeepC)


set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()


find_package(Nuke REQUIRED)

if (NUKE_VERSION_MAJOR GREATER 10)
    set(CMAKE_CXX_STANDARD 11)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# NOTE: Compile options are currently Linux only
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

add_compile_options(
    -DUSE_GLEW -fPIC -msse -msse2 -msse3 -mssse3 -msse4 -msse4.1 -msse4.2 -mavx
)

set(FASTNOISE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/FastNoise)
add_library(FastNoise OBJECT
    ${FASTNOISE_DIR}/FastNoise.cpp)

add_subdirectory(src)


install(FILES
    python/init.py python/menu.py
    DESTINATION .)

file(GLOB ICONS "icons/DeepC*.png")
install(FILES ${ICONS}
    DESTINATION icons)
