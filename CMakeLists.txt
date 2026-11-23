cmake_minimum_required(VERSION 3.15)

project(8Track VERSION 0.7.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(JUCE)

# ---------------------------------------------------------------------------
# App
# ---------------------------------------------------------------------------
juce_add_gui_app(8Track
    PRODUCT_NAME "8track"
    COMPANY_NAME "8track"
    BUNDLE_ID "com.8track.recorder"
    VERSION ${PROJECT_VERSION}
    ICON_BIG   "${CMAKE_CURRENT_SOURCE_DIR}/Assets/8track.icns"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Assets/8track.icns"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "8track needs microphone access to record audio.")

file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h)
target_sources(8Track PRIVATE ${APP_SOURCES})

target_compile_definitions(8Track PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:8Track,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:8Track,JUCE_VERSION>")

target_link_libraries(8Track
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# macOS only shows the microphone prompt (and only delivers real input) to a signed app.
# Intel toolchains don't sign automatically, so sign ad hoc after every build.
if(APPLE)
    add_custom_command(TARGET 8Track POST_BUILD
        COMMAND codesign --force --deep --sign - --identifier com.8track.recorder
                "$<TARGET_BUNDLE_DIR:8Track>"
        COMMENT "Ad-hoc signing 8track.app")
endif()

# ---------------------------------------------------------------------------
# Tests: the model and mixing code, no audio device needed.
#   cmake --build build --target 8TrackTests && ./build/8TrackTests_artefacts/Debug/8TrackTests
# ---------------------------------------------------------------------------
juce_add_console_app(8TrackTests PRODUCT_NAME "8TrackTests")

target_sources(8TrackTests PRIVATE
    Tests/TestMain.cpp
    Source/Model/Clip.cpp
    Source/Model/Arrangement.cpp
    Source/Audio/MixEngine.cpp
    Source/Audio/AudioEngine.cpp
    Source/Project/AudioFileIO.cpp
    Source/Project/Project.cpp)

target_compile_definitions(8TrackTests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_UNIT_TESTS=1)

target_link_libraries(8TrackTests
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_core
        juce::juce_events
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
