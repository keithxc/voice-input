cmake_minimum_required(VERSION 3.20)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" VOICE_INPUT_VERSION LIMIT_COUNT 1)
project(voice-input VERSION "${VOICE_INPUT_VERSION}" LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CTest)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3>=0.3.65)
find_path(SHERPA_ONNX_INCLUDE_DIR sherpa-onnx/c-api/c-api.h REQUIRED)
find_library(SHERPA_ONNX_C_API sherpa-onnx-c-api REQUIRED)
find_package(Fcitx5Core 5.0 REQUIRED)
find_package(Fcitx5Utils REQUIRED)
find_package(LayerShellQt REQUIRED)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Network Qml Quick Test)

qt_standard_project_setup(REQUIRES 6.5)

add_library(voice-input-common STATIC src/protocol.c src/score.c src/selection.c
  src/text.c)
target_include_directories(voice-input-common PUBLIC src)
target_compile_options(voice-input-common PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion>)

add_executable(voice-inputd src/daemon.c src/audio.c src/asr.c src/output.c
  src/punctuation.c)
target_include_directories(voice-inputd PRIVATE "${SHERPA_ONNX_INCLUDE_DIR}")
target_link_libraries(voice-inputd PRIVATE voice-input-common PkgConfig::PIPEWIRE
  "${SHERPA_ONNX_C_API}" m)
target_compile_definitions(voice-inputd PRIVATE VOICE_INPUT_VERSION="${PROJECT_VERSION}")
target_compile_options(voice-inputd PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion>)

add_executable(voice-inputctl src/ctl.c)
target_link_libraries(voice-inputctl PRIVATE voice-input-common)
target_compile_definitions(voice-inputctl PRIVATE VOICE_INPUT_VERSION="${PROJECT_VERSION}")
target_compile_options(voice-inputctl PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion>)

install(TARGETS voice-inputd voice-inputctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

qt_add_executable(voice-input-overlay
  overlay/main.cpp
  overlay/overlay_model.cpp
  overlay/overlay_model.h)
target_link_libraries(voice-input-overlay PRIVATE
  LayerShellQt::Interface Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick)
set_source_files_properties(overlay/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)
qt_add_qml_module(voice-input-overlay
  URI VoiceInput
  VERSION 1.0
  QML_FILES overlay/Main.qml)
install(TARGETS voice-input-overlay
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_library(voice-input-fcitx5 MODULE fcitx5/voice_input_addon.cpp)
target_link_libraries(voice-input-fcitx5 PRIVATE Fcitx5::Core Fcitx5::Utils)
install(TARGETS voice-input-fcitx5 DESTINATION "${FCITX_INSTALL_ADDONDIR}")
install(FILES fcitx5/voice-input.conf DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon")
install(FILES data/voice-inputd.service data/voice-input-overlay.service
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/user)

if(BUILD_TESTING)
  find_program(BASH_PROGRAM bash REQUIRED)
  add_executable(protocol-test tests/protocol_test.c)
  target_link_libraries(protocol-test PRIVATE voice-input-common)
  add_test(NAME protocol COMMAND protocol-test)
  add_executable(score-test tests/score_test.c)
  target_link_libraries(score-test PRIVATE voice-input-common m)
  add_test(NAME score COMMAND score-test)
  add_executable(punctuation-test tests/punctuation_test.c src/punctuation.c)
  target_include_directories(punctuation-test PRIVATE src "${SHERPA_ONNX_INCLUDE_DIR}")
  target_link_libraries(punctuation-test PRIVATE voice-input-common
    "${SHERPA_ONNX_C_API}")
  add_test(NAME punctuation COMMAND punctuation-test)
  add_executable(selection-test tests/selection_test.c)
  target_link_libraries(selection-test PRIVATE voice-input-common)
  add_test(NAME selection COMMAND selection-test)
  add_test(NAME integration
    COMMAND "${BASH_PROGRAM}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.sh"
            "$<TARGET_FILE:voice-inputd>" "$<TARGET_FILE:voice-inputctl>")
  add_executable(audio-gain-test tests/audio_gain_test.c src/audio.c)
  target_include_directories(audio-gain-test PRIVATE src)
  target_link_libraries(audio-gain-test PRIVATE voice-input-common
    PkgConfig::PIPEWIRE m)
  add_test(NAME audio-gain COMMAND audio-gain-test)
  qt_add_executable(overlay-model-test
    tests/overlay_model_test.cpp
    overlay/overlay_model.cpp
    overlay/overlay_model.h)
  target_include_directories(overlay-model-test PRIVATE overlay)
  target_link_libraries(overlay-model-test PRIVATE
    Qt6::Core Qt6::Network Qt6::Test)
  add_test(NAME overlay-model COMMAND overlay-model-test)
  file(MAKE_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/test-cache"
    "${CMAKE_CURRENT_BINARY_DIR}/test-home"
    "${CMAKE_CURRENT_BINARY_DIR}/test-runtime")
  file(CHMOD "${CMAKE_CURRENT_BINARY_DIR}/test-runtime"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
  set(overlay_test_environment
    "HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home"
    "QT_QPA_PLATFORM=offscreen"
    "QT_QUICK_BACKEND=software"
    "XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-cache"
    "XDG_RUNTIME_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-runtime")
  if(VOICE_INPUT_TEST_FONTCONFIG)
    list(APPEND overlay_test_environment
      "FONTCONFIG_FILE=${VOICE_INPUT_TEST_FONTCONFIG}")
  endif()
  if(VOICE_INPUT_TEST_QML_IMPORT_PATH)
    list(APPEND overlay_test_environment
      "QML2_IMPORT_PATH=${VOICE_INPUT_TEST_QML_IMPORT_PATH}")
  endif()
  if(VOICE_INPUT_TEST_QT_PLUGIN_PATH)
    list(APPEND overlay_test_environment
      "QT_PLUGIN_PATH=${VOICE_INPUT_TEST_QT_PLUGIN_PATH}")
  endif()
  add_test(NAME overlay-qml-smoke
    COMMAND "${CMAKE_COMMAND}" -E env
      ${overlay_test_environment}
      "$<TARGET_FILE:voice-input-overlay>"
      --demo --screenshot "${CMAKE_CURRENT_BINARY_DIR}/overlay-smoke.png")
  set_tests_properties(overlay-qml-smoke PROPERTIES TIMEOUT 10)
  if(VOICE_INPUT_TEST_MODEL_DIR)
    add_executable(asr-test tests/asr_test.c src/asr.c src/audio.c)
    target_include_directories(asr-test PRIVATE src "${SHERPA_ONNX_INCLUDE_DIR}")
    target_link_libraries(asr-test PRIVATE voice-input-common
      "${SHERPA_ONNX_C_API}" PkgConfig::PIPEWIRE m)
    add_test(NAME asr-model COMMAND asr-test "${VOICE_INPUT_TEST_MODEL_DIR}")
    add_test(NAME asr-quiet-model
      COMMAND asr-test "${VOICE_INPUT_TEST_MODEL_DIR}" --quiet)
    set_tests_properties(asr-model PROPERTIES TIMEOUT 60)
    set_tests_properties(asr-quiet-model PROPERTIES TIMEOUT 60)
  endif()
endif()
