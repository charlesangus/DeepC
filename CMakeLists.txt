cmake_minimum_required(VERSION 3.10 FATAL_ERROR)
project(DeepC)

find_package(Nuke REQUIRED)

# add RIPFRAMEWORK link library
add_library(Nuke::RIPFRAMEWORK SHARED IMPORTED)

if (UNIX)
    set_target_properties(Nuke::RIPFRAMEWORK
        PROPERTIES
            IMPORTED_LOCATION ${NUKE_ROOT}/libRIPFramework.so
            INTERFACE_INCLUDE_DIRECTORIES ${NUKE_ROOT}/include
            INTERFACE_LINK_LIBRARIES ${NUKE_ROOT}/libRIPFramework.so
            INTERFACE_COMPILE_FEATURES cxx_std_14
    )
else()
    set_target_properties(Nuke::RIPFRAMEWORK
        PROPERTIES
            IMPORTED_LOCATION ${NUKE_ROOT}/RIPFramework.dll
            IMPORTED_IMPLIB ${NUKE_ROOT}/RIPFramework.lib
            INTERFACE_INCLUDE_DIRECTORIES ${NUKE_ROOT}/include
            INTERFACE_COMPILE_FEATURES cxx_std_14
    )
endif()

set(FASTNOISE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/FastNoise)
add_library(FastNoise OBJECT ${FASTNOISE_DIR}/FastNoise.cpp)

if (UNIX)
    add_compile_options(
        -DUSE_GLEW -fPIC -msse -msse2 -msse3 -mssse3 -msse4 -msse4.1 -msse4.2 -mavx
    )
endif(

# add sub directory
add_subdirectory(src)

# install directory
install(FILES
    python/init.py python/menu.py
    DESTINATION .)

file(GLOB ICONS "icons/DeepC*.png")
install(FILES ${ICONS} DESTINATION icons)
