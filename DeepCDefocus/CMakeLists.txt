cmake_minimum_required(VERSION 3.10 FATAL_ERROR)
project(DeepCDefocus)

cmake_policy(SET CMP0074 NEW)

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
find_package(Nuke REQUIRED)

message("Using Nuke ${NUKE_VERSION_MAJOR}.${NUKE_VERSION_MINOR}v${NUKE_VERSION_RELEASE}")

set(CMAKE_CXX_STANDARD 14)

add_library(DeepCDefocus MODULE src/DeepCDefocus.cpp ../shared/src/DeepCSpec.cpp src/DefocusKernels.cpp src/DeepCDefocusSpec.cpp src/CircleMetadataKernel.cpp)
target_include_directories(DeepCDefocus PRIVATE ${NUKE_INCLUDE_DIRS})
target_include_directories(DeepCDefocus PRIVATE ../shared/include)
target_include_directories(DeepCDefocus PRIVATE ./include)
target_link_libraries(DeepCDefocus PRIVATE ${NUKE_DDIMAGE_LIBRARY})
target_compile_definitions(DeepCDefocus PRIVATE FN_EXAMPLE_PLUGIN)
set_target_properties(DeepCDefocus PROPERTIES PREFIX "")
if (APPLE)
    set_target_properties(DeepCDefocus PROPERTIES SUFFIX ".dylib")
endif()

