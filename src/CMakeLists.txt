# NON WRAPPED PLUGINS
set(PLUGINS
    DeepCAddChannels
    DeepCAdjustBBox
    DeepCBlink
    DeepCConstant
    DeepCCopyBBox
    DeepCKeymix
    DeepCRemoveChannels
    DeepCShuffle
    DeepCWorld
    )

# DeepCWrapper 
set(PLUGINS_WRAPPED
DeepCAdd
DeepCClamp
DeepCColorLookup
DeepCGamma
DeepCGrade
DeepCHueShift
DeepCInvert
DeepCMatrix
DeepCMultiply
DeepCPosterize
DeepCSaturation
)

# DeepCMWrapper
set(PLUGINS_MWRAPPED
    DeepCID
    DeepCPNoise
    )

# link DeepCPMatte with OpenGL
if (OpenGL_FOUND)
    list(APPEND PLUGINS_MWRAPPED DeepCPMatte)
endif()

# DeepC Python Menu
set(DRAW_NODES DeepCConstant DeepCID DeepCPMatte DeepCPNoise)
set(CHANNEL_NODES DeepCAddChannels DeepCRemoveChannels DeepCShuffle)
set(COLOR_NODES DeepCAdd DeepCClamp DeepCColorLookup DeepCGamma DeepCGrade
DeepCHueShift DeepCInvert DeepCMatrix DeepCMultiply DeepCPosterize DeepCSaturation)
set(3D_NODES DeepCWorld)
set(MERGE_NODES DeepCKeymix)
set(Util_NODES DeepCAdjustBBox DeepCCopyBBox)

# add nuke plugin linked to ddimage lib
function(add_nuke_plugin PLUGIN_NAME)
    add_library(${PLUGIN_NAME} MODULE ${ARGN})
    add_library(NukePlugins::${PLUGIN_NAME} ALIAS ${PLUGIN_NAME})
    target_link_libraries(${PLUGIN_NAME} PRIVATE ${NUKE_DDIMAGE_LIBRARY})
    set_target_properties(${PLUGIN_NAME} PROPERTIES PREFIX "")
    if (APPLE)
        set_target_properties(${PLUGIN_NAME} PROPERTIES SUFFIX ".dylib")
    endif()
endfunction()

# include directories
include_directories(${NUKE_INCLUDE_DIRS})

# WRAPPER LIBRARIES

# DeepCMWrapper
add_library(DeepCWrapper OBJECT DeepCWrapper.cpp)
target_link_libraries(DeepCWrapper PRIVATE ${NUKE_DDIMAGE_LIBRARY})

# DeepCMWrapper
add_library(DeepCMWrapper OBJECT DeepCMWrapper.cpp)
target_link_libraries(DeepCMWrapper PRIVATE ${NUKE_DDIMAGE_LIBRARY})
target_sources(DeepCMWrapper PRIVATE $<TARGET_OBJECTS:DeepCWrapper>)

# add configuration 
foreach(PLUGIN_NAME ${PLUGINS})
    add_nuke_plugin(${PLUGIN_NAME} ${PLUGIN_NAME}.cpp)
endforeach()

foreach(PLUGIN_NAME ${PLUGINS_WRAPPED})
    add_nuke_plugin(${PLUGIN_NAME} ${PLUGIN_NAME}.cpp)
    target_sources(${PLUGIN_NAME} PRIVATE $<TARGET_OBJECTS:DeepCWrapper>)
endforeach()

foreach(PLUGIN_NAME ${PLUGINS_MWRAPPED})
    add_nuke_plugin(${PLUGIN_NAME} ${PLUGIN_NAME}.cpp)
    target_sources(${PLUGIN_NAME} PRIVATE $<TARGET_OBJECTS:DeepCWrapper>)
    target_sources(${PLUGIN_NAME} PRIVATE $<TARGET_OBJECTS:DeepCMWrapper>)
endforeach()

# Link FastNoise into DeepCPNoise
target_include_directories(DeepCPNoise PRIVATE ${FASTNOISE_DIR})
target_sources(DeepCPNoise PRIVATE $<TARGET_OBJECTS:FastNoise>)

# link DeepCBlink with libRIPFramework
target_link_libraries(DeepCBlink PRIVATE ${NUKE_RIPFRAMEWORK_LIBRARY})

# link DeepCPMatte with OpenGL
if (OpenGL_FOUND)
    target_link_libraries(DeepCPMatte PRIVATE OpenGL::GL)
endif()

# create menu file 
string(REPLACE ";" "\", \"" DRAW_NODES "\"${DRAW_NODES}\"")
string(REPLACE ";" "\", \"" CHANNEL_NODES "\"${CHANNEL_NODES}\"")
string(REPLACE ";" "\", \"" COLOR_NODES "\"${COLOR_NODES}\"")
string(REPLACE ";" "\", \"" 3D_NODES "\"${3D_NODES}\"")
string(REPLACE ";" "\", \"" MERGE_NODES "\"${MERGE_NODES}\"")
string(REPLACE ";" "\", \"" Util_NODES "\"${Util_NODES}\"")
configure_file(../python/menu.py.in menu.py)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/menu.py" DESTINATION .)

# install files
install(TARGETS 
        ${PLUGINS} 
        ${PLUGINS_WRAPPED} 
        ${PLUGINS_MWRAPPED} 
        DESTINATION .)
