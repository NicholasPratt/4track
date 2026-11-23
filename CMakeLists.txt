cmake_minimum_required(VERSION 3.22)

project(8Track VERSION 0.7.0)

set(CMAKE_CXX_STANDARD 20)   # Tracktion Engine requires C++20
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
endif()

# Tracktion Engine (git submodule) and the JUCE version it is pinned to.
#   git submodule update --init --recursive
add_subdirectory(tracktion_engine/modules/juce)
add_subdirectory(tracktion_engine/modules)

# ---------------------------------------------------------------------------
# App
# ---------------------------------------------------------------------------
juce_add_gui_app(8Track
    PRODUCT_NAME "8track"
    COMPANY_NAME "8track"
    BUNDLE_ID "com.8track.recorder"
    VERSION ${PROJECT_VERSION}
    ICON_BIG   "${CMAKE_CURRENT_SOURCE_DIR}/Assets/8track.iconset/icon_512x512@2x.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Assets/8track.iconset/icon_32x32.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "8track needs microphone access to record audio.")

file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h)
target_sources(8Track PRIVATE ${APP_SOURCES})

# Settings shared by the app and the tests (both build Tracktion Engine).
function(configure_8track_target target)
    target_compile_definitions(${target} PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_PLUGINHOST_AU=1
        JUCE_PLUGINHOST_VST3=1
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_STRICT_REFCOUNTEDPOINTER=1)

    target_link_libraries(${target}
        PRIVATE
            tracktion::tracktion_core
            tracktion::tracktion_engine
            tracktion::tracktion_graph
            juce::juce_audio_devices
            juce::juce_audio_processors
            juce::juce_audio_utils
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    set_target_properties(${target} PROPERTIES C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)
    if(APPLE)
        target_compile_options(${target} PRIVATE -fno-aligned-allocation)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${target} PRIVATE -Wno-c2y-extensions)   # noise from Tracktion headers on clang 22
    endif()
endfunction()

configure_8track_target(8Track)
target_compile_definitions(8Track PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:8Track,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:8Track,JUCE_VERSION>")
target_link_libraries(8Track PRIVATE juce::juce_recommended_lto_flags)

# macOS only shows the microphone prompt (and only delivers real input) to a signed app.
# JUCE builds AppIcon.icns from the PNGs above; replace it with the hand-made .icns (every size
# drawn properly), then sign ad hoc, since Intel toolchains don't sign automatically.
if(APPLE)
    add_custom_command(TARGET 8Track POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/Assets/8track.icns"
                "$<TARGET_BUNDLE_CONTENT_DIR:8Track>/Resources/AppIcon.icns"
        COMMAND codesign --force --deep --sign - --identifier com.8track.recorder
                "$<TARGET_BUNDLE_DIR:8Track>"
        COMMENT "Ad-hoc signing 8track.app")
endif()

# ---------------------------------------------------------------------------
# Tests: the arrangement model, and Project/AudioEngine against a real Tracktion Edit.
#   cmake --build build --target 8TrackTests && ./build/8TrackTests_artefacts/Debug/8TrackTests
# ---------------------------------------------------------------------------
juce_add_console_app(8TrackTests PRODUCT_NAME "8TrackTests")

target_sources(8TrackTests PRIVATE
    Tests/TestMain.cpp
    Source/Model/Arrangement.cpp
    Source/Audio/AudioEngine.cpp
    Source/Project/Project.cpp)

configure_8track_target(8TrackTests)
target_compile_definitions(8TrackTests PRIVATE JUCE_UNIT_TESTS=1)
