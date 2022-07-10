cmake_minimum_required(VERSION 3.15 FATAL_ERROR)
project(DeepCBlur)

cmake_policy(SET CMP0074 NEW)

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
find_package(Nuke REQUIRED)

message("Using Nuke ${NUKE_VERSION_MAJOR}.${NUKE_VERSION_MINOR}v${NUKE_VERSION_RELEASE}")

set(CMAKE_CXX_STANDARD 14)

add_library(DeepCBlur MODULE src/DeepCBlur.cpp ../shared/src/DeepCOpTree.cpp ../shared/src/DeepCSpec.cpp src/BlurKernels.cpp src/DeepCBlurSpec.cpp)
target_include_directories(DeepCBlur PRIVATE ${NUKE_INCLUDE_DIRS})
target_include_directories(DeepCBlur PRIVATE ../shared/include)
target_include_directories(DeepCBlur PRIVATE ./include)
target_link_libraries(DeepCBlur PRIVATE ${NUKE_DDIMAGE_LIBRARY})
target_compile_definitions(DeepCBlur PRIVATE FN_EXAMPLE_PLUGIN)
set_target_properties(DeepCBlur PROPERTIES PREFIX "")
if (APPLE)
    set_target_properties(DeepCBlur PROPERTIES SUFFIX ".dylib")
endif()

