cmake_minimum_required(VERSION 3.15)

project(4Track VERSION 0.4.0)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add JUCE
add_subdirectory(JUCE)

# Create the GUI application
juce_add_gui_app(4Track
    PRODUCT_NAME "4track"
    COMPANY_NAME "4Track"
    BUNDLE_ID "com.4track.recorder"
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_SYNTH FALSE
    REQUIRES_FULL_SCREEN FALSE
    IS_PLUGIN_SYNTH FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE 4Trk
    PLUGIN_CODE 4Tk1)

# Source files
target_sources(4Track PRIVATE
    Source/Main.cpp
    Source/UI/MainComponent.cpp
    Source/UI/MainComponent.h
    Source/UI/VUMeter.cpp
    Source/UI/VUMeter.h
    Source/UI/TrackStrip.cpp
    Source/UI/TrackStrip.h
    Source/UI/TransportBar.cpp
    Source/UI/TransportBar.h
    Source/Audio/AudioEngine.cpp
    Source/Audio/AudioEngine.h
    Source/Audio/Track.cpp
    Source/Audio/Track.h
    Source/Audio/TrackManager.cpp
    Source/Audio/TrackManager.h
    Source/Audio/TransportController.cpp
    Source/Audio/TransportController.h)

# Preprocessor definitions
target_compile_definitions(4Track PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:4Track,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:4Track,JUCE_VERSION>"
    APP_VERSION="0.4.0"
    APP_BUILD_DATE=__DATE__)

# Link JUCE libraries
target_link_libraries(4Track PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
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
