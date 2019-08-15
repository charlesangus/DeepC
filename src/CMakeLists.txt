include_directories(${NUKE_INCLUDE_DIRS})

set(PLUGINS
    DeepCAddChannels
    DeepCBlink
    DeepCGrade
    DeepCID
    DeepCPMatte
    DeepCPNoise
    DeepCRemoveChannels
    DeepCSaturation
    DeepCShuffle
    DeepCWorld)


foreach(PLUGIN_NAME ${PLUGINS})
    add_library(${PLUGIN_NAME} SHARED
        "${PLUGIN_NAME}.cpp")
    target_link_libraries(${PLUGIN_NAME}
        PRIVATE
        ${NUKE_DDIMAGE_LIBRARY})
endforeach()


set_target_properties(${PLUGINS}
    PROPERTIES
    PREFIX "")

install(TARGETS ${PLUGINS}
    DESTINATION .)


# Add DeepC wrappers to plugin libs as necessary
add_library(DeepCWrapper OBJECT
    DeepCWrapper.cpp)

foreach(PLUGIN_NAME DeepCID DeepCPMatte DeepCPNoise DeepCGrade DeepCSaturation)
    target_sources(${PLUGIN_NAME} PRIVATE
        $<TARGET_OBJECTS:DeepCWrapper>)
endforeach()

add_library(DeepCMWrapper OBJECT
    DeepCMWrapper.cpp)

foreach(PLUGIN_NAME DeepCID DeepCPMatte DeepCPNoise)
    target_sources(${PLUGIN_NAME} PRIVATE
        $<TARGET_OBJECTS:DeepCMWrapper>)
endforeach()


# Link FastNoise into DeepCPNoise
target_include_directories(DeepCPNoise
    PRIVATE
    ${FASTNOISE_DIR})
target_sources(DeepCPNoise PRIVATE
    $<TARGET_OBJECTS:FastNoise>)


# Link DeepCBlink with libRIPFramework
target_link_libraries(DeepCBlink
    PRIVATE
    RIPFramework)
