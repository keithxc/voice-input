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

add_library(voice-input-common STATIC src/protocol.c)
target_include_directories(voice-input-common PUBLIC src)
target_compile_options(voice-input-common PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion>)

add_executable(voice-inputd src/daemon.c src/audio.c src/asr.c src/output.c)
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
add_library(voice-input-fcitx5 MODULE fcitx5/voice_input_addon.cpp)
target_link_libraries(voice-input-fcitx5 PRIVATE Fcitx5::Core Fcitx5::Utils)
install(TARGETS voice-input-fcitx5 DESTINATION "${FCITX_INSTALL_ADDONDIR}")
install(FILES fcitx5/voice-input.conf DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon")
install(FILES data/voice-inputd.service
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/user)

if(BUILD_TESTING)
  find_program(BASH_PROGRAM bash REQUIRED)
  add_executable(protocol-test tests/protocol_test.c)
  target_link_libraries(protocol-test PRIVATE voice-input-common)
  add_test(NAME protocol COMMAND protocol-test)
  add_test(NAME integration
    COMMAND "${BASH_PROGRAM}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.sh"
            "$<TARGET_FILE:voice-inputd>" "$<TARGET_FILE:voice-inputctl>")
  if(VOICE_INPUT_TEST_MODEL_DIR)
    add_executable(asr-test tests/asr_test.c src/asr.c)
    target_include_directories(asr-test PRIVATE src "${SHERPA_ONNX_INCLUDE_DIR}")
    target_link_libraries(asr-test PRIVATE "${SHERPA_ONNX_C_API}")
    add_test(NAME asr-model COMMAND asr-test "${VOICE_INPUT_TEST_MODEL_DIR}")
    set_tests_properties(asr-model PROPERTIES TIMEOUT 60)
  endif()
endif()
