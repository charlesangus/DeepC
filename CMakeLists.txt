cmake_minimum_required(VERSION 3.10 FATAL_ERROR)
project(DeepC)

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
find_package(Nuke REQUIRED)

set(FASTNOISE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/FastNoise)
add_library(FastNoise OBJECT ${FASTNOISE_DIR}/FastNoise.cpp)

if (UNIX)
    add_compile_options(
        -DUSE_GLEW -fPIC -msse -msse2 -msse3 -mssse3 -msse4 -msse4.1 -msse4.2 -mavx
    )
endif()

# add sub directory
add_subdirectory(src)

# install directory
install(FILES
    python/init.py python/menu.py
    DESTINATION .)

file(GLOB ICONS "icons/DeepC*.png")
install(FILES ${ICONS} DESTINATION icons)
