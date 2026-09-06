# NON WRAPPED PLUGINS
set(PLUGINS
    DeepCAddChannels
    DeepCAdjustBBox
    DeepCConstant
    DeepCCopyBBox
    DeepCKeymix
    DeepCRemoveChannels
    DeepCShuffle
    DeepCShuffle2
    DeepCWorld
    DeepCBlur
    DeepCBlur2
    DeepThinner
    DeepCDepthBlur
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
set(CHANNEL_NODES DeepCAddChannels DeepCRemoveChannels DeepCShuffle2)
set(COLOR_NODES DeepCAdd DeepCClamp DeepCColorLookup DeepCGamma DeepCGrade
DeepCHueShift DeepCInvert DeepCMatrix DeepCMultiply DeepCPosterize DeepCSaturation)
set(3D_NODES DeepCWorld)
set(MERGE_NODES DeepCKeymix)
set(Util_NODES DeepCAdjustBBox DeepCCopyBBox)
set(FILTER_NODES DeepCBlur DeepCBlur2 DeepThinner DeepCDepthBlur)

# DeepCDefocus is deliberately Linux-only (see DeepCDefocus.cpp); keep it out
# of the Windows docker build so every other node keeps building unchanged.
if (UNIX)
    list(APPEND PLUGINS DeepCDefocus)
    list(APPEND FILTER_NODES DeepCDefocus)
endif()

# add nuke plugin linked to ddimage lib
function(add_nuke_plugin PLUGIN_NAME)
    add_library(${PLUGIN_NAME} MODULE ${ARGN})
    add_library(NukePlugins::${PLUGIN_NAME} ALIAS ${PLUGIN_NAME})
    target_link_libraries(${PLUGIN_NAME} PRIVATE ${NUKE_DDIMAGE_LIBRARY})
    set_target_properties(${PLUGIN_NAME} PROPERTIES PREFIX "")
    if (APPLE)
        set_target_properties(${PLUGIN_NAME} PROPERTIES SUFFIX ".dylib")
    endif()
    if (WIN32)
        target_compile_definitions(${PLUGIN_NAME} PRIVATE NOMINMAX _USE_MATH_DEFINES)
    endif()
endfunction()

# include directories
include_directories(${NUKE_INCLUDE_DIRS})

# Qt6 — required for ShuffleMatrixKnob embedded Qt widget (Phase 03)
# Qt 6.5.3 matches the version shipped with Nuke 16.x.
# Windows cross-compilation uses MSVC x64 binaries; Linux uses gcc_64.
if (WIN32)
    list(APPEND CMAKE_PREFIX_PATH "/opt/Qt/6.5.3/msvc2019_64")
else()
    list(APPEND CMAKE_PREFIX_PATH "/opt/Qt/6.5.3/gcc_64")
endif()
find_package(Qt6 COMPONENTS Core Gui Widgets)
if (Qt6_FOUND)
  set(CMAKE_AUTOMOC ON)
endif()

# WRAPPER LIBRARIES

# DeepCMWrapper
add_library(DeepCWrapper OBJECT DeepCWrapper.cpp)
target_link_libraries(DeepCWrapper PRIVATE ${NUKE_DDIMAGE_LIBRARY})
if (WIN32)
    target_compile_definitions(DeepCWrapper PRIVATE NOMINMAX _USE_MATH_DEFINES)
endif()

# DeepCMWrapper
add_library(DeepCMWrapper OBJECT DeepCMWrapper.cpp)
target_link_libraries(DeepCMWrapper PRIVATE ${NUKE_DDIMAGE_LIBRARY})
if (WIN32)
    target_compile_definitions(DeepCMWrapper PRIVATE NOMINMAX _USE_MATH_DEFINES)
endif()
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

# link DeepCPMatte with OpenGL
if (OpenGL_FOUND)
    target_link_libraries(DeepCPMatte PRIVATE OpenGL::GL)
endif()

# Link the CPU scatter kernel into DeepCDefocus (Linux-only, see above)
if (UNIX)
    target_sources(DeepCDefocus PRIVATE DeepCDefocusScatter.cpp)

    # AVX2/FMA for the scatter's row-span multiply-adds, on THIS target only —
    # the project-wide floor above stays -mavx for every other node.
    #
    # -ffp-contract=off is not optional next to -mfma: GCC contracts a*b+c into
    # a single fused op by default, which changes the shipped pixels against the
    # unfused arithmetic this node's parity gates are stated in.  Priced rather
    # than assumed (see the option below): letting the scatter TU contract puts
    # 205 vfmadd into the .so and buys 18.10 s against 18.28 s median on a
    # 2048x1080 20-samples-per-pixel frame -- inside this box's run-to-run
    # noise -- while moving three holdout parity readings off exact 0 to
    # 5.96e-08.
    target_compile_options(DeepCDefocus PRIVATE -mavx2 -mfma -ffp-contract=off)

    option(DEEPC_DEFOCUS_VEC_REPORT
           "Emit GCC vectorizer notes while compiling DeepCDefocus" OFF)
    if (DEEPC_DEFOCUS_VEC_REPORT)
        target_compile_options(DeepCDefocus PRIVATE
                               -fopt-info-vec-optimized -fopt-info-vec-missed)
    endif()

    # Measurement-only: re-enable FMA contraction on the scatter TU alone, so
    # the cost of the -ffp-contract=off above can be priced rather than assumed.
    # Later flags win in GCC, so this overrides the target option on this file.
    option(DEEPC_DEFOCUS_SCATTER_FP_CONTRACT
           "Allow FMA contraction in DeepCDefocusScatter.cpp (measurement only)"
           OFF)
    if (DEEPC_DEFOCUS_SCATTER_FP_CONTRACT)
        set_source_files_properties(DeepCDefocusScatter.cpp PROPERTIES
                                    COMPILE_OPTIONS "-ffp-contract=fast")
    endif()
endif()

# Link Qt6 into DeepCShuffle2 for ShuffleMatrixKnob widget
if (Qt6_FOUND)
  target_sources(DeepCShuffle2 PRIVATE ShuffleMatrixWidget.h)
  target_sources(DeepCShuffle2 PRIVATE ShuffleMatrixKnob.cpp ShuffleMatrixWidget.cpp)
  target_link_libraries(DeepCShuffle2 PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
endif()

# create menu file
string(REPLACE ";" "\", \"" DRAW_NODES "\"${DRAW_NODES}\"")
string(REPLACE ";" "\", \"" CHANNEL_NODES "\"${CHANNEL_NODES}\"")
string(REPLACE ";" "\", \"" COLOR_NODES "\"${COLOR_NODES}\"")
string(REPLACE ";" "\", \"" 3D_NODES "\"${3D_NODES}\"")
string(REPLACE ";" "\", \"" MERGE_NODES "\"${MERGE_NODES}\"")
string(REPLACE ";" "\", \"" Util_NODES "\"${Util_NODES}\"")
string(REPLACE ";" "\", \"" FILTER_NODES "\"${FILTER_NODES}\"")
configure_file(../python/menu.py.in menu.py)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/menu.py" DESTINATION .)

# install files
install(TARGETS 
        ${PLUGINS} 
        ${PLUGINS_WRAPPED} 
        ${PLUGINS_MWRAPPED} 
        DESTINATION .)
