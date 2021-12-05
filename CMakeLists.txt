cmake_minimum_required(VERSION 3.10 FATAL_ERROR)
project(DeepC)

find_package(Nuke REQUIRED)

set(FASTNOISE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/FastNoise)
add_library(FastNoise OBJECT ${FASTNOISE_DIR}/FastNoise.cpp)

# add sub directory
add_subdirectory(src)

# install directory
install(FILES
    python/init.py python/menu.py
    DESTINATION .)

file(GLOB ICONS "icons/DeepC*.png")
install(FILES ${ICONS} DESTINATION icons)
