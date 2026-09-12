cmake_minimum_required(VERSION 3.22)

project(OrchCapture VERSION 0.1.0)

# Path to your JUCE folder (same as the rest of the Orch family).
add_subdirectory("C:/JUCE/JUCE" JUCE)

juce_add_plugin(OrchCapture
    COMPANY_NAME "JohnPascu"
    PLUGIN_MANUFACTURER_CODE Jpas
    PLUGIN_CODE Ocap

    FORMATS VST3 Standalone

    PRODUCT_NAME "OrchCapture"
    PLUGIN_NAME "OrchCapture"
    BUNDLE_ID "com.johnpascu.OrchCapture"

    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT TRUE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_REPLACES_VST2 FALSE

    # Goes on ~25 instrument tracks - convenient to have it land in the shared
    # VST3 folder automatically. Flip to FALSE if your build shell is not
    # elevated (the post-build copy into C:/Program Files/Common Files/VST3
    # needs write permission there) and copy the .vst3 by hand instead.
    COPY_PLUGIN_AFTER_BUILD FALSE
)

juce_generate_juce_header(OrchCapture)

target_sources(OrchCapture
    PRIVATE
        Source/OrchCaptureProcessor.cpp
        Source/OrchCaptureEditor.cpp
        Source/OrchCaptureTakeLogic.cpp
        Source/OrchCaptureLink.cpp
)

target_compile_definitions(OrchCapture
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(OrchCapture
    PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_gui_extra

    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

if (TARGET OrchCapture_VST3)
    target_compile_definitions(OrchCapture_VST3
        PRIVATE
            JUCE_VST3_CAN_REPLACE_VST2=0
    )
endif()

# Developer-only verification target for the take -> SMF logic. Pure: no
# GUI, no plugin host, just the note-buffer -> MidiFile transform.
juce_add_console_app(OrchCaptureTakeLogicCheck
    PRODUCT_NAME "OrchCaptureTakeLogicCheck"
)

juce_generate_juce_header(OrchCaptureTakeLogicCheck)

target_sources(OrchCaptureTakeLogicCheck
    PRIVATE
        Tools/OrchCaptureTakeLogicCheck.cpp
        Source/OrchCaptureTakeLogic.cpp
)

target_link_libraries(OrchCaptureTakeLogicCheck
    PRIVATE
        juce::juce_audio_basics

    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

target_include_directories(OrchCaptureTakeLogicCheck
    PRIVATE
        Source
)
