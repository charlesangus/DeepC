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
#    DeepCBlink
    DeepCConstant
    DeepCCopyBBox
    DeepCRemoveChannels
    DeepCShuffle
    DeepCWorld
    )

foreach(PLUGIN_NAME ${PLUGINS})
    add_nuke_plugin(${PLUGIN_NAME} ${PLUGIN_NAME}.cpp)
endforeach()

#target_link_libraries(DeepCBlink PRIVATE Nuke::RIPFRAMEWORK)

# DeepCWrapper
add_library(DeepCWrapper OBJECT DeepCWrapper.cpp)
target_link_libraries(DeepCWrapper PRIVATE Nuke::NDK)

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
    DeepCSaturation
    )

foreach(PLUGIN_NAME ${PLUGINS_WRAPPED})
    add_nuke_plugin(${PLUGIN_NAME} ${PLUGIN_NAME}.cpp)
    target_sources(${PLUGIN_NAME} PRIVATE $<TARGET_OBJECTS:DeepCWrapper>)
endforeach()

# DeepCMWrapper
add_library(DeepCMWrapper OBJECT DeepCMWrapper.cpp)
target_link_libraries(DeepCMWrapper PRIVATE Nuke::NDK)
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

