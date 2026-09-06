cmake_minimum_required(VERSION 3.20)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" VOICE_INPUT_VERSION LIMIT_COUNT 1)
project(voice-input VERSION "${VOICE_INPUT_VERSION}" LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CTest)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3>=0.3.65)

add_library(voice-input-common STATIC src/protocol.c)
target_include_directories(voice-input-common PUBLIC src)
target_compile_options(voice-input-common PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion>)

add_executable(voice-inputd src/daemon.c src/audio.c)
target_link_libraries(voice-inputd PRIVATE voice-input-common PkgConfig::PIPEWIRE m)
target_compile_definitions(voice-inputd PRIVATE VOICE_INPUT_VERSION="${PROJECT_VERSION}")
target_compile_options(voice-inputd PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion>)

add_executable(voice-inputctl src/ctl.c)
target_link_libraries(voice-inputctl PRIVATE voice-input-common)
target_compile_definitions(voice-inputctl PRIVATE VOICE_INPUT_VERSION="${PROJECT_VERSION}")
target_compile_options(voice-inputctl PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion>)

install(TARGETS voice-inputd voice-inputctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
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
endif()
