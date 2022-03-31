cmake_minimum_required(VERSION 3.10 FATAL_ERROR)

project(DeepCDefocus)

set(CMAKE_MODULE_PATH "CMake;${CMAKE_MODULE_PATH}")
set(NUKE_PATH $ENV{NUKE_PATH})
find_package(Nuke REQUIRED)

find_library(DDIMAGE_LIBRARY NAME DDImage libDDImage PATHS ${NUKE_PATH})
if(NOT DDIMAGE_LIBRARY)
    message(FATAL_ERROR "DDImage library not found.")
endif()

add_library(DeepCDefocus MODULE src/DeepCDefocus.cpp ../shared/src/DeepCSpec.cpp src/DefocusKernels.cpp src/DeepCDefocusSpec.cpp src/CircleMetadataKernel.cpp)
#add_library(NukePlugins::DeepBlur ALIAS DeepBlur)
target_include_directories(DeepCDefocus PRIVATE ${NUKE_PATH}/include)
target_include_directories(DeepCDefocus PRIVATE ../shared/include)
target_include_directories(DeepCDefocus PRIVATE ./include)
target_link_libraries(DeepCDefocus PRIVATE ${DDIMAGE_LIBRARY})
target_compile_definitions(DeepCDefocus PRIVATE FN_EXAMPLE_PLUGIN)
set_target_properties(DeepCDefocus PROPERTIES PREFIX "")
if (APPLE)
    set_target_properties(DeepCDefocus PROPERTIES SUFFIX ".dylib")
endif()

