cmake_minimum_required(VERSION 3.10 FATAL_ERROR)

project(DeepCBlur)

set(CMAKE_MODULE_PATH "CMake;${CMAKE_MODULE_PATH}")
set(NUKE_PATH $ENV{NUKE_PATH})
find_package(Nuke REQUIRED)

find_library(DDIMAGE_LIBRARY NAME DDImage libDDImage PATHS ${NUKE_PATH})
if(NOT DDIMAGE_LIBRARY)
    message(FATAL_ERROR "DDImage library not found.")
endif()

add_library(DeepCBlur MODULE src/DeepCBlur.cpp ../shared/src/DeepCOpTree.cpp ../shared/src/DeepCSpec.cpp src/BlurKernels.cpp src/BlurStrategy.cpp src/DeepCBlurSpec.cpp)
#add_library(NukePlugins::DeepBlur ALIAS DeepBlur)
target_include_directories(DeepCBlur PRIVATE ${NUKE_PATH}/include)
target_include_directories(DeepCBlur PRIVATE ../shared/include)
target_include_directories(DeepCBlur PRIVATE ./include)
target_link_libraries(DeepCBlur PRIVATE ${DDIMAGE_LIBRARY})
target_compile_definitions(DeepCBlur PRIVATE FN_EXAMPLE_PLUGIN)
set_target_properties(DeepCBlur PROPERTIES PREFIX "")
if (APPLE)
    set_target_properties(DeepCBlur PROPERTIES SUFFIX ".dylib")
endif()

