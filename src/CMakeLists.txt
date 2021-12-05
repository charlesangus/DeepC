function(add_nuke_plugin PLUGIN_NAME)
    add_library(${PLUGIN_NAME} MODULE ${ARGN})
    add_library(NukePlugins::${PLUGIN_NAME} ALIAS ${PLUGIN_NAME})
    target_link_libraries(${PLUGIN_NAME} PRIVATE Nuke::NDK)
    target_compile_definitions(${PLUGIN_NAME} PRIVATE FN_EXAMPLE_PLUGIN)
    set_target_properties(${PLUGIN_NAME} PROPERTIES PREFIX "")
    if (APPLE)
        set_target_properties(${PLUGIN_NAME} PROPERTIES SUFFIX ".dylib")
    endif()
endfunction()

set(PLUGINS
    DeepCAddChannels
    DeepCAdjustBBox
#   DeepCBlink
    DeepCConstant
    DeepCCopyBBox
    DeepCRemoveChannels
    DeepCShuffle
    DeepCWorld
    )

foreach(PLUGIN_NAME ${PLUGINS})
    add_nuke_plugin(${PLUGIN_NAME} ${PLUGIN_NAME}.cpp)
endforeach()

# DeepCWrapper
add_nuke_plugin(DeepCWrapper DeepCWrapper.cpp)

set(PLUGINS_WRAPPED
    DeepCAdd
    DeepCClamp
    DeepCColorLookup
    DeepCGamma
    DeepCGrade
    DeepCInvert
    DeepCMultiply
    )

foreach(PLUGIN_NAME ${PLUGINS_WRAPPED})
    add_nuke_plugin(${PLUGIN_NAME} ${PLUGIN_NAME}.cpp)
    target_sources(${PLUGIN_NAME} PRIVATE $<TARGET_OBJECTS:DeepCWrapper>)
endforeach()

# DeepCWrapper
add_nuke_plugin(DeepCMWrapper DeepCMWrapper.cpp)
target_sources(DeepCMWrapper PRIVATE $<TARGET_OBJECTS:DeepCWrapper>)

set(PLUGINS_MWRAPPED
    DeepCID
    DeepCPMatte
    DeepCPNoise
    )

foreach(PLUGIN_NAME ${PLUGINS_MWRAPPED})
    add_nuke_plugin(${PLUGIN_NAME} ${PLUGIN_NAME}.cpp)
    target_sources(${PLUGIN_NAME} PRIVATE $<TARGET_OBJECTS:DeepCWrapper>)
    target_sources(${PLUGIN_NAME} PRIVATE $<TARGET_OBJECTS:DeepCMWrapper>)
endforeach()

# Link FastNoise into DeepCPNoise
target_include_directories(DeepCPNoise PRIVATE ${FASTNOISE_DIR})
target_sources(DeepCPNoise PRIVATE $<TARGET_OBJECTS:FastNoise>)

# install files
install(TARGETS ${PLUGINS} ${PLUGINS_WRAPPED} ${PLUGINS_MWRAPPED} DESTINATION .)



#include_directories(${NUKE_INCLUDE_DIRS})

#set(PLUGINS
#    DeepCAdd
#    DeepCAddChannels
#    DeepCAdjustBBox
#    DeepCBlink
#    DeepCClamp
#    DeepCColorLookup
#    DeepCConstant
#    DeepCCopyBBox
#    DeepCGamma
#    DeepCGrade
#    DeepCID
#    DeepCInvert
#    DeepCPMatte
#    DeepCMultiply
#    DeepCPNoise
#    DeepCRemoveChannels
#    DeepCSaturation
#    DeepCShuffle
#    DeepCWorld
#    )


#foreach(PLUGIN_NAME ${PLUGINS})
#    add_library(${PLUGIN_NAME} SHARED
#        "${PLUGIN_NAME}.cpp")
#    target_link_libraries(${PLUGIN_NAME} PRIVATE Nuke::NDK)
#    target_compile_definitions(${PLUGIN_NAME} PRIVATE FN_EXAMPLE_PLUGIN)
#    set_target_properties(${PLUGIN_NAME} PROPERTIES PREFIX "")
    #target_link_libraries(${PLUGIN_NAME}
    #    PRIVATE
    #    ${NUKE_DDIMAGE_LIBRARY})
#endforeach()


#set_target_properties(${PLUGINS}
#    PROPERTIES
#    PREFIX "")

#install(TARGETS ${PLUGINS}
#    DESTINATION .)


# Add DeepC wrappers to plugin libs as necessary
#add_library(DeepCWrapper OBJECT
#    DeepCWrapper.cpp)
#target_link_libraries(DeepCWrapper PRIVATE Nuke::NDK)
#target_compile_definitions(DeepCWrapper PRIVATE FN_EXAMPLE_PLUGIN)
#set_target_properties(DeepCWrapper PROPERTIES PREFIX "")

#foreach(PLUGIN_NAME DeepCAdd DeepCClamp DeepCColorLookup DeepCID DeepCPMatte DeepCPNoise DeepCGamma DeepCGrade DeepCInvert DeepCMultiply DeepCSaturation)
#    target_sources(${PLUGIN_NAME} PRIVATE
#        $<TARGET_OBJECTS:DeepCWrapper>)
#endforeach()

#add_library(DeepCMWrapper OBJECT
#    DeepCMWrapper.cpp)
#target_link_libraries(DeepCMWrapper PRIVATE Nuke::NDK)
#target_compile_definitions(DeepCMWrapper PRIVATE FN_EXAMPLE_PLUGIN)
#set_target_properties(DeepCMWrapper PROPERTIES PREFIX "")

#foreach(PLUGIN_NAME DeepCID DeepCPMatte DeepCPNoise)
#    target_sources(${PLUGIN_NAME} PRIVATE
#        $<TARGET_OBJECTS:DeepCMWrapper>)
#endforeach()


# Link FastNoise into DeepCPNoise
#target_include_directories(DeepCPNoise
#    PRIVATE
#    ${FASTNOISE_DIR})
#target_sources(DeepCPNoise PRIVATE
#    $<TARGET_OBJECTS:FastNoise>)