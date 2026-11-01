cmake_minimum_required(VERSION 3.25)
# Without an explicit target the macOS SDK defaults to (SDK major - 1), which
# would restrict these binaries to the newest macOS. The build is arm64-only,
# so 11.0 (the first Apple Silicon release) is the lowest sensible floor.
# Override with -DCMAKE_OSX_DEPLOYMENT_TARGET=<version>.
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment target" FORCE)
endif()
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FLOWS_VERSION LIMIT_COUNT 1)
project(flows VERSION ${FLOWS_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(CTest)

add_library(flows_core
    src/compiler/program.cpp
    src/engine/engine.cpp
    src/engine/sample.cpp
)
target_include_directories(flows_core PUBLIC src)
set_target_properties(flows_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(flows_core PRIVATE /W4 /fp:precise)
else()
    target_compile_options(flows_core PRIVATE -Wall -Wextra -Wpedantic -fno-fast-math)
endif()

add_executable(flows src/host/offline_main.cpp)
target_link_libraries(flows PRIVATE flows_core)

add_library(flows_session src/host/app/RealtimeSession.cpp src/host/app/Analyzer.cpp)
target_link_libraries(flows_session PUBLIC flows_core)
set_target_properties(flows_session PROPERTIES POSITION_INDEPENDENT_CODE ON)

option(FLOWS_BUILD_APP "Build native Flows standalone, AU and VST3" OFF)
if(FLOWS_BUILD_APP)
    if(NOT APPLE)
        message(FATAL_ERROR "The desktop app currently targets macOS. Configure with FLOWS_BUILD_APP=OFF to build the portable compiler, DSP and tests.")
    endif()
    enable_language(OBJCXX)
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles("#include <thread>\n#include <stop_token>\nint main() { std::jthread worker([](std::stop_token stop) { (void) stop.stop_requested(); }); worker.request_stop(); }" FLOWS_HAS_JTHREAD)
    if(NOT FLOWS_HAS_JTHREAD)
        message(FATAL_ERROR "Flows requires C++20 std::jthread and std::stop_token. On macOS, select Xcode 26 or newer command-line tools.")
    endif()
    set(JUCE_DIR "" CACHE PATH "JUCE 8.0.7 source checkout")
    if(NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "Set JUCE_DIR to a JUCE 8.0.7 checkout")
    endif()
    add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/juce" EXCLUDE_FROM_ALL)
    include(cmake/JuceAdapters.cmake)
    flows_use_juce_adapters("${JUCE_DIR}")
    find_program(NPM_EXECUTABLE npm REQUIRED)
    find_program(NODE_EXECUTABLE node REQUIRED)
    if(BUILD_TESTING)
        add_test(NAME observation_ui COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ui/test/observation.test.mjs)
        add_test(NAME plot_math_ui COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ui/test/plotMath.test.mjs)
        add_test(NAME signal_display_ui COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ui/test/signalDisplay.test.mjs)
        add_test(NAME parameter_drag_ui COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ui/test/parameterDrag.test.mjs)
        add_test(NAME parameter_defaults_ui COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ui/test/parameterDefault.test.mjs)
        add_test(NAME panel_visibility_ui COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ui/test/panelVisibility.test.mjs)
        add_test(NAME probe_selection_ui COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ui/test/probeSelection.test.mjs)
        add_test(NAME documents_ui COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ui/test/documents.test.mjs)
        add_test(NAME choices_ui COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ui/test/choice.test.mjs)
        add_test(NAME completion_ui COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ui/test/completion.test.mjs)
        add_test(NAME completion_compiler COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ui/test/languageCompiler.test.mjs $<TARGET_FILE:flows>)
        add_test(NAME sample_path_ui COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ui/test/samplePath.test.mjs)
        add_test(NAME explorer_ui COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ui/test/explorer.test.mjs)
        add_test(NAME sequencer_ui COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ui/test/sequencer.test.mjs)
        add_test(NAME flow_layout_ui COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ui/test/flowLayout.test.mjs)
        add_test(NAME markdown_ui COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ui/test/markdown.test.mjs)
        add_test(NAME assistant_history_ui COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ui/test/assistantHistory.test.mjs)
    endif()
    add_custom_command(OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/ui/dist/index.html" "${CMAKE_CURRENT_SOURCE_DIR}/ui/dist/assets/index.js" "${CMAKE_CURRENT_SOURCE_DIR}/ui/dist/assets/index.css"
        COMMAND ${NPM_EXECUTABLE} run build WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ui"
        DEPENDS ui/package.json ui/package-lock.json ui/vite.config.ts ui/tsconfig.json ui/src/App.tsx ui/src/Editor.tsx ui/src/Agent.tsx ui/src/Signals.tsx ui/src/Keyboard.tsx ui/src/native.ts ui/src/types.ts ui/src/style.css ui/src/telemetry.ts ui/src/observation.ts ui/src/Icon.tsx ui/src/main.tsx ui/src/Macros.tsx ui/src/Dataflow.tsx ui/src/Dataflow.css ui/src/flowLayout.ts ui/src/ResizeHandle.tsx ui/src/parameterDefault.ts ui/src/parameterDrag.ts ui/src/ParameterInput.ts ui/src/signalDisplay.ts ui/src/signalPlot.ts ui/src/plotMath.ts
        DEPENDS ui/src/panelVisibility.ts ui/src/documents.ts ui/src/probeSelection.ts
        DEPENDS ui/src/Explorer.tsx ui/src/Explorer.css ui/src/explorerTree.ts ui/src/samplePath.ts
        DEPENDS ui/src/ChoiceMenu.tsx ui/src/ChoiceMenu.css ui/src/choice.ts
        DEPENDS ui/src/languageCatalog.ts ui/src/completionModel.ts ui/src/flowsCompletion.ts
        DEPENDS ui/src/StepSequencer.tsx ui/src/StepSequencer.css ui/src/sequencer.ts
        DEPENDS ui/src/Agent.css ui/src/AgentActivity.tsx ui/src/assistantHistory.ts ui/src/Markdown.tsx ui/src/Markdown.css
        COMMENT "Building Flows editor")
    juce_add_binary_data(FlowsAssets NAMESPACE FlowsAssets HEADER_NAME FlowsAssets.h SOURCES
        ui/dist/index.html ui/dist/assets/index.js ui/dist/assets/index.css
        test/presets/sine-reference.flows test/presets/probe-lab.flows
        test/presets/mono-legato-glide.flows test/presets/trance-rolling-bass.flows
        test/presets/stereo-motion.flows test/presets/sample-one-shot.flows test/presets/sample-loop.flows
        test/fixtures/samples/flows-kick.wav test/fixtures/samples/flows-stereo.wav docs/LANGUAGE.md)
    juce_add_plugin(FlowsPlugin
        COMPANY_NAME "Flows Audio" BUNDLE_ID "audio.flows.instrument"
        PLUGIN_MANUFACTURER_CODE Flwa PLUGIN_CODE Flw1
        FORMATS AU VST3 PRODUCT_NAME "Flows" VERSION "${PROJECT_VERSION}"
        IS_SYNTH TRUE NEEDS_MIDI_INPUT TRUE NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        COPY_PLUGIN_AFTER_BUILD FALSE AU_MAIN_TYPE kAudioUnitType_MusicDevice
        VST3_CATEGORIES Instrument Synth)
    target_sources(FlowsPlugin PRIVATE src/host/app/SampleAssets.cpp src/host/app/AssistantHistory.cpp src/host/app/FlowsProcessor.cpp src/host/app/LiveCompanion.cpp src/host/app/FlowsEditor.cpp src/host/app/AIService.cpp src/host/app/AIModels.cpp src/host/app/AISecrets.mm src/host/app/MacAppearance.mm)
    target_link_libraries(FlowsPlugin PRIVATE flows_session FlowsAssets "-framework Security"
        juce::juce_audio_utils juce::juce_gui_extra juce::juce_dsp juce::juce_cryptography
        PUBLIC juce::juce_recommended_config_flags)
    target_compile_definitions(FlowsPlugin PUBLIC JUCE_WEB_BROWSER=1 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
    get_target_property(_flows_public_definitions FlowsPlugin INTERFACE_COMPILE_DEFINITIONS)
    list(FILTER _flows_public_definitions EXCLUDE REGEX "^JUCE_STANDALONE_APPLICATION=")
    set_property(TARGET FlowsPlugin PROPERTY INTERFACE_COMPILE_DEFINITIONS "${_flows_public_definitions}")
    target_include_directories(FlowsPlugin PUBLIC src src/host/app "${JUCE_DIR}/modules")
    # JUCE's CXX visibility does not cover Objective-C++ translation units.
    # A host and plugin must not coalesce JUCE's per-image String/var statics.
    set_target_properties(FlowsPlugin FlowsPlugin_AU FlowsPlugin_VST3 PROPERTIES
        CXX_VISIBILITY_PRESET hidden OBJCXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN TRUE)
    if(APPLE)
        target_link_options(FlowsPlugin_AU PRIVATE
            "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/cmake/au.exports")
        target_link_options(FlowsPlugin_VST3 PRIVATE
            "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/cmake/vst3.exports")
        set_property(TARGET FlowsPlugin_AU APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/au.exports")
        set_property(TARGET FlowsPlugin_VST3 APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vst3.exports")
    endif()
    set(FLOWS_STANDALONE_NAME "Flows" CACHE STRING "Standalone application display name")
    juce_add_gui_app(FlowsStandalone PRODUCT_NAME "${FLOWS_STANDALONE_NAME}" COMPANY_NAME "Flows Audio" BUNDLE_ID "audio.flows.standalone" VERSION "${PROJECT_VERSION}"
        ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/flows-icon.png"
        ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/assets/flows-icon.png")
    target_sources(FlowsStandalone PRIVATE src/host/app/StandaloneApp.cpp)
    target_compile_definitions(FlowsStandalone PRIVATE FLOWS_STANDALONE_NAME="${FLOWS_STANDALONE_NAME}")
    target_link_libraries(FlowsStandalone PRIVATE FlowsPlugin)
    if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/cpp/host_tests.cpp")
        juce_add_console_app(AssistantHistoryTests PRODUCT_NAME "Flows Assistant History Tests")
        target_sources(AssistantHistoryTests PRIVATE test/cpp/assistant_history_tests.cpp src/host/app/AssistantHistory.cpp)
        target_include_directories(AssistantHistoryTests PRIVATE src/host/app)
        target_link_libraries(AssistantHistoryTests PRIVATE juce::juce_core)
        target_compile_definitions(AssistantHistoryTests PRIVATE JUCE_USE_CURL=0)
        add_test(NAME assistant_history COMMAND AssistantHistoryTests)
        juce_add_console_app(LiveCompanionTests PRODUCT_NAME "Flows Live Companion Tests")
        target_sources(LiveCompanionTests PRIVATE test/cpp/live_companion_tests.cpp)
        target_link_libraries(LiveCompanionTests PRIVATE FlowsPlugin)
        add_test(NAME live_companion COMMAND LiveCompanionTests)
        juce_add_console_app(MidiInputTests PRODUCT_NAME "Flows MIDI Input Tests")
        target_sources(MidiInputTests PRIVATE test/cpp/midi_input_tests.cpp)
        target_link_libraries(MidiInputTests PRIVATE FlowsPlugin)
        add_test(NAME midi_input COMMAND MidiInputTests)
        juce_add_console_app(HostTests PRODUCT_NAME "Flows Host Tests")
        target_sources(HostTests PRIVATE test/cpp/host_tests.cpp)
        target_link_libraries(HostTests PRIVATE FlowsPlugin)
        juce_add_console_app(SampleHostTests PRODUCT_NAME "Flows Sample Host Tests")
        target_sources(SampleHostTests PRIVATE test/cpp/sample_host_tests.cpp)
        target_link_libraries(SampleHostTests PRIVATE FlowsPlugin)
        juce_add_console_app(SequencerHostTests PRODUCT_NAME "Flows Sequencer Host Tests")
        target_sources(SequencerHostTests PRIVATE test/cpp/sequencer_host_tests.cpp)
        target_link_libraries(SequencerHostTests PRIVATE FlowsPlugin)
        juce_add_console_app(PluginTests PRODUCT_NAME "Flows Plugin Tests")
        target_sources(PluginTests PRIVATE test/cpp/plugin_tests.cpp)
        target_include_directories(PluginTests PRIVATE "${JUCE_DIR}/modules/juce_audio_processors/format_types/VST3_SDK")
        target_link_libraries(PluginTests PRIVATE FlowsPlugin "-framework CoreFoundation" "-framework AudioToolbox")
        add_dependencies(PluginTests FlowsPlugin_VST3 FlowsPlugin_AU)
        juce_add_gui_app(WindowTests PRODUCT_NAME "Flows Window Tests" COMPANY_NAME "Flows Audio"
            BUNDLE_ID "audio.flows.standalone" VERSION "${PROJECT_VERSION}")
        target_sources(WindowTests PRIVATE test/cpp/window_tests.mm)
        target_link_libraries(WindowTests PRIVATE FlowsPlugin "-framework WebKit" "-framework AppKit")
    endif()
endif()

if(BUILD_TESTING)
    add_executable(engine_benchmark test/cpp/engine_benchmark.cpp)
    target_link_libraries(engine_benchmark PRIVATE flows_core)
    add_executable(compiler_tests test/cpp/compiler_tests.cpp)
    target_link_libraries(compiler_tests PRIVATE flows_core)
    add_test(NAME compiler COMMAND compiler_tests)

    add_executable(developer_tests test/cpp/developer_tests.cpp)
    target_link_libraries(developer_tests PRIVATE flows_session)
    add_test(NAME developer COMMAND developer_tests)

    add_executable(sequencer_tests test/cpp/sequencer_tests.cpp)
    target_link_libraries(sequencer_tests PRIVATE flows_core)
    add_test(NAME sequencer COMMAND sequencer_tests)

    foreach(sample_test IN ITEMS compiler engine file)
        add_executable(sample_${sample_test}_tests test/cpp/sample_${sample_test}_tests.cpp)
        target_link_libraries(sample_${sample_test}_tests PRIVATE flows_core)
        add_test(NAME sample_${sample_test} COMMAND sample_${sample_test}_tests)
    endforeach()

    add_executable(engine_tests test/cpp/engine_tests.cpp)
    target_link_libraries(engine_tests PRIVATE flows_core)
    add_test(NAME engine COMMAND engine_tests)
    add_executable(boundary_tests test/cpp/boundary_tests.cpp)
    target_link_libraries(boundary_tests PRIVATE flows_core)
    add_test(NAME audio_boundaries COMMAND boundary_tests)
    find_package(Threads REQUIRED)
    add_executable(host_queue_tests test/cpp/host_queue_tests.cpp)
    target_link_libraries(host_queue_tests PRIVATE flows_core Threads::Threads)
    add_test(NAME host_queues COMMAND host_queue_tests)
    set_tests_properties(host_queues PROPERTIES TIMEOUT 15)

    foreach(test_name IN ITEMS analyzer session sample_session sample_replay)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/cpp/${test_name}_tests.cpp")
            add_executable(${test_name}_tests test/cpp/${test_name}_tests.cpp)
            target_link_libraries(${test_name}_tests PRIVATE flows_session)
            add_test(NAME ${test_name} COMMAND ${test_name}_tests)
        endif()
    endforeach()

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    if(TARGET HostTests)
        add_test(NAME sample_host COMMAND SampleHostTests)
        set_tests_properties(sample_host PROPERTIES TIMEOUT 120)
        add_test(NAME sequencer_host COMMAND SequencerHostTests)
        set_tests_properties(sequencer_host PROPERTIES TIMEOUT 90)
        add_test(NAME host_integration COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test/tools/test_ai_endpoint.py --binary $<TARGET_FILE:HostTests>)
        set_tests_properties(host_integration PROPERTIES TIMEOUT 120)
        # Native wrappers contain separate JUCE globals; exercise each format
        # in its own host process rather than loading both implementations together.
        add_test(NAME plugin_hosts_vst3 COMMAND PluginTests
            --vst3 "${CMAKE_BINARY_DIR}/FlowsPlugin_artefacts/${CMAKE_BUILD_TYPE}/VST3/Flows.vst3")
        add_test(NAME plugin_hosts_au COMMAND PluginTests
            --au "${CMAKE_BINARY_DIR}/FlowsPlugin_artefacts/${CMAKE_BUILD_TYPE}/AU/Flows.component")
        set_tests_properties(plugin_hosts_vst3 plugin_hosts_au PROPERTIES RESOURCE_LOCK flows_wrapper_workspace)
        add_test(NAME editor_layout COMMAND WindowTests --editor-layout-only --timeout-ms=30000)
        set_tests_properties(editor_layout PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE PASS_REGULAR_EXPRESSION "Window integration passed:" FAIL_REGULAR_EXPRESSION "Window integration FAILED:")
        add_test(NAME window_startup COMMAND WindowTests --timeout-ms=120000 "--snapshot=${CMAKE_BINARY_DIR}/window-tests.png")
        set_tests_properties(window_startup PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE PASS_REGULAR_EXPRESSION "Window integration passed:" FAIL_REGULAR_EXPRESSION "Window integration FAILED:")
    endif()
    add_test(NAME publication_tools
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/tools/test_publication_tools.py)
    add_test(NAME fixtures
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/tools/validate_fixtures.py)
    add_test(NAME sample_pipeline
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/tools/test_sample_pipeline.py
            --binary $<TARGET_FILE:flows>)
    add_test(NAME stereo_pipeline
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/tools/test_stereo_pipeline.py
            --binary $<TARGET_FILE:flows>)
    add_test(NAME sine_pipeline
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/tools/test_sine_pipeline.py
            --binary $<TARGET_FILE:flows>)
endif()
