cmake_minimum_required(VERSION 3.22)
project(Morph VERSION 1.0.0)

# JUCE_PATH can be overridden via -DJUCE_PATH=... on the command line
if(NOT DEFINED JUCE_PATH)
    set(JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE")
endif()
add_subdirectory("${JUCE_PATH}" "${CMAKE_BINARY_DIR}/JUCE")

juce_add_plugin(Morph
    COMPANY_NAME                "Water"
    BUNDLE_ID                   "ai.95ent.morph"
    PLUGIN_MANUFACTURER_CODE    WATR
    PLUGIN_CODE                 Mrph
    FORMATS                     AU VST3
    PRODUCT_NAME                "Morph"
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    DESCRIPTION                 "Water catalog browser and real-time morphing plugin"
    VST3_CATEGORIES             "Fx"
    AU_MAIN_TYPE                "aufx"
)

target_sources(Morph PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/LoginComponent.cpp
    Source/LibraryComponent.cpp
    Source/DropZoneComponent.cpp
    Source/WaterAPI.cpp
    Source/MorphEngine.cpp
    Source/LocalDetector.cpp
    Source/CompanionLink.cpp
)

target_compile_definitions(Morph PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
)

target_include_directories(Morph PRIVATE Libs)

target_link_libraries(Morph
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_plugin_client
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
